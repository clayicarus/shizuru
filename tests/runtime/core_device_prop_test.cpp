// Property-based tests for CoreDevice
// Uses RapidCheck + Google Test

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "controller/config.h"
#include "context/config.h"
#include "policy/config.h"
#include "controller/types.h"
#include "context/types.h"
#include "io/data_frame.h"
#include "runtime/core_device.h"
#include "mock_audit_sink.h"
#include "mock_io_bridge.h"
#include "mock_llm_client.h"
#include "mock_memory_store.h"

namespace shizuru::runtime {
namespace {

// ---------------------------------------------------------------------------
// Helper: build a CoreDevice with mock dependencies
// ---------------------------------------------------------------------------

std::unique_ptr<CoreDevice> MakeCoreDevice(
    const std::string& device_id,
    core::testing::MockLlmClient** llm_out = nullptr,
    core::testing::MockIoBridge** io_out = nullptr) {
  auto llm = std::make_unique<core::testing::MockLlmClient>();
  auto io  = std::make_unique<core::testing::MockIoBridge>();
  if (llm_out) *llm_out = llm.get();
  if (io_out)  *io_out  = io.get();

  // Default LLM: return a kResponse to end the loop quickly.
  llm->submit_fn = [](const core::ContextWindow&) -> core::LlmResult {
    core::LlmResult r;
    r.candidate.type = core::ActionType::kResponse;
    r.candidate.response_text = "done";
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  core::ControllerConfig ctrl_cfg;
  ctrl_cfg.max_turns = 5;
  ctrl_cfg.max_retries = 0;
  ctrl_cfg.retry_base_delay = std::chrono::milliseconds(1);
  ctrl_cfg.wall_clock_timeout = std::chrono::seconds(5);
  ctrl_cfg.token_budget = 100000;
  ctrl_cfg.action_count_limit = 10;

  core::ContextConfig ctx_cfg;
  ctx_cfg.max_context_tokens = 100000;

  return std::make_unique<CoreDevice>(
      device_id, "test-session",
      ctrl_cfg, ctx_cfg, core::PolicyConfig{},
      std::move(llm), std::move(io),
      std::make_unique<core::testing::MockMemoryStore>(),
      std::make_unique<core::testing::MockAuditSink>());
}

// Wait up to `timeout_ms` for `predicate` to become true, polling every 5ms.
bool WaitFor(std::function<bool()> predicate, int timeout_ms = 300) {
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

// ---------------------------------------------------------------------------
// RapidCheck generators
// ---------------------------------------------------------------------------

rc::Gen<std::string> genShortAlpha() {
  return rc::gen::suchThat(
      rc::gen::container<std::string>(rc::gen::inRange('a', static_cast<char>('z' + 1))),
      [](const std::string& s) { return !s.empty() && s.size() <= 20; });
}

// Generate a port name that is NOT "text_in" or "tool_result_in".
rc::Gen<std::string> genUnsupportedPort() {
  return rc::gen::suchThat(genShortAlpha(), [](const std::string& s) {
    return s != "text_in" && s != "tool_result_in";
  });
}

// ---------------------------------------------------------------------------
// Property 6: CoreDevice Text-to-Observation Translation
// Feature: runtime-io-redesign, Property 6: CoreDevice Text-to-Observation Translation
// ---------------------------------------------------------------------------
// **Validates: Requirements 5.2**
RC_GTEST_PROP(CoreDevicePropTest, prop_text_to_observation_translation, ()) {
  const std::string text = *genShortAlpha();

  core::testing::MockLlmClient* llm = nullptr;
  auto device = MakeCoreDevice("core0", &llm);

  // Override LLM to record the ContextWindow it receives, then return kResponse.
  std::mutex mu;
  std::vector<core::ContextWindow> windows;
  llm->submit_fn = [&](const core::ContextWindow& cw) -> core::LlmResult {
    {
      std::lock_guard<std::mutex> lock(mu);
      windows.push_back(cw);
    }
    core::LlmResult r;
    r.candidate.type = core::ActionType::kResponse;
    r.candidate.response_text = "done";
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  device->SetOutputCallback([](const std::string&, const std::string&,
                                io::DataFrame) {});
  device->Start();

  io::DataFrame frame;
  frame.type = "text/plain";
  frame.payload = std::vector<uint8_t>(text.begin(), text.end());
  device->OnInput("text_in", std::move(frame));

  bool got_call = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !windows.empty();
  });

  device->Stop();

  RC_ASSERT(got_call);

  std::lock_guard<std::mutex> lock(mu);
  RC_ASSERT(!windows.empty());
  const auto& msgs = windows.front().messages;
  RC_ASSERT(!msgs.empty());
  // The last message in the context window must carry the input text.
  RC_ASSERT(msgs.back().content == text);
}

// ---------------------------------------------------------------------------
// Property 7: CoreDevice ActionCandidate-to-DataFrame Translation
// Feature: runtime-io-redesign, Property 7: CoreDevice ActionCandidate-to-DataFrame Translation
// ---------------------------------------------------------------------------
// **Validates: Requirements 5.3**
RC_GTEST_PROP(CoreDevicePropTest, prop_action_candidate_to_dataframe, ()) {
  const std::string response_text = *genShortAlpha();

  core::testing::MockLlmClient* llm = nullptr;
  auto device = MakeCoreDevice("core1", &llm);

  // LLM returns a kResponse with the generated text.
  llm->submit_fn = [&](const core::ContextWindow&) -> core::LlmResult {
    core::LlmResult r;
    r.candidate.type = core::ActionType::kResponse;
    r.candidate.response_text = response_text;
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  std::mutex mu;
  std::vector<io::DataFrame> emitted;
  device->SetOutputCallback([&](const std::string&, const std::string&,
                                 io::DataFrame f) {
    std::lock_guard<std::mutex> lock(mu);
    emitted.push_back(std::move(f));
  });

  device->Start();

  io::DataFrame trigger;
  trigger.type = "text/plain";
  const std::string trigger_text = "go";
  trigger.payload = std::vector<uint8_t>(trigger_text.begin(), trigger_text.end());
  device->OnInput("text_in", std::move(trigger));

  bool got_output = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !emitted.empty();
  });

  device->Stop();

  RC_ASSERT(got_output);

  std::lock_guard<std::mutex> lock(mu);
  bool found = false;
  for (const auto& f : emitted) {
    if (f.type == "text/plain") {
      const std::string payload_str(f.payload.begin(), f.payload.end());
      if (payload_str == response_text) {
        found = true;
        break;
      }
    }
  }
  RC_ASSERT(found);
}

// ---------------------------------------------------------------------------
// Property 8: CoreDevice Unsupported Type Discard
// Feature: runtime-io-redesign, Property 8: CoreDevice Unsupported Type Discard
// ---------------------------------------------------------------------------
// **Validates: Requirements 5.6**
RC_GTEST_PROP(CoreDevicePropTest, prop_unsupported_port_discarded, ()) {
  const std::string port = *genUnsupportedPort();

  auto device = MakeCoreDevice("core2");

  std::atomic<int> callback_count{0};
  device->SetOutputCallback([&](const std::string&, const std::string&,
                                 io::DataFrame) {
    ++callback_count;
  });

  device->Start();

  io::DataFrame frame;
  frame.type = "text/plain";
  const std::string payload = "data";
  frame.payload = std::vector<uint8_t>(payload.begin(), payload.end());

  // Must not throw.
  EXPECT_NO_THROW(device->OnInput(port, std::move(frame)));

  // Wait briefly to confirm no spurious output fires.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  device->Stop();

  // The unsupported port input must not trigger any output callback.
  RC_ASSERT(callback_count.load() == 0);
}

}  // namespace
}  // namespace shizuru::runtime
