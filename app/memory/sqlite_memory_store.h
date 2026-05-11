#pragma once

// app/memory/sqlite_memory_store.h — SQLite-backed HistoryStore.
//
// Implements core::HistoryStore with persistent storage.
// ConversationItems are serialized to JSON for storage.
//
// Schema:
//   CREATE TABLE history (
//     id INTEGER PRIMARY KEY AUTOINCREMENT,
//     session_id TEXT NOT NULL,
//     item_id TEXT NOT NULL,
//     kind INTEGER NOT NULL,
//     item_json TEXT NOT NULL,
//     created_at_ms INTEGER NOT NULL
//   );

#include <memory>
#include <string>

#include "core/history.h"

namespace shizuru::app {

class SqliteMemoryStore : public core::HistoryStore {
 public:
  // Opens or creates the database at db_path.
  // If db_path is empty or ":memory:", uses an in-memory database.
  explicit SqliteMemoryStore(const std::string& db_path);
  ~SqliteMemoryStore() override;

  void Append(const std::string& session_id, core::ConversationItem item) override;
  std::vector<core::ConversationItem> GetWindow(
      const std::string& session_id, int max_tokens) override;
  std::vector<core::ConversationItem> GetRecent(
      const std::string& session_id, size_t max_count) override;
  void Clear(const std::string& session_id) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace shizuru::app
