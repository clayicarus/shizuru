#pragma once

// services/memory/in_memory_history.h — In-memory HistoryStore implementation.
//
// Simple vector-backed history store for development and testing.
// Not persistent — data is lost when the process exits.

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/history.h"

namespace shizuru::services {

class InMemoryHistory : public core::HistoryStore {
 public:
  InMemoryHistory() = default;

  void Append(const std::string& session_id, core::ConversationItem item) override {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[session_id].push_back(std::move(item));
  }

  std::vector<core::ConversationItem> GetWindow(
      const std::string& session_id, int max_tokens) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) { return {}; }

    const auto& items = it->second;

    // Simple heuristic: estimate ~4 chars per token, walk backward.
    int budget = max_tokens;
    size_t start_idx = items.size();
    for (size_t i = items.size(); i > 0; --i) {
      int item_tokens = EstimateTokens(items[i - 1]);
      if (budget - item_tokens < 0 && start_idx < items.size()) { break; }
      budget -= item_tokens;
      start_idx = i - 1;
    }

    return std::vector<core::ConversationItem>(
        items.begin() + static_cast<ptrdiff_t>(start_idx), items.end());
  }

  std::vector<core::ConversationItem> GetRecent(
      const std::string& session_id, size_t max_count) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) { return {}; }

    const auto& items = it->second;
    size_t start = items.size() > max_count ? items.size() - max_count : 0;
    return std::vector<core::ConversationItem>(
        items.begin() + static_cast<ptrdiff_t>(start), items.end());
  }

  void Clear(const std::string& session_id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(session_id);
  }

 private:
  static int EstimateTokens(const core::ConversationItem& item) {
    int chars = 0;
    for (const auto& part : item.parts) {
      if (auto* tp = std::get_if<core::TextPart>(&part)) {
        chars += static_cast<int>(tp->text.size());
      } else if (auto* tcp = std::get_if<core::ToolCallPart>(&part)) {
        chars += static_cast<int>(tcp->arguments_json.size());
      } else if (auto* trp = std::get_if<core::ToolResultPart>(&part)) {
        chars += static_cast<int>(trp->result_json.size());
      }
    }
    return std::max(1, chars / 4);  // ~4 chars per token heuristic.
  }

  std::mutex mutex_;
  std::unordered_map<std::string, std::vector<core::ConversationItem>> sessions_;
};

}  // namespace shizuru::services
