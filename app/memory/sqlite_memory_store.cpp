// app/memory/sqlite_memory_store.cpp — SQLite-backed MemoryStore.
// Stub implementation — to be filled in.

#include "app/memory/sqlite_memory_store.h"

namespace shizuru::app {

struct SqliteMemoryStore::Impl {
  // TODO: sqlite3* db handle, prepared statements.
};

SqliteMemoryStore::SqliteMemoryStore(const std::string& /*db_path*/) {
  // TODO: Open database, create table if not exists.
}

SqliteMemoryStore::~SqliteMemoryStore() = default;

void SqliteMemoryStore::Append(const std::string& /*user_id*/,
                               const core::MemoryEntry& /*entry*/) {
  // TODO
}

std::vector<core::MemoryEntry> SqliteMemoryStore::GetRecent(
    const std::string& /*user_id*/, size_t /*count*/) {
  return {};  // TODO
}

std::vector<core::MemoryEntry> SqliteMemoryStore::GetAll(
    const std::string& /*user_id*/) {
  return {};  // TODO
}

void SqliteMemoryStore::Summarize(const std::string& /*user_id*/,
                                  size_t /*start_index*/, size_t /*end_index*/,
                                  const core::MemoryEntry& /*summary*/) {
  // TODO
}

void SqliteMemoryStore::Clear(const std::string& /*user_id*/) {
  // TODO
}

}  // namespace shizuru::app
