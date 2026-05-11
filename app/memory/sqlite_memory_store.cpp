// app/memory/sqlite_memory_store.cpp — SQLite-backed HistoryStore.
//
// Persists ConversationItems as JSON in a SQLite database.

#include "app/memory/sqlite_memory_store.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace shizuru::app {

namespace {

constexpr size_t kMaxRecentFetch = 512;
constexpr int kBusyTimeoutMs = 5000;

void CheckSqlite(int rc, sqlite3* db, const char* what) {
  if (rc == SQLITE_OK || rc == SQLITE_ROW || rc == SQLITE_DONE) {
    return;
  }
  const char* err = db != nullptr ? sqlite3_errmsg(db) : "unknown sqlite error";
  throw std::runtime_error(std::string(what) + ": " + err);
}

int64_t NowUnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

const char* ColumnText(sqlite3_stmt* stmt, int col) {
  const auto* text = sqlite3_column_text(stmt, col);
  return text != nullptr ? reinterpret_cast<const char*>(text) : "";
}

// Serialize a ConversationItem to JSON string.
std::string SerializeItem(const core::ConversationItem& item) {
  nlohmann::json j;
  j["item_id"] = item.item_id;
  j["conversation_id"] = item.conversation_id;
  j["kind"] = static_cast<int>(item.kind);
  j["actor"] = {
      {"actor_id", item.actor.actor_id},
      {"display_name", item.actor.display_name},
      {"kind", static_cast<int>(item.actor.kind)},
  };
  j["wall_time_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
      item.wall_time.time_since_epoch()).count();
  j["mentions"] = item.mentions;
  if (item.reply_to_item_id.has_value()) {
    j["reply_to_item_id"] = *item.reply_to_item_id;
  }

  // Serialize parts.
  nlohmann::json parts_json = nlohmann::json::array();
  for (const auto& part : item.parts) {
    nlohmann::json pj;
    if (auto* tp = std::get_if<core::TextPart>(&part)) {
      pj["type"] = "text";
      pj["text"] = tp->text;
    } else if (auto* ip = std::get_if<core::ImagePart>(&part)) {
      pj["type"] = "image";
      pj["url"] = ip->url;
    } else if (auto* ap = std::get_if<core::AudioPart>(&part)) {
      pj["type"] = "audio";
      pj["format"] = ap->format;
      // Don't persist raw audio data.
    } else if (auto* tcp = std::get_if<core::ToolCallPart>(&part)) {
      pj["type"] = "tool_call";
      pj["tool_call_id"] = tcp->tool_call_id;
      pj["name"] = tcp->name;
      pj["arguments_json"] = tcp->arguments_json;
    } else if (auto* trp = std::get_if<core::ToolResultPart>(&part)) {
      pj["type"] = "tool_result";
      pj["tool_call_id"] = trp->tool_call_id;
      pj["tool_name"] = trp->tool_name;
      pj["success"] = trp->success;
      pj["result_json"] = trp->result_json;
    }
    parts_json.push_back(std::move(pj));
  }
  j["parts"] = std::move(parts_json);

  return j.dump();
}

// Deserialize a ConversationItem from JSON string.
core::ConversationItem DeserializeItem(const std::string& json_str) {
  auto j = nlohmann::json::parse(json_str);
  core::ConversationItem item;
  item.item_id = j.value("item_id", "");
  item.conversation_id = j.value("conversation_id", "");
  item.kind = static_cast<core::ConversationItemKind>(j.value("kind", 0));

  if (j.contains("actor")) {
    const auto& aj = j["actor"];
    item.actor.actor_id = aj.value("actor_id", "");
    item.actor.display_name = aj.value("display_name", "");
    item.actor.kind = static_cast<core::ActorKind>(aj.value("kind", 0));
  }

  int64_t wall_ms = j.value("wall_time_ms", int64_t{0});
  item.wall_time = std::chrono::system_clock::time_point(
      std::chrono::milliseconds(wall_ms));

  item.mentions = j.value("mentions", std::vector<std::string>{});
  if (j.contains("reply_to_item_id") && !j["reply_to_item_id"].is_null()) {
    item.reply_to_item_id = j["reply_to_item_id"].get<std::string>();
  }

  if (j.contains("parts") && j["parts"].is_array()) {
    for (const auto& pj : j["parts"]) {
      std::string type = pj.value("type", "");
      if (type == "text") {
        item.parts.emplace_back(core::TextPart{pj.value("text", "")});
      } else if (type == "image") {
        item.parts.emplace_back(core::ImagePart{pj.value("url", "")});
      } else if (type == "audio") {
        item.parts.emplace_back(core::AudioPart{{}, pj.value("format", "")});
      } else if (type == "tool_call") {
        item.parts.emplace_back(core::ToolCallPart{
            pj.value("tool_call_id", ""),
            pj.value("name", ""),
            pj.value("arguments_json", "")});
      } else if (type == "tool_result") {
        item.parts.emplace_back(core::ToolResultPart{
            pj.value("tool_call_id", ""),
            pj.value("tool_name", ""),
            pj.value("success", true),
            pj.value("result_json", "")});
      }
    }
  }

  return item;
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

  CheckSqlite(sqlite3_exec(impl_->db, "PRAGMA journal_mode=WAL;",
                           nullptr, nullptr, nullptr),
              impl_->db, "enable WAL");
  CheckSqlite(sqlite3_exec(impl_->db, "PRAGMA synchronous=NORMAL;",
                           nullptr, nullptr, nullptr),
              impl_->db, "set synchronous");

  const char* schema = R"SQL(
    CREATE TABLE IF NOT EXISTS history (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      session_id TEXT NOT NULL,
      item_id TEXT NOT NULL,
      kind INTEGER NOT NULL,
      item_json TEXT NOT NULL,
      created_at_ms INTEGER NOT NULL
    );
    CREATE INDEX IF NOT EXISTS idx_history_session_id
      ON history(session_id, id);
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

void SqliteMemoryStore::Append(const std::string& session_id,
                               core::ConversationItem item) {
  std::lock_guard<std::mutex> lock(impl_->mu);

  const char* sql = R"SQL(
    INSERT INTO history (session_id, item_id, kind, item_json, created_at_ms)
    VALUES (?, ?, ?, ?, ?)
  )SQL";
  sqlite3_stmt* stmt = nullptr;
  CheckSqlite(sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr),
              impl_->db, "prepare append");
  auto finalize = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
      stmt, sqlite3_finalize);

  std::string json = SerializeItem(item);

  CheckSqlite(sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT),
              impl_->db, "bind session_id");
  CheckSqlite(sqlite3_bind_text(stmt, 2, item.item_id.c_str(), -1, SQLITE_TRANSIENT),
              impl_->db, "bind item_id");
  CheckSqlite(sqlite3_bind_int(stmt, 3, static_cast<int>(item.kind)),
              impl_->db, "bind kind");
  CheckSqlite(sqlite3_bind_text(stmt, 4, json.c_str(), -1, SQLITE_TRANSIENT),
              impl_->db, "bind item_json");
  CheckSqlite(sqlite3_bind_int64(stmt, 5, NowUnixMs()),
              impl_->db, "bind created_at_ms");

  CheckSqlite(sqlite3_step(stmt), impl_->db, "step append");
}

std::vector<core::ConversationItem> SqliteMemoryStore::GetWindow(
    const std::string& session_id, int max_tokens) {
  // Get all recent items and trim by token budget.
  auto items = GetRecent(session_id, kMaxRecentFetch);

  // Walk backward, accumulating estimated tokens.
  int budget = max_tokens;
  size_t start_idx = items.size();
  for (size_t i = items.size(); i > 0; --i) {
    int item_tokens = 0;
    for (const auto& part : items[i - 1].parts) {
      if (auto* tp = std::get_if<core::TextPart>(&part)) {
        item_tokens += static_cast<int>(tp->text.size()) / 4;
      } else if (auto* tcp = std::get_if<core::ToolCallPart>(&part)) {
        item_tokens += static_cast<int>(tcp->arguments_json.size()) / 4;
      } else if (auto* trp = std::get_if<core::ToolResultPart>(&part)) {
        item_tokens += static_cast<int>(trp->result_json.size()) / 4;
      }
    }
    item_tokens = std::max(item_tokens, 1);
    if (budget - item_tokens < 0 && start_idx < items.size()) { break; }
    budget -= item_tokens;
    start_idx = i - 1;
  }

  return std::vector<core::ConversationItem>(
      items.begin() + static_cast<ptrdiff_t>(start_idx), items.end());
}

std::vector<core::ConversationItem> SqliteMemoryStore::GetRecent(
    const std::string& session_id, size_t max_count) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  const int limit = static_cast<int>(std::min(max_count, kMaxRecentFetch));

  const char* sql = R"SQL(
    SELECT item_json FROM history
    WHERE session_id = ?
    ORDER BY id DESC
    LIMIT ?
  )SQL";
  sqlite3_stmt* stmt = nullptr;
  CheckSqlite(sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr),
              impl_->db, "prepare get recent");
  auto finalize = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
      stmt, sqlite3_finalize);

  CheckSqlite(sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT),
              impl_->db, "bind session_id");
  CheckSqlite(sqlite3_bind_int(stmt, 2, limit),
              impl_->db, "bind limit");

  std::vector<core::ConversationItem> result;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    std::string json = ColumnText(stmt, 0);
    try {
      result.push_back(DeserializeItem(json));
    } catch (...) {
      // Skip malformed entries.
    }
  }
  std::reverse(result.begin(), result.end());
  return result;
}

void SqliteMemoryStore::Clear(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  const char* sql = "DELETE FROM history WHERE session_id = ?;";
  sqlite3_stmt* stmt = nullptr;
  CheckSqlite(sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr),
              impl_->db, "prepare clear");
  auto finalize = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
      stmt, sqlite3_finalize);

  CheckSqlite(sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT),
              impl_->db, "bind session_id");
  CheckSqlite(sqlite3_step(stmt), impl_->db, "step clear");
}

}  // namespace shizuru::app
