#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "core/content_part.h"
#include "core/control_signal.h"
#include "core/conversation_item.h"
#include "runtime/core_device.h"
#include "io/audio/audio_device/audio_frame.h"
#include "runtime/agent_runtime.h"
#include "runtime/port_payload_kind.h"
#include "runtime/tool_dispatch_device.h"
#include "services/audit/log_audit_sink.h"
#include "services/memory/in_memory_history.h"
#include "tests/agent/mocks/mock_llm_client.h"
#include "tests/agent/mocks/mock_memory_store.h"
#include "mock_io_device.h"

namespace shizuru::runtime {
namespace {

using testing::MockIoDevice;

PortAddress PA(const std::string& device_id, const std::string& port_name) {
  return PortAddress{device_id, port_name};
}

io::PortDescriptor PD(const std::string& name,
                      io::PortDirection direction,
                      PortPayloadKind kind) {
  io::PortDescriptor pd;
  pd.name = name;
  pd.direction = direction;
  pd.data_type = "";
  pd.payload_kind = kind;
  return pd;
}

TEST(AgentRuntimeTypedDispatch, ConversationItemRouteDeliversToTypedInput) {
  AgentRuntime runtime;

  auto src = std::make_unique<MockIoDevice>(
      "src",
      std::vector<io::PortDescriptor>{
          PD("item_out", io::PortDirection::kOutput,
             PortPayloadKind::kConversationItem),
      });
  auto dst = std::make_unique<MockIoDevice>(
      "dst",
      std::vector<io::PortDescriptor>{
          PD("item_in", io::PortDirection::kInput,
             PortPayloadKind::kConversationItem),
      });

  auto* src_ptr = src.get();
  auto* dst_ptr = dst.get();

  runtime.RegisterDevice(std::move(src));
  runtime.RegisterDevice(std::move(dst));
  runtime.AddRoute(PA("src", "item_out"), PA("dst", "item_in"));
  runtime.StartAll();

  core::ConversationItem item;
  item.item_id = "item1";
  item.conversation_id = "conv1";
  item.kind = core::ConversationItemKind::kUserMessage;
  item.actor = {"user1", "Alice", core::ActorKind::kHuman};
  item.parts.emplace_back(core::TextPart{"hello"});

  src_ptr->EmitConversationItemOutput("item_out", item);

  auto received = dst_ptr->ReceivedItems();
  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(received[0].first, "item_in");
  EXPECT_EQ(received[0].second.item_id, "item1");
  EXPECT_EQ(received[0].second.conversation_id, "conv1");
}

TEST(AgentRuntimeTypedDispatch, ControlSignalRouteDeliversToTypedInput) {
  AgentRuntime runtime;

  auto src = std::make_unique<MockIoDevice>(
      "src",
      std::vector<io::PortDescriptor>{
          PD("ctrl_out", io::PortDirection::kOutput,
             PortPayloadKind::kControlSignal),
      });
  auto dst = std::make_unique<MockIoDevice>(
      "dst",
      std::vector<io::PortDescriptor>{
          PD("ctrl_in", io::PortDirection::kInput,
             PortPayloadKind::kControlSignal),
      });

  auto* src_ptr = src.get();
  auto* dst_ptr = dst.get();

  runtime.RegisterDevice(std::move(src));
  runtime.RegisterDevice(std::move(dst));
  runtime.AddRoute(PA("src", "ctrl_out"), PA("dst", "ctrl_in"));
  runtime.StartAll();

  src_ptr->EmitControlSignalOutput("ctrl_out", core::CancelSignal{});

  auto received = dst_ptr->ReceivedSignals();
  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(received[0].first, "ctrl_in");
  EXPECT_TRUE(std::holds_alternative<core::CancelSignal>(received[0].second));
}

TEST(AgentRuntimeTypedDispatch, AudioFrameRouteDeliversToTypedInput) {
  AgentRuntime runtime;

  auto src = std::make_unique<MockIoDevice>(
      "src",
      std::vector<io::PortDescriptor>{
          PD("audio_out", io::PortDirection::kOutput,
             PortPayloadKind::kAudioFrame),
      });
  auto dst = std::make_unique<MockIoDevice>(
      "dst",
      std::vector<io::PortDescriptor>{
          PD("audio_in", io::PortDirection::kInput,
             PortPayloadKind::kAudioFrame),
      });

  auto* src_ptr = src.get();
  auto* dst_ptr = dst.get();

  runtime.RegisterDevice(std::move(src));
  runtime.RegisterDevice(std::move(dst));
  runtime.AddRoute(PA("src", "audio_out"), PA("dst", "audio_in"));
  runtime.StartAll();

  io::AudioFrame frame;
  frame.sample_rate = 16000;
  frame.channel_count = 1;
  frame.sample_count = 4;
  frame.data[0] = 1;
  frame.data[1] = 2;
  frame.data[2] = 3;
  frame.data[3] = 4;
  src_ptr->EmitAudioOutput("audio_out", frame);

  auto received = dst_ptr->ReceivedAudioFrames();
  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(received[0].first, "audio_in");
  EXPECT_EQ(received[0].second.sample_rate, 16000);
  ASSERT_EQ(received[0].second.sample_count, 4u);
}

TEST(AgentRuntimeTypedDispatch, AddRouteRejectsMismatchedPayloadKinds) {
  AgentRuntime runtime;

  auto src = std::make_unique<MockIoDevice>(
      "src",
      std::vector<io::PortDescriptor>{
          PD("item_out", io::PortDirection::kOutput,
             PortPayloadKind::kConversationItem),
      });
  auto dst = std::make_unique<MockIoDevice>(
      "dst",
      std::vector<io::PortDescriptor>{
          PD("audio_in", io::PortDirection::kInput,
             PortPayloadKind::kAudioFrame),
      });

  runtime.RegisterDevice(std::move(src));
  runtime.RegisterDevice(std::move(dst));

  EXPECT_THROW(
      runtime.AddRoute(PA("src", "item_out"), PA("dst", "audio_in")),
      std::invalid_argument);
}

TEST(AgentRuntimeTypedDispatch, AddRouteRejectsMissingPort) {
  AgentRuntime runtime;

  auto src = std::make_unique<MockIoDevice>(
      "src",
      std::vector<io::PortDescriptor>{
          PD("item_out", io::PortDirection::kOutput,
             PortPayloadKind::kConversationItem),
      });
  auto dst = std::make_unique<MockIoDevice>(
      "dst",
      std::vector<io::PortDescriptor>{
          PD("item_in", io::PortDirection::kInput,
             PortPayloadKind::kConversationItem),
      });

  runtime.RegisterDevice(std::move(src));
  runtime.RegisterDevice(std::move(dst));

  EXPECT_THROW(
      runtime.AddRoute(PA("src", "missing"), PA("dst", "item_in")),
      std::invalid_argument);
  EXPECT_THROW(
      runtime.AddRoute(PA("src", "item_out"), PA("dst", "missing")),
      std::invalid_argument);
}

TEST(AgentRuntimeTypedDispatch, AddRouteRejectsWrongDirections) {
  AgentRuntime runtime;

  auto src = std::make_unique<MockIoDevice>(
      "src",
      std::vector<io::PortDescriptor>{
          PD("item_in", io::PortDirection::kInput,
             PortPayloadKind::kConversationItem),
      });
  auto dst = std::make_unique<MockIoDevice>(
      "dst",
      std::vector<io::PortDescriptor>{
          PD("item_out", io::PortDirection::kOutput,
             PortPayloadKind::kConversationItem),
      });

  runtime.RegisterDevice(std::move(src));
  runtime.RegisterDevice(std::move(dst));

  EXPECT_THROW(
      runtime.AddRoute(PA("src", "item_in"), PA("dst", "item_out")),
      std::invalid_argument);
}

TEST(AgentRuntimeTypedDispatch, AppOutputRequiresMatchingVirtualPortForItem) {
  AgentRuntime runtime;

  auto src = std::make_unique<MockIoDevice>(
      "src",
      std::vector<io::PortDescriptor>{
          PD("item_out", io::PortDirection::kOutput,
             PortPayloadKind::kConversationItem),
      });

  runtime.RegisterDevice(std::move(src));

  EXPECT_NO_THROW(
      runtime.AddRoute(PA("src", "item_out"), PA("app_output", "item_in")));
  EXPECT_THROW(
      runtime.AddRoute(PA("src", "item_out"), PA("app_output", "frame_in")),
      std::invalid_argument);
}

TEST(AgentRuntimeTypedDispatch, AppOutputRequiresMatchingVirtualPortForControl) {
  AgentRuntime runtime;

  auto src = std::make_unique<MockIoDevice>(
      "src",
      std::vector<io::PortDescriptor>{
          PD("ctrl_out", io::PortDirection::kOutput,
             PortPayloadKind::kControlSignal),
      });

  runtime.RegisterDevice(std::move(src));

  EXPECT_NO_THROW(
      runtime.AddRoute(PA("src", "ctrl_out"), PA("app_output", "control_in")));
  EXPECT_THROW(
      runtime.AddRoute(PA("src", "ctrl_out"), PA("app_output", "item_in")),
      std::invalid_argument);
}

TEST(AgentRuntimeTypedDispatch, CoreAndToolDispatchExposeTypedPortsForRouting) {
  runtime::ToolRegistry registry;
  auto tool = std::make_unique<runtime::ToolDispatchDevice>(registry);
  auto item_src = std::make_unique<MockIoDevice>(
      "item_src",
      std::vector<io::PortDescriptor>{
          PD("item_out", io::PortDirection::kOutput,
             PortPayloadKind::kConversationItem),
      });
  auto ctrl_src = std::make_unique<MockIoDevice>(
      "ctrl_src",
      std::vector<io::PortDescriptor>{
          PD("ctrl_out", io::PortDirection::kOutput,
             PortPayloadKind::kControlSignal),
      });
  auto core = std::make_unique<runtime::CoreDevice>(
      "core", "session-1",
      core::ControllerConfig{},
      core::ContextConfig{},
      core::PolicyConfig{},
      std::make_unique<core::testing::MockLlmClient>(),
      std::make_unique<services::InMemoryHistory>(),
      std::make_unique<services::LogAuditSink>());

  AgentRuntime runtime;
  runtime.RegisterDevice(std::move(item_src));
  runtime.RegisterDevice(std::move(ctrl_src));
  runtime.RegisterDevice(std::move(core));
  runtime.RegisterDevice(std::move(tool));

  EXPECT_NO_THROW(
      runtime.AddRoute(PA("item_src", "item_out"), PA("core", "item_in")));
  EXPECT_NO_THROW(
      runtime.AddRoute(PA("core", "item_out"), PA("app_output", "item_in")));
  EXPECT_NO_THROW(
      runtime.AddRoute(PA("ctrl_src", "ctrl_out"), PA("tool_dispatch", "control_in")));
}

TEST(AgentRuntimeTypedDispatch, AppOutputControlSinkReceivesSignalFromTypedRoute) {
  AgentRuntime runtime;

  auto src = std::make_unique<MockIoDevice>(
      "src",
      std::vector<io::PortDescriptor>{
          PD("ctrl_out", io::PortDirection::kOutput,
             PortPayloadKind::kControlSignal),
      });
  auto* src_ptr = src.get();

  runtime.RegisterDevice(std::move(src));
  runtime.AddRoute(PA("src", "ctrl_out"), PA("app_output", "control_in"));

  std::mutex mu;
  std::vector<core::ControlSignal> signals;
  runtime.OnControlSignalSink([&](core::ControlSignal sig) {
    std::lock_guard<std::mutex> lock(mu);
    signals.push_back(std::move(sig));
  });

  runtime.StartAll();
  src_ptr->EmitControlSignalOutput(
      "ctrl_out",
      core::ToolCallStartSignal{"call_1", "search", R"({"q":"hello"})"});
  runtime.Shutdown();

  std::lock_guard<std::mutex> lock(mu);
  ASSERT_EQ(signals.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<core::ToolCallStartSignal>(signals[0]));
  const auto& sig = std::get<core::ToolCallStartSignal>(signals[0]);
  EXPECT_EQ(sig.tool_call_id, "call_1");
  EXPECT_EQ(sig.name, "search");
}

}  // namespace
}  // namespace shizuru::runtime
