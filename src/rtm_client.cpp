#include "rtm_client.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/regex.hpp>
#include <boost/variant.hpp>
#include <gsl/gsl>
#include <json.hpp>
#include <memory>
#include <queue>
#include <unordered_map>

#include "cbor_json.h"
#include "logging.h"
#include "metrics.h"
#include "threadutils.h"

namespace asio = boost::asio;

namespace satori {

namespace video {

namespace rtm {

using endpoint_iterator_t = asio::ip::tcp::resolver::iterator;
using endpoint_t = asio::ip::tcp::resolver::endpoint_type;

struct client_error_category : std::error_category {
  const char *name() const noexcept override { return "rtm-client"; }
  std::string message(int ev) const override {
    switch (static_cast<client_error>(ev)) {
      case client_error::UNKNOWN:
        return "unknown error";
      case client_error::NOT_CONNECTED:
        return "client is not connected";
      case client_error::RESPONSE_PARSING_ERROR:
        return "error parsing response";
      case client_error::INVALID_RESPONSE:
        return "invalid response";
      case client_error::SUBSCRIPTION_ERROR:
        return "subscription error";
      case client_error::SUBSCRIBE_ERROR:
        return "subscribe error";
      case client_error::UNSUBSCRIBE_ERROR:
        return "unsubscribe error";
      case client_error::ASIO_ERROR:
        return "asio error";
      case client_error::INVALID_MESSAGE:
        return "invalid message";
      case client_error::PUBLISH_ERROR:
        return "publish error";
    }
  }
};

std::error_condition make_error_condition(client_error e) {
  static client_error_category category;
  return {static_cast<int>(e), category};
}

namespace {

constexpr int read_buffer_size = 100000;
constexpr bool use_cbor = true;

const boost::posix_time::seconds ws_ping_interval{1};

const std::vector<double> latency_buckets{
    0,    1,    2,    3,    4,    5,     6,     7,     8,     9,    10,   15,
    20,   25,   30,   40,   50,   60,    70,    80,    90,    100,  150,  200,
    250,  300,  400,  500,  600,  700,   800,   900,   1000,  2000, 3000, 4000,
    5000, 6000, 7000, 8000, 9000, 10000, 25000, 50000, 100000};

auto &rtm_client_start = prometheus::BuildCounter()
                             .Name("rtm_client_start")
                             .Register(metrics_registry())
                             .Add({});

auto &rtm_client_error =
    prometheus::BuildCounter().Name("rtm_client_error").Register(metrics_registry());

auto &rtm_actions_received = prometheus::BuildCounter()
                                 .Name("rtm_actions_received_total")
                                 .Register(metrics_registry());

auto &rtm_messages_received = prometheus::BuildCounter()
                                  .Name("rtm_messages_received_total")
                                  .Register(metrics_registry());

auto &rtm_messages_bytes_received = prometheus::BuildCounter()
                                        .Name("rtm_messages_received_bytes_total")
                                        .Register(metrics_registry());

auto &rtm_messages_sent = prometheus::BuildCounter()
                              .Name("rtm_messages_sent_total")
                              .Register(metrics_registry());

auto &rtm_messages_bytes_sent = prometheus::BuildCounter()
                                    .Name("rtm_messages_sent_bytes_total")
                                    .Register(metrics_registry());

auto &rtm_messages_in_pdu =
    prometheus::BuildHistogram()
        .Name("rtm_messages_in_pdu")
        .Register(metrics_registry())
        .Add({}, std::vector<double>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 20, 30, 40, 50, 60,
                                     70, 80, 90, 100});

auto &rtm_bytes_written = prometheus::BuildCounter()
                              .Name("rtm_bytes_written_total")
                              .Register(metrics_registry())
                              .Add({});

auto &rtm_bytes_read = prometheus::BuildCounter()
                           .Name("rtm_bytes_read_total")
                           .Register(metrics_registry())
                           .Add({});

auto &rtm_pings_sent_total = prometheus::BuildCounter()
                                 .Name("rtm_pings_sent_total")
                                 .Register(metrics_registry())
                                 .Add({});

auto &rtm_frames_received_total = prometheus::BuildCounter()
                                      .Name("rtm_frames_received_total")
                                      .Register(metrics_registry());

auto &rtm_last_pong_time_seconds = prometheus::BuildGauge()
                                       .Name("rtm_last_pong_time_seconds")
                                       .Register(metrics_registry())
                                       .Add({});

auto &rtm_last_ping_time_seconds = prometheus::BuildGauge()
                                       .Name("rtm_last_ping_time_seconds")
                                       .Register(metrics_registry())
                                       .Add({});

auto &rtm_ping_latency_millis = prometheus::BuildHistogram()
                                    .Name("rtm_ping_latency_millis")
                                    .Register(metrics_registry())
                                    .Add({}, latency_buckets);

auto &rtm_pending_requests = prometheus::BuildGauge()
                                 .Name("rtm_pending_requests")
                                 .Register(metrics_registry())
                                 .Add({});

auto &rtm_subscription_error_total = prometheus::BuildCounter()
                                         .Name("rtm_subscription_error_total")
                                         .Register(metrics_registry())
                                         .Add({});

auto &rtm_write_delay_microseconds =
    prometheus::BuildHistogram()
        .Name("rtm_write_delay_microseconds")
        .Register(metrics_registry())
        .Add({}, std::vector<double>{0,    1,    5,     10,    25,    50,    100,
                                     250,  500,  750,   1000,  2000,  3000,  4000,
                                     5000, 7500, 10000, 25000, 50000, 100000});

auto &rtm_publish_ack_latency_millis = prometheus::BuildHistogram()
                                           .Name("rtm_publish_ack_latency_millis")
                                           .Register(metrics_registry())
                                           .Add({}, latency_buckets);

auto &rtm_publish_error_total = prometheus::BuildCounter()
                                    .Name("rtm_publish_error_total")
                                    .Register(metrics_registry())
                                    .Add({});

auto &rtm_publish_inflight_total = prometheus::BuildGauge()
                                       .Name("rtm_publish_inflight_total")
                                       .Register(metrics_registry())
                                       .Add({});

auto &rtm_subscribe_error_total = prometheus::BuildCounter()
                                      .Name("rtm_subscribe_error_total")
                                      .Register(metrics_registry())
                                      .Add({});

auto &rtm_unsubscribe_error_total = prometheus::BuildCounter()
                                        .Name("rtm_unsubscribe_error_total")
                                        .Register(metrics_registry())
                                        .Add({});

// TODO: convert to function
struct subscribe_request {
  const uint64_t id;
  const std::string channel;
  boost::optional<uint64_t> age;
  boost::optional<uint64_t> count;

  nlohmann::json to_json() const {
    nlohmann::json document =
        R"({"action":"rtm/subscribe", "body":{"channel":"<not_set>", "subscription_id":"<not_set>"}, "id": 2})"_json;

    CHECK(document.is_object());
    document["id"] = id;
    auto &body = document["body"];
    body["channel"] = channel;
    body["subscription_id"] = channel;

    if (age || count) {
      nlohmann::json history;
      if (age) {
        history.emplace("age", *age);
      }
      if (count) {
        history.emplace("count", *count);
      }

      body.emplace("history", history);
    }

    return document;
  }
};

// TODO: convert to function
struct unsubscribe_request {
  const uint64_t id;
  const std::string channel;

  nlohmann::json to_json() const {
    nlohmann::json document =
        R"({"action":"rtm/unsubscribe", "body":{"subscription_id":"<not_set>"}, "id": 2})"_json;

    CHECK(document.is_object());
    document["id"] = id;
    auto &body = document["body"];
    body["subscription_id"] = channel;

    return document;
  }
};

enum class client_state { STOPPED = 1, RUNNING = 2, PENDING_STOPPED = 3 };

std::ostream &operator<<(std::ostream &out, client_state const &s) {
  switch (s) {
    case client_state::RUNNING:
      return out << "RUNNING";
    case client_state::PENDING_STOPPED:
      return out << "PENDING_STOPPED";
    case client_state::STOPPED:
      return out << "STOPPED";
  }
}

using request_done_cb = std::function<void(boost::system::error_code)>;

struct ping_request {
  uint64_t id;
  request_done_cb done_cb;
};

struct write_request {
  write_request(std::string &&str, request_done_cb &&done_cb)
      : data(std::move(str)), done_cb(std::move(done_cb)), buffer(asio::buffer(data)) {}
  std::string data;
  request_done_cb done_cb;
  boost::asio::const_buffer buffer;
};

using io_request = boost::variant<ping_request, write_request>;

struct get_done_cb_visitor : public boost::static_visitor<const request_done_cb &> {
  const request_done_cb &operator()(const write_request &request) const {
    return request.done_cb;
  }

  const request_done_cb &operator()(const ping_request &request) const {
    return request.done_cb;
  }
};

uint64_t new_request_id() {
  static uint64_t request_id{1};
  return request_id++;
}

struct subscription_details {
  const std::string channel;
  const subscription &sub;
  subscription_callbacks &callbacks;
};

class subscriptions_map {
 public:
  void add(const std::string &channel, const subscription &sub,
           subscription_callbacks &callbacks) {
    CHECK_EQ(_channels_map.count(channel), 0) << "already exists for channel " << channel;
    CHECK_EQ(_subs_map.count(&sub), 0) << "already exists for sub " << channel;

    auto it = _sub_infos.emplace(_sub_infos.end(),
                                 subscription_details{channel, sub, callbacks});

    _channels_map.emplace(channel, it);
    _subs_map.emplace(&sub, it);
  }

  boost::optional<subscription_details &> find_by_channel(
      const std::string &channel) const {
    auto it = _channels_map.find(channel);
    if (it == _channels_map.end()) {
      return boost::none;
    }
    return *(it->second);
  }

  boost::optional<subscription_details &> find_by_sub(const subscription &sub) const {
    auto it = _subs_map.find(&sub);
    if (it == _subs_map.end()) {
      return boost::none;
    }
    return *(it->second);
  }

  bool delete_by_channel(const std::string &channel) {
    auto it = _channels_map.find(channel);
    if (it == _channels_map.end()) {
      return false;
    }

    auto sub_info_it = it->second;
    _subs_map.erase(&sub_info_it->sub);
    _channels_map.erase(sub_info_it->channel);
    _sub_infos.erase(sub_info_it);
    return true;
  }

  void clear() {
    _channels_map.clear();
    _subs_map.clear();
    _sub_infos.clear();
  }

 private:
  std::list<subscription_details> _sub_infos;
  std::unordered_map<std::string, std::list<subscription_details>::iterator>
      _channels_map;
  // TODO: using object addresses may not be reliable
  std::unordered_map<const subscription *, std::list<subscription_details>::iterator>
      _subs_map;
};

enum class request_type { PUBLISH = 0, SUBSCRIBE = 1, UNSUBSCRIBE = 2 };

struct sent_request_info {
  const request_type type;
  const std::string channel;
  const nlohmann::json pdu;
  const std::chrono::system_clock::time_point time;
  const size_t buffer_size;
  request_callbacks *callbacks;  // TODO: later on convert it to reference
};

class secure_client : public client, public boost::static_visitor<> {
 public:
  explicit secure_client(const std::string &host, const std::string &port,
                         const std::string &appkey, uint64_t client_id,
                         error_callbacks &common_error_callbacks,
                         asio::io_service &io_service, asio::ssl::context &ssl_ctx)
      : _host{host},
        _port{port},
        _appkey{appkey},
        _tcp_resolver{io_service},
        _ws{io_service, ssl_ctx},
        _client_id{client_id},
        _common_error_callbacks{common_error_callbacks},
        _ping_timer{io_service} {
    _control_callback = [this](boost::beast::websocket::frame_type type,
                               const boost::beast::string_view &payload) {
      switch (type) {
        case boost::beast::websocket::frame_type::close:
          rtm_frames_received_total.Add({{"type", "close"}}).Increment();
          LOG(2) << "got close frame " << payload;
          break;
        case boost::beast::websocket::frame_type::ping:
          rtm_frames_received_total.Add({{"type", "ping"}}).Increment();
          LOG(2) << "got ping frame " << payload;
          break;
        case boost::beast::websocket::frame_type::pong:
          rtm_frames_received_total.Add({{"type", "ping"}}).Increment();
          LOG(4) << "got pong frame " << payload;
          on_pong(payload);
          break;
      }
    };
  }

  ~secure_client() override = default;

  std::error_condition start() override {
    CHECK_EQ(_client_state.load(), client_state::STOPPED);
    LOG(INFO) << "Starting secure RTM client: " << _host << ":" << _port
              << ", appkey: " << _appkey;

    boost::system::error_code ec;

    auto endpoints = _tcp_resolver.resolve({_host, _port}, ec);
    if (ec.value() != 0) {
      LOG(ERROR) << "can't resolve endpoint: [" << ec << "] " << ec.message();
      rtm_client_error.Add({{"type", "tcp_resolve_endpoint"}}).Increment();
      return make_error_condition(client_error::ASIO_ERROR);
    }

    _ws.read_message_max(read_buffer_size);

    // tcp connect
    asio::connect(_ws.next_layer().next_layer(), endpoints, ec);
    if (ec.value() != 0) {
      LOG(ERROR) << "can't connect: [" << ec << "] " << ec.message();
      rtm_client_error.Add({{"type", "tcp_connect"}}).Increment();
      return make_error_condition(client_error::ASIO_ERROR);
    }

    // ssl handshake
    _ws.next_layer().handshake(boost::asio::ssl::stream_base::client, ec);
    if (ec.value() != 0) {
      LOG(ERROR) << "can't handshake SSL: [" << ec << "] " << ec.message();
      rtm_client_error.Add({{"type", "ssl_handshake"}}).Increment();
      return make_error_condition(client_error::ASIO_ERROR);
    }

    // upgrade to ws.
    boost::beast::websocket::response_type ws_upgrade_response;
    _ws.handshake_ex(ws_upgrade_response, _host + ":" + _port, "/v2?appkey=" + _appkey,
                     [this](boost::beast::websocket::request_type &ws_upgrade_request) {
                       if (use_cbor) {
                         ws_upgrade_request.set(
                             boost::beast::http::field::sec_websocket_protocol, "cbor");
                       }
                       LOG(2) << "websocket upgrade request:\n" << ws_upgrade_request;
                     },
                     ec);
    LOG(2) << "websocket upgrade response:\n" << ws_upgrade_response;
    if (ec.value() != 0) {
      LOG(ERROR) << "can't upgrade to websocket protocol: [" << ec << "] "
                 << ec.message();
      rtm_client_error.Add({{"type", "ws_upgrade"}}).Increment();
      return make_error_condition(client_error::ASIO_ERROR);
    }
    LOG(INFO) << "websocket open";
    rtm_client_start.Increment();

    _ws.control_callback(_control_callback);
    if (use_cbor) {
      _ws.binary(true);
    }

    arm_ping_timer();

    _client_state = client_state::RUNNING;
    ask_for_read();
    return {};
  }

  std::error_condition stop() override {
    CHECK_EQ(_client_state, client_state::RUNNING);
    LOG(INFO) << "Stopping secure RTM client";

    _client_state = client_state::PENDING_STOPPED;
    boost::system::error_code ec;
    _ping_timer.cancel(ec);
    if (ec.value() != 0) {
      LOG(ERROR) << "can't stop ping timer: [" << ec << "] " << ec.message();
      rtm_client_error.Add({{"type", "stop_ping_timer"}}).Increment();
      return make_error_condition(client_error::ASIO_ERROR);
    }

    _ws.next_layer().next_layer().close(ec);
    if (ec.value() != 0) {
      LOG(ERROR) << "can't close: [" << ec << "] " << ec.message();
      rtm_client_error.Add({{"type", "close_connection"}}).Increment();
      return make_error_condition(client_error::ASIO_ERROR);
    }

    _ws.control_callback();
    return {};
  }

  request_done_cb handle_write(
      std::unordered_map<uint64_t, sent_request_info>::const_iterator it) {
    return [this, it](boost::system::error_code ec) {
      const auto after_write = std::chrono::system_clock::now();
      const auto &request_info = it->second;
      rtm_write_delay_microseconds.Observe(
          std::chrono::duration_cast<std::chrono::microseconds>(after_write
                                                                - request_info.time)
              .count());

      if (ec.value() != 0) {
        LOG(ERROR) << "write request failure: [" << ec << "] " << ec.message()
                   << ", message " << request_info.pdu;
        rtm_client_error.Add({{"type", "publish"}}).Increment();
        if (request_info.callbacks != nullptr) {
          if (request_info.type == request_type::PUBLISH) {
            request_info.callbacks->on_error(
                make_error_condition(client_error::PUBLISH_ERROR));
          } else if (request_info.type == request_type::SUBSCRIBE) {
            request_info.callbacks->on_error(
                make_error_condition(client_error::SUBSCRIBE_ERROR));
          } else if (request_info.type == request_type::UNSUBSCRIBE) {
            request_info.callbacks->on_error(
                make_error_condition(client_error::UNSUBSCRIBE_ERROR));
          } else {
            ABORT() << "unreachable";
          }
        }
        _sent_request_infos.erase(it);
      } else {
        if (request_info.type == request_type::PUBLISH) {
          rtm_messages_sent.Add({{"channel", request_info.channel}}).Increment();
          rtm_messages_bytes_sent.Add({{"channel", request_info.channel}})
              .Increment(request_info.buffer_size);
        }
        rtm_bytes_written.Increment(request_info.buffer_size);
      }
    };
  }

  void publish(const std::string &channel, nlohmann::json &&message,
               request_callbacks *callbacks) override {
    if (_client_state == client_state::PENDING_STOPPED) {
      LOG(1) << "RTM client is pending stop";
      return;
    }
    CHECK_EQ(_client_state, client_state::RUNNING)
        << "RTM client is not running, channel " << channel << ", message " << message;

    nlohmann::json pdu = nlohmann::json::object();
    pdu["action"] = "rtm/publish";
    auto &body = pdu["body"];
    body = nlohmann::json::object();
    body["channel"] = channel;
    body["message"] = message;
    const uint64_t request_id = new_request_id();
    pdu["id"] = request_id;

    std::string buffer = use_cbor ? json_to_cbor(pdu) : pdu.dump();

    const auto insert_result = _sent_request_infos.emplace(
        request_id,
        sent_request_info{request_type::PUBLISH, channel, std::move(pdu),
                          std::chrono::system_clock::now(), buffer.size(), callbacks});
    CHECK(insert_result.second);
    const auto it = insert_result.first;

    write(std::move(buffer), handle_write(it));
  }

  void subscribe(const std::string &channel, const subscription &sub,
                 subscription_callbacks &data_callbacks, request_callbacks *callbacks,
                 const subscription_options *options) override {
    if (_client_state == client_state::PENDING_STOPPED) {
      LOG(1) << "RTM client is pending stop";
      return;
    }
    CHECK_EQ(_client_state, client_state::RUNNING) << "RTM client is not running";

    const uint64_t request_id = new_request_id();
    subscribe_request request{request_id, channel};
    if (options != nullptr) {
      request.age = options->history.age;
      request.count = options->history.count;
    }

    _channel_subscriptions.add(channel, sub, data_callbacks);

    nlohmann::json pdu = request.to_json();
    std::string buffer = use_cbor ? json_to_cbor(pdu) : pdu.dump();

    const auto insert_result = _sent_request_infos.emplace(
        request_id,
        sent_request_info{request_type::SUBSCRIBE, channel, std::move(pdu),
                          std::chrono::system_clock::now(), buffer.size(), callbacks});
    CHECK(insert_result.second);
    const auto it = insert_result.first;

    write(std::move(buffer), handle_write(it));
  }

  void unsubscribe(const subscription &sub_to_delete,
                   request_callbacks *callbacks) override {
    if (_client_state == client_state::PENDING_STOPPED) {
      LOG(1) << "RTM client is pending stop";
      return;
    }
    CHECK_EQ(_client_state, client_state::RUNNING) << "RTM client is not running";

    const auto found = _channel_subscriptions.find_by_sub(sub_to_delete);
    CHECK(found) << "didn't find subscription";

    const uint64_t request_id = new_request_id();
    unsubscribe_request request{request_id, found->channel};

    nlohmann::json pdu = request.to_json();
    std::string buffer = use_cbor ? json_to_cbor(pdu) : pdu.dump();

    const auto insert_result = _sent_request_infos.emplace(
        request_id,
        sent_request_info{request_type::UNSUBSCRIBE, found->channel, std::move(pdu),
                          std::chrono::system_clock::now(), buffer.size(), callbacks});
    CHECK(insert_result.second);
    const auto it = insert_result.first;

    write(std::move(buffer), handle_write(it));
  }

 private:
  void on_pong(const boost::beast::string_view &payload) {
    const auto now = std::chrono::system_clock::now();
    rtm_last_pong_time_seconds.Set(
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());

    uint64_t value;
    try {
      value = std::stoull(std::string{payload.data(), payload.size()});
    } catch (const std::exception &e) {
      ABORT() << "Invalid pong value: " << e.what() << " " << payload;
      return;
    }

    auto it = _ping_times.find(value);
    CHECK(it != _ping_times.end()) << "Unexpected pong value: " << payload;
    const std::chrono::system_clock::time_point ping_time = it->second;
    _ping_times.erase(it);

    const double latency = std::abs(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - ping_time).count());
    rtm_ping_latency_millis.Observe(latency);
  }

  void ask_for_read() {
    _ws.async_read(_read_buffer, [this](boost::system::error_code const &ec,
                                        unsigned long bytes_read) {
      const auto arrival_time = std::chrono::system_clock::now();

      LOG(4) << this << " async_read " << bytes_read << " bytes ec=" << ec;
      if (ec.value() != 0) {
        if (ec == boost::asio::error::operation_aborted) {
          LOG(INFO) << this << " async_read operation is cancelled";
          CHECK(_client_state == client_state::PENDING_STOPPED);
          _client_state = client_state::STOPPED;
          _channel_subscriptions.clear();
        } else if (_client_state == client_state::RUNNING) {
          rtm_client_error.Add({{"type", "read"}}).Increment();
          LOG(ERROR) << this << " asio error: [" << ec << "] " << ec.message();
          _common_error_callbacks.on_error(
              make_error_condition(client_error::ASIO_ERROR));
        } else {
          LOG(INFO) << this << " ignoring asio error because in state " << _client_state
                    << ": [" << ec << "] " << ec.message();
        }
        return;
      }

      const std::string buffer = boost::beast::buffers_to_string(_read_buffer.data());
      CHECK_EQ(buffer.size(), _read_buffer.size());
      _read_buffer.consume(_read_buffer.size());
      rtm_bytes_read.Increment(_read_buffer.size());

      nlohmann::json document;

      if (use_cbor) {
        auto doc_or_error = cbor_to_json(buffer);
        if (!doc_or_error.ok()) {
          LOG(ERROR) << "CBOR message couldn't be processed: "
                     << doc_or_error.error_message();
          return;
        }
        document = doc_or_error.get();
      } else {
        try {
          document = nlohmann::json::parse(buffer);
        } catch (const std::exception &e) {
          LOG(ERROR) << "Bad data: " << e.what() << " " << buffer;
          return;
        }
      }

      LOG(9) << this << " async_read processing input";
      process_input(document, buffer.size(), arrival_time);

      LOG(9) << this << " async_read asking for read";
      ask_for_read();
    });
  }

  void arm_ping_timer() {
    LOG(4) << this << " setting ws ping timer";

    _ping_timer.expires_from_now(ws_ping_interval);
    _ping_timer.async_wait([this](const boost::system::error_code &ec_timer) {
      LOG(4) << this << " ping timer ec=" << ec_timer;
      if (ec_timer.value() != 0) {
        if (ec_timer == boost::asio::error::operation_aborted) {
          LOG(INFO) << this << " ping timer is cancelled";
        } else if (_client_state == client_state::RUNNING) {
          LOG(ERROR) << this << " ping timer error for ping timer: [" << ec_timer << "] "
                     << ec_timer.message();
          rtm_client_error.Add({{"type", "ping_timer"}}).Increment();
          _common_error_callbacks.on_error(
              make_error_condition(client_error::ASIO_ERROR));
        } else {
          LOG(INFO) << this << " ignoring asio error for ping timer because in state "
                    << _client_state << ": [" << ec_timer << "] " << ec_timer.message();
        }
        return;
      }

      const auto request_id = new_request_id();
      _ping_times.emplace(request_id, std::chrono::system_clock::now());
      ping(request_id, [this](boost::system::error_code const &ec_ping) {
        LOG(4) << this << " ping write ec=" << ec_ping;
        if (ec_ping.value() != 0) {
          if (ec_ping == boost::asio::error::operation_aborted) {
            LOG(INFO) << this << " ping operation is cancelled";
          } else if (_client_state == client_state::RUNNING) {
            LOG(ERROR) << this << " asio error for ping operation: [" << ec_ping << "] "
                       << ec_ping.message();
            rtm_client_error.Add({{"type", "ping"}}).Increment();
            _common_error_callbacks.on_error(
                make_error_condition(client_error::ASIO_ERROR));
          } else {
            LOG(INFO) << this
                      << " ignoring asio error for ping operation because in state "
                      << _client_state << ": [" << ec_ping << "] " << ec_ping.message();
          }
          return;
        }

        rtm_pings_sent_total.Increment();

        auto time_since_epoch = std::chrono::system_clock::now().time_since_epoch();
        rtm_last_ping_time_seconds.Set(
            std::chrono::duration_cast<std::chrono::seconds>(time_since_epoch).count());

        LOG(4) << this << " scheduling next ping";
        arm_ping_timer();
      });
    });
  }

  std::unordered_map<uint64_t, sent_request_info>::const_iterator
  process_request_confirmation(const nlohmann::json &pdu,
                               const std::chrono::system_clock::time_point arrival_time) {
    CHECK(pdu.find("id") != pdu.end()) << "no id in pdu: " << pdu;
    const uint64_t id = pdu["id"];
    const auto it = _sent_request_infos.find(id);
    CHECK(it != _sent_request_infos.end()) << "unexpected confirmation: " << pdu;
    const auto &request_info = it->second;
    if (request_info.type == request_type::PUBLISH) {
      rtm_publish_ack_latency_millis.Observe(
          std::chrono::duration_cast<std::chrono::milliseconds>(arrival_time
                                                                - request_info.time)
              .count());
      rtm_publish_inflight_total.Set(_sent_request_infos.size() - 1);
    }
    return it;
  }

  std::pair<subscription_details &, const nlohmann::json &> process_subscription_pdu(
      const nlohmann::json &pdu) {
    CHECK(pdu.find("body") != pdu.end()) << "no body in pdu: " << pdu;
    const auto &body = pdu["body"];
    CHECK(body.find("subscription_id") != body.end())
        << "no subscription_id in body: " << pdu;
    const std::string channel = body["subscription_id"];

    const auto found = _channel_subscriptions.find_by_channel(channel);
    CHECK(found) << "no subscription for pdu: " << pdu;
    return {*found, body};
  }

  void process_input(const nlohmann::json &pdu, size_t byte_size,
                     std::chrono::system_clock::time_point arrival_time) {
    CHECK(pdu.is_object()) << "not an object: " << pdu;
    CHECK(pdu.find("action") != pdu.end()) << "no action in pdu: " << pdu;
    const std::string action = pdu["action"];
    rtm_actions_received.Add({{"action", action}}).Increment();

    if (action == "rtm/subscription/data") {
      auto result = process_subscription_pdu(pdu);
      auto &sub_info = result.first;
      const auto &body = result.second;

      CHECK(body.find("messages") != body.end()) << "no messages in body: " << pdu;
      const auto &messages = body["messages"];
      CHECK(messages.is_array()) << "messages is not an array: " << pdu;

      rtm_messages_received.Add({{"channel", sub_info.channel}}).Increment();
      rtm_messages_bytes_received.Add({{"channel", sub_info.channel}})
          .Increment(byte_size);
      rtm_messages_in_pdu.Observe(messages.size());

      for (const auto &m : messages) {
        // TODO: avoid copying of m
        sub_info.callbacks.on_data(sub_info.sub, {m, arrival_time});
      }
    } else if (action == "rtm/subscription/error") {
      LOG(ERROR) << "subscription error: " << pdu;
      rtm_subscription_error_total.Increment();
      auto result = process_subscription_pdu(pdu);
      auto &sub_info = result.first;
      sub_info.callbacks.on_error(make_error_condition(client_error::SUBSCRIPTION_ERROR));
    } else if (action == "rtm/publish/ok") {
      auto it = process_request_confirmation(pdu, arrival_time);
      if (it->second.callbacks != nullptr) {
        it->second.callbacks->on_ok();
      }
      _sent_request_infos.erase(it);
    } else if (action == "rtm/publish/error") {
      LOG(ERROR) << "got publish error: " << pdu;
      rtm_publish_error_total.Increment();
      auto it = process_request_confirmation(pdu, arrival_time);
      if (it->second.callbacks != nullptr) {
        it->second.callbacks->on_error(make_error_condition(client_error::PUBLISH_ERROR));
      }
      _sent_request_infos.erase(it);
    } else if (action == "rtm/subscribe/ok") {
      auto it = process_request_confirmation(pdu, arrival_time);
      if (it->second.callbacks != nullptr) {
        it->second.callbacks->on_ok();
      }
      _sent_request_infos.erase(it);
    } else if (action == "rtm/subscribe/error") {
      LOG(ERROR) << "got subscribe error: " << pdu;
      rtm_subscribe_error_total.Increment();
      auto it = process_request_confirmation(pdu, arrival_time);
      if (it->second.callbacks != nullptr) {
        it->second.callbacks->on_error(
            make_error_condition(client_error::SUBSCRIBE_ERROR));
      }
      _sent_request_infos.erase(it);
      CHECK(_channel_subscriptions.delete_by_channel(it->second.channel))
          << "failed to delete: " << pdu;
    } else if (action == "rtm/unsubscribe/ok") {
      auto it = process_request_confirmation(pdu, arrival_time);
      if (it->second.callbacks != nullptr) {
        it->second.callbacks->on_ok();
      }
      _sent_request_infos.erase(it);
      CHECK(_channel_subscriptions.delete_by_channel(it->second.channel))
          << "failed to delete: " << pdu;
    } else if (action == "rtm/unsubscribe/error") {
      LOG(ERROR) << "got unsubscribe error: " << pdu;
      rtm_unsubscribe_error_total.Increment();
      auto it = process_request_confirmation(pdu, arrival_time);
      if (it->second.callbacks != nullptr) {
        it->second.callbacks->on_error(
            make_error_condition(client_error::UNSUBSCRIBE_ERROR));
      }
      _sent_request_infos.erase(it);
      CHECK(_channel_subscriptions.delete_by_channel(it->second.channel))
          << "failed to delete: " << pdu;
    } else if (action == "/error") {
      ABORT() << "got unexpected error: " << pdu;
    } else {
      ABORT() << "unsupported action: " << pdu;
    }
  }

  void write(std::string &&data, request_done_cb &&done_cb) {
    LOG(4) << "write " << data.size();
    _pending_requests.push(write_request{std::move(data), std::move(done_cb)});
    drain_requests();
  }

  void ping(uint64_t request_id, request_done_cb &&done_cb) {
    LOG(4) << "ping";
    _pending_requests.push(ping_request{request_id, std::move(done_cb)});
    drain_requests();
  }

  void drain_requests() {
    LOG(4) << "drain requests " << _pending_requests.size();
    rtm_pending_requests.Set(_pending_requests.size());
    if (_pending_requests.empty() || _request_in_flight) {
      LOG(4) << "drain requests early return";
      return;
    }

    _request_in_flight = true;
    const auto &request = _pending_requests.front();
    boost::apply_visitor(*this, request);
  }

 public:
  // public for visitor
  void operator()(const write_request &request) {
    LOG(4) << "write request";
    auto buffer_size = request.data.size();
    _ws.async_write(request.buffer, [this, buffer_size](boost::system::error_code ec,
                                                        std::size_t bytes_transferred) {
      LOG(4) << "write done " << bytes_transferred;
      if (!ec) {
        CHECK(buffer_size == bytes_transferred);
      }
      on_request_done(ec);
    });
  }

  void operator()(const ping_request &request) {
    LOG(4) << "ping request";
    boost::beast::websocket::ping_data payload{std::to_string(request.id)};
    _ws.async_ping(payload, [this](boost::system::error_code ec) {
      LOG(4) << "ping done";
      on_request_done(ec);
    });
  }

 private:
  void on_request_done(boost::system::error_code ec) {
    const auto &request = _pending_requests.front();
    boost::apply_visitor(get_done_cb_visitor{}, request)(ec);
    _pending_requests.pop();
    _request_in_flight = false;
    drain_requests();
  }

  // TODO: check if can get rid of client state
  std::atomic<client_state> _client_state{client_state::STOPPED};

  const std::string _host;
  const std::string _port;
  const std::string _appkey;
  const uint64_t _client_id;
  error_callbacks &_common_error_callbacks;

  asio::ip::tcp::resolver _tcp_resolver;
  boost::beast::websocket::stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket> >
      _ws;
  boost::beast::multi_buffer _read_buffer{read_buffer_size};
  subscriptions_map _channel_subscriptions;
  boost::asio::deadline_timer _ping_timer;
  std::unordered_map<uint64_t, std::chrono::system_clock::time_point> _ping_times;
  std::function<void(boost::beast::websocket::frame_type type,
                     boost::beast::string_view payload)>
      _control_callback;
  std::unordered_map<uint64_t, sent_request_info> _sent_request_infos;
  std::queue<io_request> _pending_requests;
  bool _request_in_flight{false};
};

}  // namespace

std::unique_ptr<client> new_client(const std::string &endpoint, const std::string &port,
                                   const std::string &appkey,
                                   asio::io_service &io_service,
                                   asio::ssl::context &ssl_ctx, size_t id,
                                   error_callbacks &callbacks) {
  LOG(1) << "Creating RTM client for " << endpoint << ":" << port << "?appkey=" << appkey;
  std::unique_ptr<secure_client> client(
      new secure_client(endpoint, port, appkey, id, callbacks, io_service, ssl_ctx));
  return std::move(client);
}

resilient_client::resilient_client(asio::io_service &io_service,
                                   std::thread::id io_thread_id,
                                   resilient_client::client_factory_t &&factory,
                                   error_callbacks &callbacks)
    : _io(io_service),
      _io_thread_id(io_thread_id),
      _factory(std::move(factory)),
      _error_callbacks(callbacks) {}

void resilient_client::publish(const std::string &channel, nlohmann::json &&message,
                               request_callbacks *callbacks) {
  CHECK_EQ(std::this_thread::get_id(), _io_thread_id)
      << "Invocation from " << threadutils::get_current_thread_name();

  _client->publish(channel, std::move(message), callbacks);
}

void resilient_client::subscribe(const std::string &channel, const subscription &sub,
                                 subscription_callbacks &data_callbacks,
                                 request_callbacks *callbacks,
                                 const subscription_options *options) {
  CHECK_EQ(std::this_thread::get_id(), _io_thread_id)
      << "Invocation from " << threadutils::get_current_thread_name();

  _subscriptions.push_back({channel, &sub, &data_callbacks, callbacks, options});
  _client->subscribe(channel, sub, data_callbacks, callbacks, options);
}

void resilient_client::unsubscribe(const subscription &sub,
                                   request_callbacks *callbacks) {
  CHECK_EQ(std::this_thread::get_id(), _io_thread_id)
      << "Invocation from " << threadutils::get_current_thread_name();

  _client->unsubscribe(sub, callbacks);
  std::remove_if(_subscriptions.begin(), _subscriptions.end(),
                 [&sub](const subscription_info &si) { return &sub == si.sub; });
}

std::error_condition resilient_client::start() {
  CHECK_EQ(std::this_thread::get_id(), _io_thread_id)
      << "Invocation from " << threadutils::get_current_thread_name();

  if (!_client) {
    LOG(1) << "creating new client";
    _client = _factory(*this);
  }

  _started = true;
  return _client->start();
}

std::error_condition resilient_client::stop() {
  CHECK_EQ(std::this_thread::get_id(), _io_thread_id)
      << "Invocation from " << threadutils::get_current_thread_name();

  _started = false;
  return _client->stop();
}

void resilient_client::on_error(std::error_condition ec) {
  CHECK_EQ(std::this_thread::get_id(), _io_thread_id)
      << "Invocation from " << threadutils::get_current_thread_name();

  LOG(INFO) << "restarting rtm client because of error: " << ec.message();
  restart();
}

void resilient_client::restart() {
  CHECK_EQ(std::this_thread::get_id(), _io_thread_id)
      << "Invocation from " << threadutils::get_current_thread_name();

  LOG(1) << "creating new client";
  _client = _factory(*this);
  if (!_started) {
    return;
  }

  LOG(1) << "starting new client";
  auto ec = _client->start();
  if (ec) {
    LOG(ERROR) << "can't restart client: " << ec.message();
    _error_callbacks.on_error(ec);
    return;
  }

  LOG(1) << "restoring subscriptions";
  for (const auto &sub : _subscriptions) {
    _client->subscribe(sub.channel, *sub.sub, *sub.data_callbacks, sub.callbacks,
                       sub.options);
  }

  LOG(1) << "client restart done";
}

thread_checking_client::thread_checking_client(asio::io_service &io,
                                               std::thread::id io_thread_id,
                                               std::unique_ptr<client> client)
    : _io(io), _io_thread_id(io_thread_id), _client(std::move(client)) {}

void thread_checking_client::publish(const std::string &channel, nlohmann::json &&message,
                                     request_callbacks *callbacks) {
  if (std::this_thread::get_id() != _io_thread_id) {
    LOG(WARNING) << "Forwarding request from thread "
                 << threadutils::get_current_thread_name();
    _io.post([ this, channel, message = std::move(message), callbacks ]() mutable {
      _client->publish(channel, std::move(message), callbacks);
    });
    return;
  }

  _client->publish(channel, std::move(message), callbacks);
}

void thread_checking_client::subscribe(const std::string &channel,
                                       const subscription &sub,
                                       subscription_callbacks &data_callbacks,
                                       request_callbacks *callbacks,
                                       const subscription_options *options) {
  if (std::this_thread::get_id() != _io_thread_id) {
    LOG(WARNING) << "Forwarding request from thread "
                 << threadutils::get_current_thread_name();
    _io.post([this, channel, &sub, &data_callbacks, callbacks, options]() {
      _client->subscribe(channel, sub, data_callbacks, callbacks, options);
    });
    return;
  }

  _client->subscribe(channel, sub, data_callbacks, callbacks, options);
}

void thread_checking_client::unsubscribe(const subscription &sub,
                                         request_callbacks *callbacks) {
  if (std::this_thread::get_id() != _io_thread_id) {
    LOG(5) << "Forwarding request from thread " << threadutils::get_current_thread_name();
    _io.post([this, &sub, callbacks]() { _client->unsubscribe(sub, callbacks); });
    return;
  }

  _client->unsubscribe(sub, callbacks);
}

std::error_condition thread_checking_client::start() {
  CHECK_EQ(std::this_thread::get_id(), _io_thread_id)
      << "Invocation from " << threadutils::get_current_thread_name();

  return _client->start();
}

std::error_condition thread_checking_client::stop() {
  CHECK_EQ(std::this_thread::get_id(), _io_thread_id)
      << "Invocation from " << threadutils::get_current_thread_name();

  return _client->stop();
}

}  // namespace rtm
}  // namespace video
}  // namespace satori
