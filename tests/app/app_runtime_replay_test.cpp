#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "app/assembly/app_runtime.h"
#include "app/memory/sqlite_memory_store.h"
#include "core/content_part.h"
#include "core/conversation_item.h"
#include "tests/runtime/mock_io_device.h"
#include "runtime/port_payload_kind.h"

namespace shizuru::app {
namespace {

TEST(AppRuntimeReplay, ReplaysRecentHistoryOnStart) {
  const std::string db_path =
      "/private/tmp/shizuru_app_runtime_replay_test_" +
      std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) +
      ".db";
  const std::string session_id = "replay-user";

  {
    SqliteMemoryStore store(db_path);

    core::ConversationItem user;
    user.item_id = "u1";
    user.conversation_id = session_id;
    user.kind = core::ConversationItemKind::kUserMessage;
    user.actor = {"user", "User", core::ActorKind::kHuman};
    user.parts.emplace_back(core::TextPart{"hello"});
    user.wall_time = std::chrono::system_clock::now();
    store.Append(session_id, user);

    core::ConversationItem assistant;
    assistant.item_id = "a1";
    assistant.conversation_id = session_id;
    assistant.kind = core::ConversationItemKind::kAssistantMessage;
    assistant.actor = {"assistant", "Assistant", core::ActorKind::kAssistant};
    assistant.parts.emplace_back(core::TextPart{"hi"});
    assistant.wall_time = std::chrono::system_clock::now();
    store.Append(session_id, assistant);
  }

  AppConfig config;
  config.user_id = session_id;
  config.db_path = db_path;

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

  ASSERT_NE(app.Bus().FindDevice("elevenlabs_tts"), nullptr);
  ASSERT_NE(app.Bus().FindDevice("baidu_asr"), nullptr);
  ASSERT_NE(app.Bus().FindDevice("audio_playout"), nullptr);
  ASSERT_NE(app.Bus().FindDevice("vad"), nullptr);

  std::vector<core::ConversationItem> replayed;
  app.OnConversationItem([&](const core::ConversationItem& item, bool is_delta) {
    if (!is_delta) {
      replayed.push_back(item);
    }
  });

  app.Start();
  app.Shutdown();

  auto u1 = std::find_if(replayed.begin(), replayed.end(),
                         [](const core::ConversationItem& item) {
                           return item.item_id == "u1";
                         });
  auto a1 = std::find_if(replayed.begin(), replayed.end(),
                         [](const core::ConversationItem& item) {
                           return item.item_id == "a1";
                         });

  ASSERT_NE(u1, replayed.end());
  ASSERT_NE(a1, replayed.end());
  EXPECT_LT(std::distance(replayed.begin(), u1),
            std::distance(replayed.begin(), a1));
}

}  // namespace
}  // namespace shizuru::app
