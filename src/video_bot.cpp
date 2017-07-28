#include <rapidjson/document.h>
#include <rapidjson/filereadstream.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/program_options.hpp>
#include <boost/scope_exit.hpp>
#include <chrono>
#include <iostream>
#include <list>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "base64.h"
#include "bot_environment.h"
#include "cbor_json.h"
#include "librtmvideo/data.h"
#include "librtmvideo/decoder.h"
#include "librtmvideo/rtmpacket.h"
#include "librtmvideo/rtmvideo.h"
#include "librtmvideo/tele.h"
#include "librtmvideo/video_bot.h"
#include "rtmclient.h"
#include "stopwatch.h"
#include "tele_impl.h"
#include "worker.h"

namespace asio = boost::asio;

namespace rtm {
namespace video {

namespace {

constexpr size_t network_frames_max_buffer_size = 1024;
constexpr size_t image_frames_max_buffer_size = 2;

auto frames_received = tele::counter_new("vbot", "frames_received");
auto messages_received = tele::counter_new("vbot", "messages_received");
auto bytes_received = tele::counter_new("vbot", "bytes_received");
auto metadata_received = tele::counter_new("vbot", "metadata_received");
auto network_frame_buffer_size =
    tele::gauge_new("vbot", "network_frame_buffer_size");
auto image_frame_buffer_size =
    tele::gauge_new("vbot", "image_frame_buffer_size");
auto decoding_times_millis =
    tele::distribution_new("vbot", "decoding_times_millis");
auto processing_times_millis =
    tele::distribution_new("vbot", "processing_times_millis");
auto image_frames_dropped = tele::counter_new("vbot", "image_frames_dropped");
auto network_buffer_dropped =
    tele::counter_new("vbot", "network_buffer_dropped");

struct bot_message {
  cbor_item_t* data;
  bot_message_kind kind;
};

struct image_frame {
  std::string image_data;
  frame_id id;
  uint16_t width;
  uint16_t height;
  uint16_t linesize;
};

struct metadata_frame {
  std::string codec_name;
  std::string codec_data;
};

struct channel_names {
  explicit channel_names(const std::string &base_name) :
      frames(base_name + frames_channel_suffix),
      control(base_name + control_channel_suffix),
      metadata(base_name + metadata_channel_suffix),
      analysis(base_name + analysis_channel_suffix),
      debug(base_name + debug_channel_suffix) {}

  const std::string frames;
  const std::string control;
  const std::string metadata;
  const std::string analysis;
  const std::string debug;
};


}  // namespace

class bot_api_exception : public std::runtime_error {
 public:
  bot_api_exception() : runtime_error("bot api error") {}
};

network_frame decode_network_frame(const rapidjson::Value& msg) {
  auto t = msg["i"].GetArray();
  uint64_t i1 = t[0].GetUint64();
  uint64_t i2 = t[1].GetUint64();

  uint32_t rtp_timestamp = msg.HasMember("rt") ? (uint32_t)msg["rt"].GetInt64()
                                               : 0;  // rjson thinks its int64
  double ntp_timestamp = msg.HasMember("t") ? msg["t"].GetDouble() : 0;

  uint32_t chunk = 1, chunks = 1;
  if (msg.HasMember("c")) {
    chunk = msg["c"].GetUint();
    chunks = msg["l"].GetUint();
  }

  return {msg["d"].GetString(), std::make_pair(i1, i2),
          std::chrono::system_clock::from_time_t(ntp_timestamp), chunk, chunks};
}

metadata decode_metadata_frame(const rapidjson::Value& msg) {
  std::string codec_data =
      msg.HasMember("codecData") ? decode64(msg["codecData"].GetString()) : "";
  std::string codec_name = msg["codecName"].GetString();
  return {codec_name, codec_data};
}

class bot_instance : public bot_context, public rtm::subscription_callbacks {
 public:
  bot_instance(const std::string& bot_id, const bot_descriptor& descriptor,
               const std::string& channel, rtm::video::bot_environment& env)
      : _bot_id(bot_id), _descriptor(descriptor),
        _channels(channel),
        _env(env) {
    _decoder_worker = std::make_unique<threaded_worker<network_frame>>(
        network_frames_max_buffer_size, [this](network_frame&& frame) {
          process_network_frame(std::move(frame));
        });
    _process_worker = std::make_unique<threaded_worker<image_frame>>(
        image_frames_max_buffer_size,
        [this](image_frame&& frame) { process_image_frame(std::move(frame)); });
  }

  void queue_message(const bot_message_kind kind, cbor_item_t* message) {
    bot_message newmsg{message, kind};
    cbor_incref(message);
    _message_buffer.push_back(newmsg);
  }

  void subscribe(rtm::subscriber& s) {
    s.subscribe_channel(_channels.frames, _frames_subscription, *this);
    s.subscribe_channel(_channels.control, _control_subscription, *this);

    subscription_options metadata_options;
    metadata_options.history.count = 1;
    s.subscribe_channel(_channels.metadata, _metadata_subscription, *this,
                        &metadata_options);
  }

  void on_error(error e, const std::string& msg) override {
    std::cerr << "ERROR: " << (int)e << " " << msg << "\n";
    throw bot_api_exception();
  }

  void on_data(const subscription& sub,
               const rapidjson::Value& value) override {
    if (&sub == &_metadata_subscription)
      on_metadata(value);
    else if (&sub == &_frames_subscription)
      on_frame_data(value);
    else if (&sub == &_control_subscription)
      on_message_data(value);
    else
      BOOST_ASSERT_MSG(false, "Unknown subscription");
  }

 private:
  void on_metadata(const rapidjson::Value& msg) {
    metadata new_metadata = decode_metadata_frame(msg);
    tele::counter_inc(metadata_received);

    if (new_metadata.codec_data == _metadata.codec_data &&
        new_metadata.codec_name == _metadata.codec_name)
      return;

    _metadata = new_metadata;
    std::lock_guard<std::mutex> guard(_decoder_mutex);

    _decoder.reset(decoder_new_keep_proportions(_descriptor.image_width,
                                                _descriptor.image_height,
                                                _descriptor.pixel_format),
                   [this](decoder* d) {
                     std::cerr << "Deleting decoder\n";
                     decoder_delete(d);
                   });
    BOOST_VERIFY(_decoder);

    decoder_set_metadata(_decoder.get(), _metadata.codec_name.c_str(),
                         (const uint8_t*)_metadata.codec_data.data(),
                         _metadata.codec_data.size());
    std::cout << "Video decoder initialized\n";
  }

  void on_message_data(const rapidjson::Value& msg) {
    if (!_descriptor.ctrl_callback) return;
    if (msg.IsArray()) {
      for (auto& m : msg.GetArray()) on_message_data(m);
      return;
    }

    if (msg.IsObject()) {
      cbor_item_t* cmd = json_to_cbor(msg);
      auto cbor_deleter = gsl::finally([&cmd]() { cbor_decref(&cmd); });
      cbor_item_t* response = _descriptor.ctrl_callback(*this, cmd);
      if (response != nullptr) {
        queue_message(bot_message_kind::DEBUG, response);
        cbor_decref(&response);
      }
      send_messages(-1, -1);
      return;
    }
    std::cerr << "ERROR: Unsupported kind of message\n";
  }

  void on_frame_data(const rapidjson::Value& msg) {
    tele::counter_inc(messages_received);

    if (!_decoder) {
      return;
    }

    network_frame frame = decode_network_frame(msg);
    tele::counter_inc(bytes_received, frame.base64_data.size());

    tele::gauge_set(network_frame_buffer_size, _decoder_worker->queue_size());
    tele::gauge_set(image_frame_buffer_size, _process_worker->queue_size());

    if (!_decoder_worker->try_send(std::move(frame))) {
      tele::counter_inc(network_buffer_dropped);
      std::cerr << "dropped network frame, clearing network buffer\n";
      _decoder_worker->clear();
    }
  }

  // called only from decoder worker.
  void process_network_frame(network_frame&& frame) {
    std::lock_guard<std::mutex> guard(_decoder_mutex);

    decoder* decoder = _decoder.get();
    {
      stopwatch<> s;
      decoder_process_frame_message(decoder, frame.id.first, frame.id.second,
                                    (const uint8_t*)frame.base64_data.c_str(),
                                    frame.base64_data.size(), frame.chunk,
                                    frame.chunks);
      tele::distribution_add(decoding_times_millis, s.millis());
    }

    if (decoder_frame_ready(decoder)) {
      tele::counter_inc(frames_received);
      if (_descriptor.img_callback) {
        image_frame image_frame{
            std::string((const char*)decoder_image_data(decoder),
                        decoder_image_line_size(decoder) *
                            decoder_image_height(decoder)),
            frame.id, ((uint16_t)decoder_image_width(decoder)),
            ((uint16_t)decoder_image_height(decoder)),
            ((uint16_t)decoder_image_line_size(decoder))};
        if (!_process_worker->try_send(std::move(image_frame))) {
          tele::counter_inc(image_frames_dropped);
        }
      }
    }
  }

  void process_image_frame(image_frame&& frame) {
    {
      stopwatch<> s;
      _descriptor.img_callback(*this, ((const uint8_t*)frame.image_data.data()),
                               frame.width, frame.height, frame.linesize);
      tele::distribution_add(processing_times_millis, s.millis());
    }
    // todo: first id should be last_frame.second + 1.
    send_messages(frame.id.first, frame.id.second);
  }

  void send_messages(int64_t i1, int64_t i2) {
    for (auto&& msg : _message_buffer) {
      cbor_item_t* data = msg.data;

      if (i1 >= 0) {
        cbor_item_t* is = cbor_new_definite_array(2);
        cbor_array_set(is, 0,
                       cbor_move(cbor_build_uint64(static_cast<uint64_t>(i1))));
        cbor_array_set(is, 1,
                       cbor_move(cbor_build_uint64(static_cast<uint64_t>(i2))));
        cbor_map_add(
            data, (struct cbor_pair){.key = cbor_move(cbor_build_string("i")),
                                     .value = cbor_move(is)});
      }

      switch (msg.kind) {
        case bot_message_kind::ANALYSIS:
          _env.publisher().publish(_channels.analysis, data);
          break;
        case bot_message_kind::DEBUG:
          _env.publisher().publish(_channels.debug, data);
          break;
      }
      cbor_decref(&msg.data);
    }
    _message_buffer.clear();
  }

  const std::string _bot_id;
  const bot_descriptor _descriptor;
  const channel_names _channels;
  const rtm::subscription _frames_subscription{};
  const rtm::subscription _control_subscription{};
  const rtm::subscription _metadata_subscription{};
  const rtm::subscription _config_subscription{};
  rtm::video::bot_environment& _env;
  std::list<rtm::video::bot_message> _message_buffer;
  std::shared_ptr<decoder> _decoder;
  std::mutex _decoder_mutex;
  metadata _metadata;

  std::unique_ptr<threaded_worker<network_frame>> _decoder_worker;
  std::unique_ptr<threaded_worker<image_frame>> _process_worker;
};

cbor_item_t* configure_command(cbor_item_t* config) {
  cbor_item_t* cmd = cbor_new_definite_map(2);
  cbor_map_add(cmd, (struct cbor_pair){
                        .key = cbor_move(cbor_build_string("action")),
                        .value = cbor_move(cbor_build_string("configure"))});
  cbor_map_add(cmd,
               (struct cbor_pair){.key = cbor_move(cbor_build_string("body")),
                                  .value = config});
  return cmd;
}

bot_environment& bot_environment::instance() {
  static bot_environment env;
  return env;
}

void bot_environment::register_bot(const bot_descriptor* bot) {
  assert(!_bot_descriptor);
  _bot_descriptor = bot;
}

bool bot_environment::io_loop(
    std::function<std::unique_ptr<rtm::client>()> newClient,
    boost::asio::io_service& io_service) {
  _client = std::move(newClient());
  _bot_instance->subscribe(*_client);
  tele::publisher tele_publisher(*_client, io_service);

  io_service.run();
  return true;
}

boost::program_options::variables_map parse_command_line(int argc,
                                                         char* argv[]) {
  namespace po = boost::program_options;
  po::options_description desc("Allowed options");
  desc.add_options()("help", "produce help message")(
      "endpoint", po::value<std::string>(), "app endpoint")(
      "appkey", po::value<std::string>(), "app key")(
      "channel", po::value<std::string>(), "channel")(
      "port", po::value<std::string>(), "port")(
      "config", po::value<std::string>(), "bot config file")(
      "id", po::value<std::string>()->default_value(""), "bot id");

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);

  if (vm.count("help") || argc == 1) {
    std::cout << desc << "\n";
    exit(1);
  }

  if (!vm.count("endpoint")) {
    std::cerr << "Missing --endpoint argument"
              << "\n";
    exit(1);
  }

  if (!vm.count("appkey")) {
    std::cerr << "Missing --appkey argument"
              << "\n";
    exit(1);
  }

  if (!vm.count("channel")) {
    std::cerr << "Missing --channel argument"
              << "\n";
    exit(1);
  }

  if (!vm.count("port")) {
    std::cerr << "Missing --port argument"
              << "\n";
    exit(1);
  }

  return vm;
}

int bot_environment::main(int argc, char* argv[]) {
  auto cmd_args = parse_command_line(argc, argv);
  const std::string channel = cmd_args["channel"].as<std::string>();
  const std::string id = cmd_args["id"].as<std::string>();
  _bot_instance.reset(new bot_instance(id, *_bot_descriptor, channel, *this));

  if (_bot_descriptor->ctrl_callback) {
    cbor_item_t* config;

    if (cmd_args.count("config")) {
      FILE* fp = fopen(cmd_args["config"].as<std::string>().c_str(), "r");
      assert(fp);
      auto file_closer = gsl::finally([&fp]() { fclose(fp); });

      char readBuffer[65536];
      rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
      rapidjson::Document d;
      d.ParseStream(is);

      config = json_to_cbor(d);
    } else {
      config = cbor_new_definite_map(0);
    }
    cbor_item_t* cmd = configure_command(config);
    auto cbor_deleter = gsl::finally([&config, &cmd]() {
      cbor_decref(&config);
      cbor_decref(&cmd);
    });

    cbor_item_t* response = _bot_descriptor->ctrl_callback(*_bot_instance, cmd);
    if (response != nullptr) {
      _bot_instance->queue_message(bot_message_kind::DEBUG, response);
      cbor_decref(&response);
    }
  } else if (cmd_args.count("config")) {
    std::cerr << "Config specified but there is no control method set\n";
  }

  const std::string endpoint = cmd_args["endpoint"].as<std::string>();
  const std::string appkey = cmd_args["appkey"].as<std::string>();
  const std::string port = cmd_args["port"].as<std::string>();

  decoder_init_library();
  boost::asio::io_service io_service;
  boost::asio::ssl::context ssl_context{asio::ssl::context::sslv23};

  boost::asio::signal_set signals(io_service);
  signals.add(SIGINT);
  signals.add(SIGTERM);
  signals.add(SIGQUIT);
  signals.async_wait(boost::bind(&boost::asio::io_service::stop, &io_service));

  while (true) {
    try {
      auto rtm_client_factory = [&endpoint, &port, &appkey, &io_service,
                                 &ssl_context, this]() {
        return rtm::new_client(endpoint, port, appkey, io_service, ssl_context,
                               1, *this);
      };
      if (io_loop(rtm_client_factory, io_service)) {
        break;
      }
    } catch (const boost::system::system_error& e) {
      std::cerr << "Error: " << e.code() << '\n' << e.what() << '\n';
      if (e.code() != boost::system::errc::broken_pipe) break;  // Broken Pipe
    } catch (const bot_api_exception& e) {
      std::cerr << "Bot API Exception\n";
    }
  }

  return 0;
}

}  // namespace video
}  // namespace rtm

void rtm_video_bot_message(bot_context& ctx, const bot_message_kind kind,
                           cbor_item_t* message) {
  BOOST_ASSERT_MSG(cbor_map_is_indefinite(message),
                   "Message must be indefinite map");
  static_cast<rtm::video::bot_instance&>(ctx).queue_message(kind, message);
}

void rtm_video_bot_register(const bot_descriptor& bot) {
  rtm::video::bot_environment::instance().register_bot(&bot);
}

int rtm_video_bot_main(int argc, char* argv[]) {
  return rtm::video::bot_environment::instance().main(argc, argv);
}
