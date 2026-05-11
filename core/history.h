#pragma once

// core/history.h — History storage interface.
//
// Defines the abstract interface for storing and retrieving ConversationItems.
// History stores the semantic objects that make up the conversation — both
// user messages, assistant responses, tool calls, and tool results.
//
// Requirements 10.1, 10.3: History stores ConversationItem objects.

#include <string>
#include <vector>

#include "core/conversation_item.h"

namespace shizuru::core {

// Abstract interface for conversation history storage.
class HistoryStore {
 public:
  virtual ~HistoryStore() = default;

  // Append a ConversationItem to the history for a given session.
  virtual void Append(const std::string& session_id, ConversationItem item) = 0;

  // Get the most recent items that fit within a token budget.
  // Returns items in chronological order (oldest first).
  // max_tokens is an approximate budget — implementations may use heuristics.
  virtual std::vector<ConversationItem> GetWindow(
      const std::string& session_id, int max_tokens) = 0;

  // Get the N most recent items for a session.
  virtual std::vector<ConversationItem> GetRecent(
      const std::string& session_id, size_t max_count) = 0;

  // Clear all history for a session.
  virtual void Clear(const std::string& session_id) = 0;
};

}  // namespace shizuru::core
