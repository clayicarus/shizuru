// Property-based tests for AgentRuntime
// Feature: runtime-io-redesign
// Uses RapidCheck + Google Test

// All includes MUST be at file scope, never inside a namespace block.
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "io/data_frame.h"
#include "io/io_device.h"
#include "runtime/agent_runtime.h"
#include "runtime/route_table.h"
#include "mock_io_device.h"

// ---------------------------------------------------------------------------
// MinimalMockLlmServer — file-scope, no namespace
// Returns a plain kResponse for every POST to /v1/chat/completions.
// ---------------------------------------------------------------------------
class MinimalMockLlmServer {
 public:
  MinimalMockLlmServer() {
    server_.Post("/v1/chat/completions",
                 [](const httplib::Request&, httplib::Response& res) {
                   nlohmann::json msg;
                   msg["role"] = "assistant";
                   msg["content"] = "ok";
                   nlohmann::json choice;
                   choice["index"] = 0;
                   choice["message"] = msg;
                   choice["finish_reason"] = "stop";
                   nlohmann::json usage;
                   usage["prompt_tokens"] = 1;
                   usage["completion_tokens"] = 1;
                   nlohmann::json resp;
                   resp["id"] = "mock";
                   resp["object"] = "chat.completion";
                   resp["choices"] = nlohmann::json::array({choice});
                   resp["usage"] = usage;
                   res.set_content(resp.dump(), "application/json");
                 });
    port_ = server_.bind_to_any_port("127.0.0.1");
    thread_ = std::thread([this] { server_.listen_after_bind(); });
    for (int i = 0; i < 100; ++i) {
      httplib::Client cli("http://127.0.0.1:" + std::to_string(port_));
      cli.set_connection_timeout(std::chrono::milliseconds(50));
      if (cli.Get("/healthz")) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  ~MinimalMockLlmServer() {
    server_.stop();
    if (thread_.joinable()) thread_.join();
  }
  std::string BaseUrl() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

 private:
  httplib::Server server_;
  int port_;
  std::thread thread_;
};

// ---------------------------------------------------------------------------
// All property tests live inside the shizuru::runtime anonymous namespace.
// ---------------------------------------------------------------------------
namespace shizuru::runtime {
namespace {

using testing::MockIoDevice;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

RuntimeConfig MakeConfig() {
  RuntimeConfig cfg;
  cfg.controller.max_turns = 1;
  cfg.controller.max_retries = 0;
  cfg.controller.retry_base_delay = std::chrono::milliseconds(1);
  cfg.controller.turn_timeout = std::chrono::seconds(5);
  cfg.controller.token_budget = 100000;
  cfg.controller.action_count_limit = 10;
  cfg.context.max_context_tokens = 100000;
  return cfg;
}

io::DataFrame MakeTextFrame(const std::string& text) {
  io::DataFrame f;
  f.type = "text/plain";
  f.payload = std::vector<uint8_t>(text.begin(), text.end());
  f.source_device = "test";
  f.source_port = "out";
  f.timestamp = std::chrono::steady_clock::now();
  return f;
}

// ---------------------------------------------------------------------------
// Property 2: Device Lifecycle Controls Frame Processing
// Feature: runtime-io-redesign, Property 2
// Validates: Requirements 1.6, 1.7, 1.8
// ---------------------------------------------------------------------------
RC_GTEST_PROP(AgentRuntimePropTest, prop_device_lifecycle_controls_processing,
              ()) {
  const std::string payload = *rc::gen::nonEmpty(
      rc::gen::container<std::string>(rc::gen::inRange('a', 'z')));

  auto dev = std::make_unique<MockIoDevice>("mock_dev");
  MockIoDevice* dev_ptr = dev.get();

  dev_ptr->Start();
  dev_ptr->OnInput("port_in", MakeTextFrame(payload));
  RC_ASSERT(dev_ptr->ReceivedCount() == 1u);

  dev_ptr->Stop();
  dev_ptr->OnInput("port_in", MakeTextFrame(payload));
  RC_ASSERT(dev_ptr->ReceivedCount() == 1u);  // still 1, not 2
}

// ---------------------------------------------------------------------------
// Property 11: Zero Transformation Invariant
// Feature: runtime-io-redesign, Property 11
// Validates: Requirements 8.1
// ---------------------------------------------------------------------------
RC_GTEST_PROP(AgentRuntimePropTest, prop_zero_transformation_invariant, ()) {
  const std::string type_tag = *rc::gen::nonEmpty(
      rc::gen::container<std::string>(rc::gen::inRange('a', 'z')));
  const std::string payload_str = *rc::gen::container<std::string>(
      rc::gen::inRange('\x20', '\x7e'));
  const std::string meta_key = *rc::gen::nonEmpty(
      rc::gen::container<std::string>(rc::gen::inRange('a', 'z')));
  const std::string meta_val = *rc::gen::container<std::string>(
      rc::gen::inRange('a', 'z'));

  io::DataFrame original;
  original.type = type_tag;
  original.payload = std::vector<uint8_t>(payload_str.begin(), payload_str.end());
  original.source_device = "src_dev";
  original.source_port = "src_port";
  original.timestamp = std::chrono::steady_clock::now();
  original.metadata[meta_key] = meta_val;

  RouteTable table;
  table.AddRoute(PortAddress{"src_dev", "src_port"},
                 PortAddress{"dst_dev", "dst_port"},
                 RouteOptions{.requires_control_plane = false});

  auto dst = std::make_unique<MockIoDevice>("dst_dev");
  MockIoDevice* dst_ptr = dst.get();
  dst_ptr->Start();

  auto destinations = table.Lookup(PortAddress{"src_dev", "src_port"});
  RC_ASSERT(!destinations.empty());

  dst_ptr->OnInput("dst_port", original);

  auto received = dst_ptr->ReceivedFrames();
  RC_ASSERT(received.size() == 1u);

  const io::DataFrame& delivered = received[0].second;
  RC_ASSERT(delivered.type == original.type);
  RC_ASSERT(delivered.payload == original.payload);
  RC_ASSERT(delivered.source_device == original.source_device);
  RC_ASSERT(delivered.source_port == original.source_port);
  RC_ASSERT(delivered.metadata == original.metadata);
}

// ---------------------------------------------------------------------------
// Property 12: Reverse-Order Shutdown
// Feature: runtime-io-redesign, Property 12
// Validates: Requirements 8.6
// ---------------------------------------------------------------------------
RC_GTEST_PROP(AgentRuntimePropTest, prop_reverse_order_shutdown, ()) {
  int n = *rc::gen::inRange(2, 6);

  std::vector<std::string> stop_order;
  std::mutex stop_mu;
  std::vector<std::string> registration_order;
  std::vector<std::unique_ptr<MockIoDevice>> devices;

  for (int i = 0; i < n; ++i) {
    std::string id = "dev_" + std::to_string(i);
    registration_order.push_back(id);
    auto dev = std::make_unique<MockIoDevice>(id);
    dev->Start();
    devices.push_back(std::move(dev));
  }

  for (auto it = registration_order.rbegin(); it != registration_order.rend();
       ++it) {
    for (auto& dev : devices) {
      if (dev->GetDeviceId() == *it) {
        dev->Stop();
        std::lock_guard<std::mutex> lock(stop_mu);
        stop_order.push_back(*it);
        break;
      }
    }
  }

  RC_ASSERT(static_cast<int>(stop_order.size()) == n);
  for (int i = 0; i < n; ++i) {
    RC_ASSERT(stop_order[i] == registration_order[n - 1 - i]);
  }
}

// ---------------------------------------------------------------------------
// Property 1: Device ID Uniqueness
// Feature: runtime-io-redesign, Property 1
// Validates: Requirements 1.4
// ---------------------------------------------------------------------------
RC_GTEST_PROP(AgentRuntimePropTest, prop_device_id_uniqueness, ()) {
  const std::string id = *rc::gen::nonEmpty(
      rc::gen::container<std::string>(rc::gen::inRange('a', 'z')));

  services::ToolRegistry tools;
  AgentRuntime runtime(MakeConfig(), tools);

  auto dev1 = std::make_unique<MockIoDevice>(id);
  auto dev2 = std::make_unique<MockIoDevice>(id);

  runtime.RegisterDevice(std::move(dev1));

  bool threw = false;
  try {
    runtime.RegisterDevice(std::move(dev2));
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  RC_ASSERT(threw);
}

// ---------------------------------------------------------------------------
// Property 13: SendMessage Routes to CoreDevice
// Feature: runtime-io-redesign, Property 13
// Validates: Requirements 11.6
// ---------------------------------------------------------------------------
RC_GTEST_PROP(AgentRuntimePropTest, prop_send_message_routes_to_core_device,
              ()) {
  const std::string content = *rc::gen::nonEmpty(
      rc::gen::container<std::string>(rc::gen::inRange('a', 'z')));

  MinimalMockLlmServer mock_server;

  services::ToolRegistry tools;
  RuntimeConfig cfg = MakeConfig();
  cfg.llm.base_url = mock_server.BaseUrl();
  cfg.llm.api_key = "mock";
  cfg.llm.model = "mock";
  cfg.llm.connect_timeout = std::chrono::seconds(5);
  cfg.llm.read_timeout = std::chrono::seconds(10);

  AgentRuntime runtime(cfg, tools);

  std::mutex mu;
  std::string received_text;
  runtime.OnOutput([&](const RuntimeOutput& out) {
    std::lock_guard<std::mutex> lock(mu);
    received_text = out.text;
  });

  runtime.StartSession();
  runtime.SendMessage(content);

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  bool got_response = false;
  while (std::chrono::steady_clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> lock(mu);
      if (!received_text.empty()) { got_response = true; break; }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  runtime.Shutdown();
  RC_ASSERT(got_response);
}

}  // namespace
}  // namespace shizuru::runtime
