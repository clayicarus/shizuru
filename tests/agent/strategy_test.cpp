// Unit tests for Controller strategies:
//   - ObservationFilter
//   - TtsSegmentStrategy
//   - ResponseFilter

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "context/config.h"
#include "context/context_strategy.h"
#include "controller/config.h"
#include "controller/controller.h"
#include "controller/types.h"
#include "io/data_frame.h"
#include "mock_audit_sink.h"
#include "mock_llm_client.h"
#include "mock_memory_store.h"
#include "policy/config.h"
#include "policy/policy_layer.h"
#include "strategies/observation_filter.h"
#include "strategies/response_filter.h"
#include "strategies/tts_segment_strategy.h"

namespace shizuru::core {
namespace {

bool WaitFor(std::function<bool()> pred, int timeout_ms = 2000) {
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

Observation MakeUserObs(const std::string& content) {
  Observation obs;
  obs.type = ObservationType::kUserMessage;
  obs.content = content;
  obs.source = "user";
  obs.timestamp = std::chrono::steady_clock::now();
  return obs;
}

ControllerConfig DefaultConfig() {
  ControllerConfig cfg;
  cfg.max_turns = 20;
  cfg.max_retries = 0;
  cfg.retry_base_delay = std::chrono::milliseconds(1);
  cfg.turn_timeout = std::chrono::seconds(5);
  cfg.token_budget = 100000;
  cfg.action_count_limit = 50;
  return cfg;
}

// =========================================================================
// PunctuationSegmentStrategy unit tests (no Controller needed)
// =========================================================================

TEST(PunctuationSegmentStrategyTest, FlushesOnSentenceEnd) {
  PunctuationSegmentStrategy strat;
  strat.Append("Hello world.");
  EXPECT_EQ(strat.ReadyLength(), 12u);  // "Hello world." = 12 chars
}

TEST(PunctuationSegmentStrategyTest, DoesNotFlushBelowMinChars) {
  PunctuationSegmentStrategy strat;
  strat.Append("Hi.");  // 3 chars < min_chars(10)
  EXPECT_EQ(strat.ReadyLength(), 0u);
}

TEST(PunctuationSegmentStrategyTest, ForceFlushAtMaxChars) {
  PunctuationSegmentStrategy::Config cfg;
  cfg.min_chars = 5;
  cfg.max_chars = 20;
  PunctuationSegmentStrategy strat(cfg);

  strat.Append("This is a long sentence without any punctuation at all");
  EXPECT_EQ(strat.ReadyLength(), 54u);  // > max_chars → flush all
}

TEST(PunctuationSegmentStrategyTest, ConsumeRemovesPrefix) {
  PunctuationSegmentStrategy strat;
  strat.Append("First sentence. Second part");
  size_t ready = strat.ReadyLength();
  // "First sentence." — period at index 14, ReadyLength returns 15.
  EXPECT_EQ(ready, 15u);

  strat.Consume(ready);

  // Remaining: " Second part" (leading space preserved)
  EXPECT_EQ(strat.ReadyLength(), 0u);  // no punctuation, below max
  std::string remaining = strat.Flush();
  EXPECT_EQ(remaining, " Second part");
}

TEST(PunctuationSegmentStrategyTest, FlushReturnsAllAndClears) {
  PunctuationSegmentStrategy strat;
  strat.Append("partial");
  std::string result = strat.Flush();
  EXPECT_EQ(result, "partial");
  EXPECT_EQ(strat.Flush(), "");  // already cleared
}

TEST(PunctuationSegmentStrategyTest, ResetClearsBuffer) {
  PunctuationSegmentStrategy strat;
  strat.Append("some text.");
  strat.Reset();
  EXPECT_EQ(strat.ReadyLength(), 0u);
  EXPECT_EQ(strat.Flush(), "");
}

TEST(PunctuationSegmentStrategyTest, QuestionMarkFlushes) {
  PunctuationSegmentStrategy strat;
  strat.Append("How are you?");
  EXPECT_GT(strat.ReadyLength(), 0u);
}

TEST(PunctuationSegmentStrategyTest, ExclamationMarkFlushes) {
  PunctuationSegmentStrategy strat;
  strat.Append("Watch out! Be careful");
  size_t ready = strat.ReadyLength();
  // "Watch out!" — '!' at index 9, ReadyLength returns 10.
  EXPECT_EQ(ready, 10u);
}

// =========================================================================
// StripThinkingFilter unit tests
// =========================================================================

TEST(StripThinkingFilterTest, PassesThroughNormalText) {
  StripThinkingFilter filter;
  EXPECT_EQ(filter.Filter("Hello world"), "Hello world");
}

TEST(StripThinkingFilterTest, StripsThinkingBlock) {
  StripThinkingFilter filter;
  EXPECT_EQ(filter.Filter("<think>reasoning</think>Hello"),
            "Hello");
}

TEST(StripThinkingFilterTest, StripsMultipleBlocks) {
  StripThinkingFilter filter;
  EXPECT_EQ(filter.Filter("A<think>x</think>B<think>y</think>C"),
            "ABC");
}

TEST(StripThinkingFilterTest, UnclosedTagStripsToEnd) {
  StripThinkingFilter filter;
  EXPECT_EQ(filter.Filter("Hello<think>unclosed"), "Hello");
}

TEST(StripThinkingFilterTest, EmptyInput) {
  StripThinkingFilter filter;
  EXPECT_EQ(filter.Filter(""), "");
}

// =========================================================================
// ObservationFilter integration test with Controller
// =========================================================================

// A filter that rejects observations containing "ignore".
class RejectIgnoreFilter : public ObservationFilter {
 public:
  std::atomic<int> reject_count{0};
  bool ShouldProcess(const Observation& obs) override {
    if (obs.content.find("ignore") != std::string::npos) {
      reject_count.fetch_add(1);
      return false;
    }
    return true;
  }
};

TEST(ObservationFilterIntegrationTest, FilteredObservationStaysInListening) {
  testing::MockMemoryStore memory;
  testing::MockAuditSink audit;
  ContextConfig ctx_cfg;
  ctx_cfg.max_context_tokens = 100000;
  ContextStrategy context(ctx_cfg, memory);
  context.InitSession("s1");
  PolicyConfig pol_cfg;
  PolicyLayer policy(pol_cfg, audit);
  policy.InitSession("s1");

  auto llm = std::make_unique<testing::MockLlmClient>();
  std::atomic<int> llm_calls{0};
  llm->submit_fn = [&](const ContextWindow&) -> LlmResult {
    llm_calls.fetch_add(1);
    LlmResult r;
    r.candidate.type = ActionType::kResponse;
    r.candidate.response_text = "ok";
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  auto filter = std::make_unique<RejectIgnoreFilter>();
  auto* filter_ptr = filter.get();

  Controller ctrl("s1", DefaultConfig(), std::move(llm),
                  nullptr, nullptr, context, policy,
                  nullptr,  // observation_aggregator
                  std::move(filter));

  ctrl.Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  // Send a filtered observation.
  ctrl.EnqueueObservation(MakeUserObs("please ignore this"));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Controller should still be in kListening — LLM was never called.
  EXPECT_EQ(ctrl.GetState(), State::kListening);
  EXPECT_EQ(llm_calls.load(), 0);
  EXPECT_EQ(filter_ptr->reject_count.load(), 1);

  // Send a valid observation — should be processed.
  ctrl.EnqueueObservation(MakeUserObs("hello"));
  ASSERT_TRUE(WaitFor([&] { return llm_calls.load() >= 1; }));

  ctrl.Shutdown();
}

// =========================================================================
// TtsSegmentStrategy integration test with Controller (streaming)
// =========================================================================

TEST(TtsSegmentIntegrationTest, StreamingTokensEmitTtsFrames) {
  testing::MockMemoryStore memory;
  testing::MockAuditSink audit;
  ContextConfig ctx_cfg;
  ctx_cfg.max_context_tokens = 100000;
  ContextStrategy context(ctx_cfg, memory);
  context.InitSession("s1");
  PolicyConfig pol_cfg;
  PolicyLayer policy(pol_cfg, audit);
  policy.InitSession("s1");

  auto llm = std::make_unique<testing::MockLlmClient>();

  // Override SubmitStreaming to actually call on_token with simulated tokens.
  llm->submit_fn = [](const ContextWindow&) -> LlmResult {
    LlmResult r;
    r.candidate.type = ActionType::kResponse;
    r.candidate.response_text = "Hello world. How are you?";
    r.prompt_tokens = 10;
    r.completion_tokens = 5;
    return r;
  };

  // We need a custom mock that calls on_token. Replace submit_fn approach
  // by subclassing directly.
  struct StreamingMockLlm : public LlmClient {
    LlmResult Submit(const ContextWindow&) override { return {}; }
    LlmResult SubmitStreaming(const ContextWindow&,
                              StreamCallback on_token) override {
      // Simulate streaming tokens.
      on_token("Hello ");
      on_token("world. ");
      on_token("How ");
      on_token("are ");
      on_token("you?");

      LlmResult r;
      r.candidate.type = ActionType::kResponse;
      r.candidate.response_text = "Hello world. How are you?";
      r.prompt_tokens = 10;
      r.completion_tokens = 5;
      return r;
    }
    void Cancel() override {}
  };

  auto streaming_llm = std::make_unique<StreamingMockLlm>();

  // Capture emitted frames.
  std::mutex mu;
  std::vector<io::DataFrame> tts_frames;
  Controller::EmitFrameCallback emit = [&](const std::string& port,
                                           io::DataFrame frame) {
    std::lock_guard<std::mutex> lock(mu);
    if (frame.metadata.count("tts_ready")) {
      tts_frames.push_back(std::move(frame));
    }
  };

  ControllerConfig cfg = DefaultConfig();
  cfg.use_streaming = true;

  auto tts_strat = std::make_unique<PunctuationSegmentStrategy>();

  Controller ctrl("s1", cfg, std::move(streaming_llm),
                  std::move(emit), nullptr, context, policy,
                  nullptr,  // observation_aggregator
                  nullptr,  // observation_filter
                  std::move(tts_strat),
                  nullptr);  // response_filter

  ctrl.Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  ctrl.EnqueueObservation(MakeUserObs("say something"));

  // Wait for TTS frames to be emitted.
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !tts_frames.empty();
  }));

  // Wait for controller to finish responding.
  ASSERT_TRUE(WaitFor([&] {
    return ctrl.GetState() == State::kListening ||
           ctrl.GetState() == State::kIdle;
  }));

  ctrl.Shutdown();

  std::lock_guard<std::mutex> lock(mu);
  // Should have at least one TTS frame (the sentence "Hello world." triggers
  // a flush, and "How are you?" triggers another flush on '?').
  ASSERT_GE(tts_frames.size(), 1u);

  // All TTS frames should have tts_ready metadata.
  for (const auto& f : tts_frames) {
    EXPECT_EQ(f.metadata.at("tts_ready"), "1");
  }

  // Concatenate all TTS frame payloads — should equal the full response.
  std::string combined;
  for (const auto& f : tts_frames) {
    combined.append(f.payload.begin(), f.payload.end());
  }
  EXPECT_EQ(combined, "Hello world. How are you?");
}

// =========================================================================
// ResponseFilter integration test with Controller
// =========================================================================

TEST(ResponseFilterIntegrationTest, ThinkingTagsStrippedFromResponse) {
  testing::MockMemoryStore memory;
  testing::MockAuditSink audit;
  ContextConfig ctx_cfg;
  ctx_cfg.max_context_tokens = 100000;
  ContextStrategy context(ctx_cfg, memory);
  context.InitSession("s1");
  PolicyConfig pol_cfg;
  PolicyLayer policy(pol_cfg, audit);
  policy.InitSession("s1");

  auto llm = std::make_unique<testing::MockLlmClient>();
  llm->submit_fn = [](const ContextWindow&) -> LlmResult {
    LlmResult r;
    r.candidate.type = ActionType::kResponse;
    r.candidate.response_text = "<think>internal reasoning</think>The answer is 42.";
    r.prompt_tokens = 10;
    r.completion_tokens = 5;
    return r;
  };

  // Capture responses.
  std::mutex mu;
  std::vector<std::string> responses;

  Controller ctrl("s1", DefaultConfig(), std::move(llm),
                  nullptr, nullptr, context, policy,
                  nullptr,  // observation_aggregator
                  nullptr,  // observation_filter
                  nullptr,  // tts_segment
                  std::make_unique<StripThinkingFilter>());

  ctrl.OnResponse([&](const ActionCandidate& ac) {
    std::lock_guard<std::mutex> lock(mu);
    responses.push_back(ac.response_text);
  });

  ctrl.Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  ctrl.EnqueueObservation(MakeUserObs("what is the answer?"));

  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !responses.empty();
  }));

  ctrl.Shutdown();

  std::lock_guard<std::mutex> lock(mu);
  ASSERT_EQ(responses.size(), 1u);
  EXPECT_EQ(responses[0], "The answer is 42.");
}

// =========================================================================
// ResponseFilter: empty response after filtering is suppressed
// =========================================================================

TEST(ResponseFilterIntegrationTest, EmptyResponseSuppressed) {
  testing::MockMemoryStore memory;
  testing::MockAuditSink audit;
  ContextConfig ctx_cfg;
  ctx_cfg.max_context_tokens = 100000;
  ContextStrategy context(ctx_cfg, memory);
  context.InitSession("s1");
  PolicyConfig pol_cfg;
  PolicyLayer policy(pol_cfg, audit);
  policy.InitSession("s1");

  auto llm = std::make_unique<testing::MockLlmClient>();
  llm->submit_fn = [](const ContextWindow&) -> LlmResult {
    LlmResult r;
    r.candidate.type = ActionType::kResponse;
    r.candidate.response_text = "<think>only thinking</think>";
    r.prompt_tokens = 10;
    r.completion_tokens = 5;
    return r;
  };

  std::mutex mu;
  std::vector<std::string> responses;

  Controller ctrl("s1", DefaultConfig(), std::move(llm),
                  nullptr, nullptr, context, policy,
                  nullptr, nullptr, nullptr,
                  std::make_unique<StripThinkingFilter>());

  ctrl.OnResponse([&](const ActionCandidate& ac) {
    std::lock_guard<std::mutex> lock(mu);
    responses.push_back(ac.response_text);
  });

  ctrl.Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  ctrl.EnqueueObservation(MakeUserObs("think about it"));

  // Wait for controller to return to Listening (response was suppressed).
  ASSERT_TRUE(WaitFor([&] {
    return ctrl.GetState() == State::kListening;
  }));

  // Give a moment for any callbacks to fire.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  ctrl.Shutdown();

  std::lock_guard<std::mutex> lock(mu);
  // OnResponse should NOT have been called (response was empty after filtering).
  EXPECT_TRUE(responses.empty());
}

// ---------------------------------------------------------------------------
// TTS Thinking Filter: <think> content must NOT reach TTS
// ---------------------------------------------------------------------------
TEST(StrategyTest, TtsThinkingFilter_ThinkingContentExcludedFromTts) {
  testing::MockMemoryStore memory;
  testing::MockAuditSink audit;
  ContextConfig ctx_cfg;
  ctx_cfg.max_context_tokens = 100000;
  ContextStrategy context(ctx_cfg, memory);
  context.InitSession("test-session");
  PolicyConfig pol_cfg;
  PolicyLayer policy(pol_cfg, audit);
  policy.InitSession("test-session");

  // Custom streaming LLM that emits tokens with <think> blocks.
  struct ThinkingStreamLlm : public LlmClient {
    LlmResult Submit(const ContextWindow&) override { return {}; }
    LlmResult SubmitStreaming(const ContextWindow&,
                              StreamCallback on_token) override {
      on_token("<think>");
      on_token("internal reasoning");
      on_token("</think>");
      on_token("Hello ");
      on_token("world.");

      LlmResult r;
      r.candidate.type = ActionType::kResponse;
      r.candidate.response_text =
          "<think>internal reasoning</think>Hello world.";
      r.prompt_tokens = 10;
      r.completion_tokens = 5;
      return r;
    }
    void Cancel() override {}
  };

  std::mutex mu;
  std::vector<io::DataFrame> tts_frames;
  Controller::EmitFrameCallback emit = [&](const std::string& port,
                                           io::DataFrame frame) {
    if (port == "tts_out") {
      std::lock_guard<std::mutex> lock(mu);
      tts_frames.push_back(std::move(frame));
    }
  };

  ControllerConfig cfg;
  cfg.max_turns = 20;
  cfg.max_retries = 0;
  cfg.retry_base_delay = std::chrono::milliseconds(1);
  cfg.turn_timeout = std::chrono::seconds(5);
  cfg.token_budget = 100000;
  cfg.action_count_limit = 50;
  cfg.use_streaming = true;

  auto tts_strat = std::make_unique<PunctuationSegmentStrategy>();

  Controller ctrl("test-session", cfg,
                  std::make_unique<ThinkingStreamLlm>(),
                  std::move(emit), nullptr, context, policy,
                  nullptr, nullptr, std::move(tts_strat), nullptr);

  ctrl.Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  ctrl.EnqueueObservation(MakeUserObs("say something"));

  // Wait for TTS frames to be emitted.
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !tts_frames.empty();
  }, 3000)) << "No TTS frames emitted";

  // Wait for controller to finish responding.
  ASSERT_TRUE(WaitFor([&] {
    return ctrl.GetState() == State::kListening ||
           ctrl.GetState() == State::kIdle;
  }, 3000));

  ctrl.Shutdown();

  std::lock_guard<std::mutex> lock(mu);

  // Concatenate all TTS payloads — must NOT contain thinking content.
  std::string combined;
  for (const auto& f : tts_frames) {
    combined.append(f.payload.begin(), f.payload.end());
  }
  EXPECT_EQ(combined, "Hello world.");
  EXPECT_EQ(combined.find("internal reasoning"), std::string::npos);
  EXPECT_EQ(combined.find("<think>"), std::string::npos);
}

// ---------------------------------------------------------------------------
// TTS Filter: <tool_call> and <tool_result> tags excluded from TTS
// ---------------------------------------------------------------------------
TEST(StrategyTest, TtsFilter_ToolCallAndResultTagsExcludedFromTts) {
  testing::MockMemoryStore memory;
  testing::MockAuditSink audit;
  ContextConfig ctx_cfg;
  ctx_cfg.max_context_tokens = 100000;
  ContextStrategy context(ctx_cfg, memory);
  context.InitSession("tts-filter-session");
  PolicyConfig pol_cfg;
  PolicyLayer policy(pol_cfg, audit);
  policy.InitSession("tts-filter-session");

  // Streaming LLM that emits tokens with think, tool_call, and tool_result.
  struct ToolTagStreamLlm : public LlmClient {
    LlmResult Submit(const ContextWindow&) override { return {}; }
    LlmResult SubmitStreaming(const ContextWindow&,
                              StreamCallback on_token) override {
      on_token("<think>reasoning</think>");
      on_token("<tool_call>{\"name\":\"test\"}</tool_call>");
      on_token("<tool_result>{\"success\":true}</tool_result>");
      on_token("Final answer.");

      LlmResult r;
      r.candidate.type = ActionType::kResponse;
      r.candidate.response_text =
          "<think>reasoning</think>"
          "<tool_call>{\"name\":\"test\"}</tool_call>"
          "<tool_result>{\"success\":true}</tool_result>"
          "Final answer.";
      r.prompt_tokens = 10;
      r.completion_tokens = 5;
      return r;
    }
    void Cancel() override {}
  };

  std::mutex mu;
  std::vector<io::DataFrame> tts_frames;
  Controller::EmitFrameCallback emit = [&](const std::string& port,
                                           io::DataFrame frame) {
    if (port == "tts_out") {
      std::lock_guard<std::mutex> lock(mu);
      tts_frames.push_back(std::move(frame));
    }
  };

  ControllerConfig cfg;
  cfg.max_turns = 20;
  cfg.max_retries = 0;
  cfg.retry_base_delay = std::chrono::milliseconds(1);
  cfg.turn_timeout = std::chrono::seconds(5);
  cfg.token_budget = 100000;
  cfg.action_count_limit = 50;
  cfg.use_streaming = true;

  auto tts_strat = std::make_unique<PunctuationSegmentStrategy>();

  Controller ctrl("tts-filter-session", cfg,
                  std::make_unique<ToolTagStreamLlm>(),
                  std::move(emit), nullptr, context, policy,
                  nullptr, nullptr, std::move(tts_strat), nullptr);

  ctrl.Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  ctrl.EnqueueObservation(MakeUserObs("do something"));

  // Wait for TTS frames to be emitted.
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !tts_frames.empty();
  }, 3000)) << "No TTS frames emitted";

  // Wait for controller to finish.
  ASSERT_TRUE(WaitFor([&] {
    return ctrl.GetState() == State::kListening ||
           ctrl.GetState() == State::kIdle;
  }, 3000));

  ctrl.Shutdown();

  std::lock_guard<std::mutex> lock(mu);

  // Concatenate all TTS payloads.
  std::string combined;
  for (const auto& f : tts_frames) {
    combined.append(f.payload.begin(), f.payload.end());
  }

  // TTS must contain ONLY "Final answer." — no think/tool_call/tool_result.
  EXPECT_EQ(combined, "Final answer.");
  EXPECT_EQ(combined.find("<think>"), std::string::npos);
  EXPECT_EQ(combined.find("<tool_call>"), std::string::npos);
  EXPECT_EQ(combined.find("<tool_result>"), std::string::npos);
  EXPECT_EQ(combined.find("reasoning"), std::string::npos);
}

// ---------------------------------------------------------------------------
// StripThinkingFilter: strips all tag types
// ---------------------------------------------------------------------------
TEST(StripThinkingFilterTest, StripsAllTagTypes) {
  StripThinkingFilter filter;
  std::string input =
      "<think>reasoning</think>Hello<tool_call>json</tool_call>"
      " world<tool_result>result</tool_result>!";
  EXPECT_EQ(filter.Filter(input), "Hello world!");
}

}  // namespace
}  // namespace shizuru::core
