#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "app/assembly/app_runtime.h"
#include "core/content_part.h"
#include "core/conversation_item.h"
#include "runtime/port_payload_kind.h"
#include "tests/runtime/mock_io_device.h"

namespace shizuru::app {
namespace {

TEST(AppRuntimeInput, TypedConversationItemInputReachesObservers) {
  AppConfig config;
  config.user_id = "typed-ui-user";
  config.controller.token_budget = 0;

  AppRuntime app(config);
  app.Bus().RegisterDevice(std::make_unique<runtime::testing::MockIoDevice>(
      "elevenlabs_tts",
      std::vector<io::PortDescriptor>{
          {"item_in", io::PortDirection::kInput, "",
           runtime::PortPayloadKind::kConversationItem},
          {"signal_in", io::PortDirection::kInput, "",
           runtime::PortPayloadKind::kControlSignal},
      }));
  app.Bus().RegisterDevice(std::make_unique<runtime::testing::MockIoDevice>(
      "baidu_asr",
      std::vector<io::PortDescriptor>{
          {"signal_in", io::PortDirection::kInput, "",
           runtime::PortPayloadKind::kControlSignal},
      }));
  app.Bus().RegisterDevice(std::make_unique<runtime::testing::MockIoDevice>(
      "audio_playout",
      std::vector<io::PortDescriptor>{
          {"signal_in", io::PortDirection::kInput, "",
           runtime::PortPayloadKind::kControlSignal},
      }));
  app.Bus().RegisterDevice(std::make_unique<runtime::testing::MockIoDevice>(
      "vad",
      std::vector<io::PortDescriptor>{
          {"audio_out", io::PortDirection::kOutput, "audio/pcm",
           runtime::PortPayloadKind::kAudioFrame},
          {"control_signal_out", io::PortDirection::kOutput, "",
           runtime::PortPayloadKind::kControlSignal},
      }));

  app.Start();

  core::ConversationItem item;
  item.item_id = "ui:test:1";
  item.conversation_id = "typed-ui-user";
  item.kind = core::ConversationItemKind::kUserMessage;
  item.actor = {"local-user", "You", core::ActorKind::kHuman};
  item.parts.emplace_back(core::TextPart{"hello from typed input"});
  item.wall_time = std::chrono::system_clock::now();

  app.SendConversationItem(item);

  const auto state = app.GetState();
  app.Shutdown();

  EXPECT_NE(state, core::State::kIdle);
}

}  // namespace
}  // namespace shizuru::app
