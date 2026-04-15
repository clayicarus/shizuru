#pragma once

// app/memory/sqlite_memory_store.h — SQLite-backed MemoryStore.
//
// Implements core::MemoryStore with persistent storage.
// Data survives process restarts and can be synced across devices.
//
// Schema (single table, simple):
//   CREATE TABLE memory (
//     id INTEGER PRIMARY KEY AUTOINCREMENT,
//     user_id TEXT NOT NULL,
//     type INTEGER NOT NULL,        -- MemoryEntryType ordinal
//     role TEXT NOT NULL,
//     content TEXT NOT NULL,
//     source_tag TEXT DEFAULT '',
//     tool_call_id TEXT DEFAULT '',
//     tool_calls_json TEXT DEFAULT '',
//     timestamp_ms INTEGER NOT NULL, -- milliseconds since epoch
//     estimated_tokens INTEGER DEFAULT 0
//   );
//
// The session_id parameter in MemoryStore methods maps to user_id here,
// since the product layer uses user_id as the memory key.

#include <string>

#include "core/interfaces/memory_store.h"

namespace shizuru::app {

class SqliteMemoryStore : public core::MemoryStore {
 public:
  // Opens or creates the database at db_path.
  // If db_path is empty or ":memory:", uses an in-memory database.
  explicit SqliteMemoryStore(const std::string& db_path);
  ~SqliteMemoryStore() override;

  void Append(const std::string& user_id, const core::MemoryEntry& entry) override;
  std::vector<core::MemoryEntry> GetRecent(const std::string& user_id, size_t count) override;
  std::vector<core::MemoryEntry> GetAll(const std::string& user_id) override;
  void Summarize(const std::string& user_id, size_t start_index, size_t end_index,
                 const core::MemoryEntry& summary) override;
  void Clear(const std::string& user_id) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace shizuru::app
