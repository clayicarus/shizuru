#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "app/assembly/app_config.h"
#include "app/assembly/app_runtime.h"
#include "app/memory/sqlite_memory_store.h"
#include "core/conversation/item.h"

namespace shizuru::app {
namespace {

constexpr size_t kExpectedReplayLimit = 256;

std::filesystem::path UniqueDbPath(const std::string& name) {
  return std::filesystem::temp_directory_path() /
         (name + "-" +
          std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count()) +
          ".sqlite3");
}

core::MemoryEntry MakeEntry(core::MemoryEntryType type,
                            const core::conversation::ConversationItem& item) {
  core::MemoryEntry entry;
  entry.type = type;
  entry.item = item;
  entry.timestamp = std::chrono::steady_clock::now();
  entry.estimated_tokens = 8;
  return entry;
}

TEST(AppRuntimeTest, StartReplaysPersistedConversationItemsToUi) {
  const std::string user_id = "history-user";
  const auto db_path = UniqueDbPath("app-runtime-history");

  {
    SqliteMemoryStore store(db_path.string());
    auto user_item = core::conversation::MakeHumanMessageItem(
        "user", "", "hello from disk");
    user_item.item_id = "user-1";

    auto assistant_item = core::conversation::MakeAssistantMessageItem(
        "assistant", "Shizuru", "hello from runtime");
    assistant_item.item_id = "assistant-1";
    assistant_item.turn_group_id = "turn-1";

    store.Append(
        user_id,
        MakeEntry(core::MemoryEntryType::kUserMessage, user_item));
    store.Append(
        user_id,
        MakeEntry(core::MemoryEntryType::kAssistantMessage, assistant_item));
  }

  AppConfig config;
  config.user_id = user_id;
  config.db_path = db_path.string();

  AppRuntime runtime(config);

  std::vector<core::conversation::ConversationItem> replayed;
  runtime.OnConversationItem(
      [&](const core::conversation::ConversationItem& item, bool is_delta) {
        EXPECT_FALSE(is_delta);
        replayed.push_back(item);
      });

  runtime.Start();
  runtime.Shutdown();

  ASSERT_EQ(replayed.size(), 2u);
  EXPECT_EQ(replayed[0].kind, core::conversation::ItemKind::kHumanMessage);
  EXPECT_EQ(replayed[0].payload.value("text", ""), "hello from disk");
  EXPECT_EQ(replayed[1].kind, core::conversation::ItemKind::kAssistantMessage);
  EXPECT_EQ(replayed[1].payload.value("text", ""), "hello from runtime");

  {
    SqliteMemoryStore reopened(db_path.string());
    ASSERT_EQ(reopened.GetAll(user_id).size(), 2u);
  }

  std::filesystem::remove(db_path);
}

TEST(AppRuntimeTest, StartReplaysOnlyRecentCommittedWindowToUi) {
  const std::string user_id = "history-window-user";
  const auto db_path = UniqueDbPath("app-runtime-history-window");

  {
    SqliteMemoryStore store(db_path.string());
    for (size_t i = 0; i < kExpectedReplayLimit + 20; ++i) {
      auto item = core::conversation::MakeHumanMessageItem(
          "user", "", "msg-" + std::to_string(i));
      item.item_id = "user-" + std::to_string(i);
      store.Append(
          user_id,
          MakeEntry(core::MemoryEntryType::kUserMessage, item));
    }
  }

  AppConfig config;
  config.user_id = user_id;
  config.db_path = db_path.string();

  AppRuntime runtime(config);

  std::vector<core::conversation::ConversationItem> replayed;
  runtime.OnConversationItem(
      [&](const core::conversation::ConversationItem& item, bool is_delta) {
        EXPECT_FALSE(is_delta);
        replayed.push_back(item);
      });

  runtime.Start();
  runtime.Shutdown();

  ASSERT_EQ(replayed.size(), kExpectedReplayLimit);
  EXPECT_EQ(replayed.front().payload.value("text", ""), "msg-20");
  EXPECT_EQ(
      replayed.back().payload.value("text", ""),
      "msg-" + std::to_string(kExpectedReplayLimit + 19));

  std::filesystem::remove(db_path);
}

}  // namespace
}  // namespace shizuru::app
