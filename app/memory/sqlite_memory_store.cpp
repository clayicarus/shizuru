#include "app/memory/sqlite_memory_store.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "conversation/item.h"
#include "conversation/render.h"

namespace shizuru::app {

namespace {

constexpr size_t kMaxRecentFetch = 512;
constexpr int kBusyTimeoutMs = 5000;

void CheckSqlite(int rc, sqlite3* db, const char* what);
int64_t ToStoredMs(const core::MemoryEntry& entry);

void InsertEntry(sqlite3* db,
                 const std::string& session_key,
                 const core::MemoryEntry& entry,
                 std::optional<int64_t> created_at_ms = std::nullopt) {
  // Serialize item to JSON for message-like entries.
  std::string item_json;
  std::string role;
  std::string content;
  std::string source_tag;
  std::string tool_call_id;
  std::string tool_calls_json;

  if (entry.type != core::MemoryEntryType::kSummary &&
      entry.type != core::MemoryEntryType::kExternalContext) {
    item_json = core::conversation::SerializeConversationItem(entry.item);
    // Also populate flat fields for backward compatibility with old readers.
    auto rendered = core::conversation::RenderForLlm(entry.item);
    role = rendered.role;
    content = rendered.content;
    source_tag = rendered.name;
    tool_call_id = rendered.tool_call_id;
    tool_calls_json = rendered.tool_calls_json;
  } else {
    role = entry.role;
    content = entry.content;
    source_tag = entry.source_tag;
  }

  const char* sql = R"SQL(
    INSERT INTO memory (
      session_key, type, role, content, source_tag, tool_call_id,
      tool_calls_json, item_json, created_at_ms, estimated_tokens
    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
  )SQL";
  sqlite3_stmt* stmt = nullptr;
  CheckSqlite(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr),
              db, "prepare append");
  auto finalize = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
      stmt, sqlite3_finalize);

  CheckSqlite(sqlite3_bind_text(stmt, 1, session_key.c_str(), -1, SQLITE_TRANSIENT),
              db, "bind session_key");
  CheckSqlite(sqlite3_bind_int(stmt, 2, static_cast<int>(entry.type)),
              db, "bind type");
  CheckSqlite(sqlite3_bind_text(stmt, 3, role.c_str(), -1, SQLITE_TRANSIENT),
              db, "bind role");
  CheckSqlite(sqlite3_bind_text(stmt, 4, content.c_str(), -1, SQLITE_TRANSIENT),
              db, "bind content");
  CheckSqlite(sqlite3_bind_text(stmt, 5, source_tag.c_str(), -1, SQLITE_TRANSIENT),
              db, "bind source_tag");
  CheckSqlite(sqlite3_bind_text(stmt, 6, tool_call_id.c_str(), -1, SQLITE_TRANSIENT),
              db, "bind tool_call_id");
  CheckSqlite(sqlite3_bind_text(stmt, 7, tool_calls_json.c_str(), -1, SQLITE_TRANSIENT),
              db, "bind tool_calls_json");
  CheckSqlite(sqlite3_bind_text(stmt, 8, item_json.c_str(), -1, SQLITE_TRANSIENT),
              db, "bind item_json");
  CheckSqlite(sqlite3_bind_int64(stmt, 9, created_at_ms.value_or(ToStoredMs(entry))),
              db, "bind created_at_ms");
  CheckSqlite(sqlite3_bind_int(stmt, 10, entry.estimated_tokens),
              db, "bind estimated_tokens");

  CheckSqlite(sqlite3_step(stmt), db, "step append");
}

int64_t NowUnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t ToStoredMs(const core::MemoryEntry& /*entry*/) {
  // MemoryEntry timestamps use steady_clock, which is not stable across
  // process restarts. Persist wall-clock time at append time instead.
  return NowUnixMs();
}

core::MemoryEntryType DecodeType(int value) {
  switch (value) {
    case 0: return core::MemoryEntryType::kUserMessage;
    case 1: return core::MemoryEntryType::kAssistantMessage;
    case 2: return core::MemoryEntryType::kToolCall;
    case 3: return core::MemoryEntryType::kToolResult;
    case 4: return core::MemoryEntryType::kSummary;
    case 5: return core::MemoryEntryType::kExternalContext;
    default: return core::MemoryEntryType::kUserMessage;
  }
}

std::chrono::steady_clock::time_point FromStoredMs(int64_t ms) {
  const auto now_sys = std::chrono::system_clock::now();
  const auto now_steady = std::chrono::steady_clock::now();
  const auto stored_sys = std::chrono::system_clock::time_point(
      std::chrono::milliseconds(ms));
  const auto delta = stored_sys - now_sys;
  return now_steady + std::chrono::duration_cast<std::chrono::steady_clock::duration>(delta);
}

void CheckSqlite(int rc, sqlite3* db, const char* what) {
  if (rc == SQLITE_OK || rc == SQLITE_ROW || rc == SQLITE_DONE) {
    return;
  }
  const char* err = db != nullptr ? sqlite3_errmsg(db) : "unknown sqlite error";
  throw std::runtime_error(std::string(what) + ": " + err);
}

const char* ColumnText(sqlite3_stmt* stmt, int col) {
  const auto* text = sqlite3_column_text(stmt, col);
  return text != nullptr ? reinterpret_cast<const char*>(text) : "";
}

core::MemoryEntry ReadEntry(sqlite3_stmt* stmt) {
  core::MemoryEntry entry;
  entry.type = DecodeType(sqlite3_column_int(stmt, 1));
  entry.role = ColumnText(stmt, 2);
  entry.content = ColumnText(stmt, 3);
  entry.source_tag = ColumnText(stmt, 4);
  // Columns 5 (tool_call_id) and 6 (tool_calls_json) are kept in DB for
  // backward compatibility but no longer stored in MemoryEntry.
  std::string item_json = ColumnText(stmt, 7);
  entry.timestamp = FromStoredMs(sqlite3_column_int64(stmt, 8));
  entry.estimated_tokens = sqlite3_column_int(stmt, 9);

  // Restore ConversationItem from item_json for message-like entries.
  if (!item_json.empty()) {
    auto parsed = core::conversation::TryParseConversationItem(item_json);
    if (parsed.has_value()) {
      entry.item = *parsed;
    }
  }

  return entry;
}

}  // namespace

struct SqliteMemoryStore::Impl {
  sqlite3* db = nullptr;
  std::mutex mu;
};

SqliteMemoryStore::SqliteMemoryStore(const std::string& db_path)
    : impl_(std::make_unique<Impl>()) {
  const std::string effective_path =
      (db_path.empty() ? ":memory:" : db_path);

  const int open_flags = SQLITE_OPEN_READWRITE |
                         SQLITE_OPEN_CREATE |
                         SQLITE_OPEN_FULLMUTEX;
  CheckSqlite(sqlite3_open_v2(effective_path.c_str(), &impl_->db,
                              open_flags, nullptr),
              impl_->db, "sqlite3_open");
  CheckSqlite(sqlite3_busy_timeout(impl_->db, kBusyTimeoutMs),
              impl_->db, "set busy timeout");

  CheckSqlite(sqlite3_exec(impl_->db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr),
              impl_->db, "enable WAL");
  CheckSqlite(sqlite3_exec(impl_->db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr),
              impl_->db, "set synchronous");
  CheckSqlite(sqlite3_exec(impl_->db, "PRAGMA user_version=1;", nullptr, nullptr, nullptr),
              impl_->db, "set user_version");

  const char* schema = R"SQL(
    CREATE TABLE IF NOT EXISTS memory (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      session_key TEXT NOT NULL,
      type INTEGER NOT NULL,
      role TEXT NOT NULL,
      content TEXT NOT NULL,
      source_tag TEXT NOT NULL DEFAULT '',
      tool_call_id TEXT NOT NULL DEFAULT '',
      tool_calls_json TEXT NOT NULL DEFAULT '',
      item_json TEXT NOT NULL DEFAULT '',
      created_at_ms INTEGER NOT NULL,
      estimated_tokens INTEGER NOT NULL DEFAULT 0
    );
    CREATE INDEX IF NOT EXISTS idx_memory_session_id_id
      ON memory(session_key, id);
    CREATE INDEX IF NOT EXISTS idx_memory_session_id_created_at
      ON memory(session_key, created_at_ms);
  )SQL";
  CheckSqlite(sqlite3_exec(impl_->db, schema, nullptr, nullptr, nullptr),
              impl_->db, "create schema");
}

SqliteMemoryStore::~SqliteMemoryStore() {
  if (impl_) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->db != nullptr) {
      sqlite3_close_v2(impl_->db);
      impl_->db = nullptr;
    }
  }
}

void SqliteMemoryStore::Append(const std::string& session_key,
                               const core::MemoryEntry& entry) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  InsertEntry(impl_->db, session_key, entry);
}

std::vector<core::MemoryEntry> SqliteMemoryStore::GetRecent(
    const std::string& session_key, size_t count) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  const int limit = static_cast<int>(std::min(count, kMaxRecentFetch));
  if (limit <= 0) {
    return {};
  }

  const char* sql = R"SQL(
    SELECT id, type, role, content, source_tag, tool_call_id,
           tool_calls_json, item_json, created_at_ms, estimated_tokens
      FROM memory
     WHERE session_key = ?
     ORDER BY id DESC
     LIMIT ?
  )SQL";
  sqlite3_stmt* stmt = nullptr;
  CheckSqlite(sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr),
              impl_->db, "prepare get recent");
  auto finalize = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
      stmt, sqlite3_finalize);

  CheckSqlite(sqlite3_bind_text(stmt, 1, session_key.c_str(), -1, SQLITE_TRANSIENT),
              impl_->db, "bind recent session_key");
  CheckSqlite(sqlite3_bind_int(stmt, 2, limit),
              impl_->db, "bind recent limit");

  std::vector<core::MemoryEntry> result;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    result.push_back(ReadEntry(stmt));
  }
  std::reverse(result.begin(), result.end());
  return result;
}

size_t SqliteMemoryStore::Count(const std::string& session_key) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  const char* sql = R"SQL(
    SELECT COUNT(*)
      FROM memory
     WHERE session_key = ?
  )SQL";
  sqlite3_stmt* stmt = nullptr;
  CheckSqlite(sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr),
              impl_->db, "prepare count");
  auto finalize = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
      stmt, sqlite3_finalize);

  CheckSqlite(sqlite3_bind_text(stmt, 1, session_key.c_str(), -1, SQLITE_TRANSIENT),
              impl_->db, "bind count session_key");
  CheckSqlite(sqlite3_step(stmt), impl_->db, "step count");
  return static_cast<size_t>(sqlite3_column_int64(stmt, 0));
}

std::vector<core::MemoryEntry> SqliteMemoryStore::GetAll(
    const std::string& session_key) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  const char* sql = R"SQL(
    SELECT id, type, role, content, source_tag, tool_call_id,
           tool_calls_json, item_json, created_at_ms, estimated_tokens
      FROM memory
     WHERE session_key = ?
     ORDER BY id ASC
  )SQL";
  sqlite3_stmt* stmt = nullptr;
  CheckSqlite(sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr),
              impl_->db, "prepare get all");
  auto finalize = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
      stmt, sqlite3_finalize);

  CheckSqlite(sqlite3_bind_text(stmt, 1, session_key.c_str(), -1, SQLITE_TRANSIENT),
              impl_->db, "bind get all session_key");

  std::vector<core::MemoryEntry> result;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    result.push_back(ReadEntry(stmt));
  }
  return result;
}

void SqliteMemoryStore::Summarize(const std::string& session_key,
                                  size_t start_index, size_t end_index,
                                  const core::MemoryEntry& summary) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (start_index >= end_index) {
    return;
  }

  const char* select_sql = R"SQL(
    SELECT id
      FROM memory
     WHERE session_key = ?
     ORDER BY id ASC
     LIMIT ? OFFSET ?
  )SQL";
  sqlite3_stmt* select_stmt = nullptr;
  CheckSqlite(sqlite3_prepare_v2(impl_->db, select_sql, -1, &select_stmt, nullptr),
              impl_->db, "prepare summarize select ids");
  auto finalize_select = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
      select_stmt, sqlite3_finalize);

  const int span = static_cast<int>(end_index - start_index);
  CheckSqlite(sqlite3_bind_text(select_stmt, 1, session_key.c_str(), -1, SQLITE_TRANSIENT),
              impl_->db, "bind summarize session_key");
  CheckSqlite(sqlite3_bind_int(select_stmt, 2, span),
              impl_->db, "bind summarize span");
  CheckSqlite(sqlite3_bind_int(select_stmt, 3, static_cast<int>(start_index)),
              impl_->db, "bind summarize offset");

  std::vector<int64_t> ids;
  while (sqlite3_step(select_stmt) == SQLITE_ROW) {
    ids.push_back(sqlite3_column_int64(select_stmt, 0));
  }
  if (ids.empty()) {
    return;
  }

  CheckSqlite(sqlite3_exec(impl_->db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, nullptr),
              impl_->db, "begin summarize");
  try {
    const char* update_sql = R"SQL(
      UPDATE memory
         SET type = ?,
             role = ?,
             content = ?,
             source_tag = ?,
             tool_call_id = ?,
             tool_calls_json = ?,
             item_json = ?,
             estimated_tokens = ?
       WHERE id = ?
    )SQL";
    sqlite3_stmt* update_stmt = nullptr;
    CheckSqlite(sqlite3_prepare_v2(impl_->db, update_sql, -1, &update_stmt, nullptr),
                impl_->db, "prepare summarize update");
    auto finalize_update = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
        update_stmt, sqlite3_finalize);

    CheckSqlite(sqlite3_bind_int(update_stmt, 1, static_cast<int>(summary.type)),
                impl_->db, "bind summarize type");
    CheckSqlite(sqlite3_bind_text(update_stmt, 2, summary.role.c_str(), -1, SQLITE_TRANSIENT),
                impl_->db, "bind summarize role");
    CheckSqlite(sqlite3_bind_text(update_stmt, 3, summary.content.c_str(), -1, SQLITE_TRANSIENT),
                impl_->db, "bind summarize content");
    CheckSqlite(sqlite3_bind_text(update_stmt, 4, summary.source_tag.c_str(), -1, SQLITE_TRANSIENT),
                impl_->db, "bind summarize source_tag");
    CheckSqlite(sqlite3_bind_text(update_stmt, 5, "", -1, SQLITE_TRANSIENT),
                impl_->db, "bind summarize tool_call_id");
    CheckSqlite(sqlite3_bind_text(update_stmt, 6, "", -1, SQLITE_TRANSIENT),
                impl_->db, "bind summarize tool_calls_json");
    CheckSqlite(sqlite3_bind_text(update_stmt, 7, "", -1, SQLITE_TRANSIENT),
                impl_->db, "bind summarize item_json");
    CheckSqlite(sqlite3_bind_int(update_stmt, 8, summary.estimated_tokens),
                impl_->db, "bind summarize estimated_tokens");
    CheckSqlite(sqlite3_bind_int64(update_stmt, 9, ids.front()),
                impl_->db, "bind summarize target id");
    CheckSqlite(sqlite3_step(update_stmt), impl_->db, "step summarize update");

    const char* delete_sql = "DELETE FROM memory WHERE id = ?;";
    sqlite3_stmt* delete_stmt = nullptr;
    CheckSqlite(sqlite3_prepare_v2(impl_->db, delete_sql, -1, &delete_stmt, nullptr),
                impl_->db, "prepare summarize delete");
    auto finalize_delete = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
        delete_stmt, sqlite3_finalize);

    for (size_t i = 1; i < ids.size(); ++i) {
      auto id = ids[i];
      sqlite3_reset(delete_stmt);
      sqlite3_clear_bindings(delete_stmt);
      CheckSqlite(sqlite3_bind_int64(delete_stmt, 1, id),
                  impl_->db, "bind summarize delete id");
      CheckSqlite(sqlite3_step(delete_stmt), impl_->db, "step summarize delete");
    }
    CheckSqlite(sqlite3_exec(impl_->db, "COMMIT;", nullptr, nullptr, nullptr),
                impl_->db, "commit summarize");
  } catch (...) {
    sqlite3_exec(impl_->db, "ROLLBACK;", nullptr, nullptr, nullptr);
    throw;
  }
}

void SqliteMemoryStore::Clear(const std::string& session_key) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  const char* sql = "DELETE FROM memory WHERE session_key = ?;";
  sqlite3_stmt* stmt = nullptr;
  CheckSqlite(sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr),
              impl_->db, "prepare clear");
  auto finalize = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
      stmt, sqlite3_finalize);

  CheckSqlite(sqlite3_bind_text(stmt, 1, session_key.c_str(), -1, SQLITE_TRANSIENT),
              impl_->db, "bind clear session_key");
  CheckSqlite(sqlite3_step(stmt), impl_->db, "step clear");
}

}  // namespace shizuru::app
