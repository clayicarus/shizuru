#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

#include "app/memory/sqlite_memory_store.h"
#include "conversation/item.h"

namespace shizuru::app {
namespace {

std::filesystem::path UniqueDbPath(const std::string& name) {
  return std::filesystem::temp_directory_path() /
         (name + "-" +
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
          ".sqlite3");
}

core::MemoryEntry MakeEntry(core::MemoryEntryType type,
                            std::string role,
                            std::string content) {
  core::MemoryEntry entry;
  entry.type = type;
  entry.role = std::move(role);
  entry.content = content;
  entry.timestamp = std::chrono::steady_clock::now();
  entry.estimated_tokens = 3;

  // Populate the ConversationItem for message-like entries.
  switch (type) {
    case core::MemoryEntryType::kUserMessage:
      entry.item = core::conversation::MakeHumanMessageItem("user", "", content);
      break;
    case core::MemoryEntryType::kAssistantMessage:
      entry.item = core::conversation::MakeAssistantMessageItem("assistant", "", content);
      break;
    default:
      break;
  }
  return entry;
}

TEST(SqliteMemoryStoreTest, AppendAndGetRecentRoundTrip) {
  auto path = UniqueDbPath("memory-roundtrip");
  {
    SqliteMemoryStore store(path.string());
    store.Append("user-a", MakeEntry(core::MemoryEntryType::kUserMessage, "user", "hello"));
    store.Append("user-a", MakeEntry(core::MemoryEntryType::kAssistantMessage, "assistant", "world"));

    auto recent = store.GetRecent("user-a", 10);
    ASSERT_EQ(recent.size(), 2u);
    EXPECT_EQ(recent[0].content, "hello");
    EXPECT_EQ(recent[1].content, "world");
  }

  {
    SqliteMemoryStore reopened(path.string());
    auto all = reopened.GetAll("user-a");
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].content, "hello");
    EXPECT_EQ(all[1].content, "world");
  }

  std::filesystem::remove(path);
}

TEST(SqliteMemoryStoreTest, GetRecentReturnsNewestEntriesInChronologicalOrder) {
  auto path = UniqueDbPath("memory-recent");
  SqliteMemoryStore store(path.string());

  store.Append("user-a", MakeEntry(core::MemoryEntryType::kUserMessage, "user", "one"));
  store.Append("user-a", MakeEntry(core::MemoryEntryType::kUserMessage, "user", "two"));
  store.Append("user-a", MakeEntry(core::MemoryEntryType::kUserMessage, "user", "three"));

  auto recent = store.GetRecent("user-a", 2);
  ASSERT_EQ(recent.size(), 2u);
  EXPECT_EQ(recent[0].content, "two");
  EXPECT_EQ(recent[1].content, "three");

  std::filesystem::remove(path);
}

TEST(SqliteMemoryStoreTest, ClearRemovesOnlyOneSession) {
  auto path = UniqueDbPath("memory-clear");
  SqliteMemoryStore store(path.string());

  store.Append("user-a", MakeEntry(core::MemoryEntryType::kUserMessage, "user", "a1"));
  store.Append("user-b", MakeEntry(core::MemoryEntryType::kUserMessage, "user", "b1"));

  store.Clear("user-a");

  EXPECT_TRUE(store.GetAll("user-a").empty());
  ASSERT_EQ(store.GetAll("user-b").size(), 1u);
  EXPECT_EQ(store.GetAll("user-b")[0].content, "b1");

  std::filesystem::remove(path);
}

TEST(SqliteMemoryStoreTest, SummarizeReplacesRequestedRange) {
  auto path = UniqueDbPath("memory-summarize");
  SqliteMemoryStore store(path.string());

  store.Append("user-a", MakeEntry(core::MemoryEntryType::kUserMessage, "user", "m1"));
  store.Append("user-a", MakeEntry(core::MemoryEntryType::kAssistantMessage, "assistant", "m2"));
  store.Append("user-a", MakeEntry(core::MemoryEntryType::kUserMessage, "user", "m3"));

  auto summary = MakeEntry(core::MemoryEntryType::kSummary, "system", "summary");
  store.Summarize("user-a", 0, 2, summary);

  auto all = store.GetAll("user-a");
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0].type, core::MemoryEntryType::kSummary);
  EXPECT_EQ(all[0].content, "summary");
  EXPECT_EQ(all[1].content, "m3");

  std::filesystem::remove(path);
}

}  // namespace
}  // namespace shizuru::app
