#pragma once

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/history.h"

namespace shizuru::core::testing {

// In-memory mock HistoryStore for tests.
class MockHistoryStore : public HistoryStore {
 public:
  void Append(const std::string& session_id, ConversationItem item) override {
    std::lock_guard<std::mutex> lock(mu_);
    items_[session_id].push_back(std::move(item));
  }

  std::vector<ConversationItem> GetWindow(
      const std::string& session_id, int max_tokens) override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = items_.find(session_id);
    if (it == items_.end()) return {};
    // Simple: return all items (ignore token budget in mock).
    (void)max_tokens;
    return it->second;
  }

  std::vector<ConversationItem> GetRecent(
      const std::string& session_id, size_t max_count) override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = items_.find(session_id);
    if (it == items_.end()) return {};
    const auto& all = it->second;
    if (all.size() <= max_count) return all;
    return std::vector<ConversationItem>(all.end() - max_count, all.end());
  }

  void Clear(const std::string& session_id) override {
    std::lock_guard<std::mutex> lock(mu_);
    items_.erase(session_id);
  }

  // Test helper: get count of items for a session.
  size_t Count(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = items_.find(session_id);
    if (it == items_.end()) return 0;
    return it->second.size();
  }

 private:
  std::mutex mu_;
  std::unordered_map<std::string, std::vector<ConversationItem>> items_;
};

}  // namespace shizuru::core::testing
