#pragma once

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "interfaces/memory_store.h"

namespace shizuru::core::testing {

// Hand-written mock for MemoryStore.
// Uses in-memory std::vector storage per session.
class MockMemoryStore : public MemoryStore {
 public:
  void Append(const std::string& session_id,
              const MemoryEntry& entry) override {
    std::lock_guard<std::mutex> lock(mu_);
    entries_[session_id].push_back(entry);
  }

  std::vector<MemoryEntry> GetRecent(const std::string& session_id,
                                      size_t count) override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(session_id);
    if (it == entries_.end()) {
      return {};
    }
    const auto& all = it->second;
    if (count >= all.size()) {
      return all;
    }
    return std::vector<MemoryEntry>(all.end() - static_cast<ptrdiff_t>(count),
                                    all.end());
  }

  size_t Count(const std::string& session_id) override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(session_id);
    if (it == entries_.end()) {
      return 0;
    }
    return it->second.size();
  }

  // Test-only helper used by assertions over committed history.
  std::vector<MemoryEntry> GetAll(const std::string& session_id) override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(session_id);
    if (it == entries_.end()) {
      return {};
    }
    return it->second;
  }

  void Summarize(const std::string& session_id,
                 size_t start_index, size_t end_index,
                 const MemoryEntry& summary) override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(session_id);
    if (it == entries_.end()) {
      return;
    }
    auto& vec = it->second;
    if (start_index >= vec.size() || end_index > vec.size() ||
        start_index >= end_index) {
      return;
    }
    vec.erase(vec.begin() + static_cast<ptrdiff_t>(start_index),
              vec.begin() + static_cast<ptrdiff_t>(end_index));
    vec.insert(vec.begin() + static_cast<ptrdiff_t>(start_index), summary);
  }

  void Clear(const std::string& session_id) override {
    std::lock_guard<std::mutex> lock(mu_);
    entries_.erase(session_id);
  }

 private:
  std::mutex mu_;
  std::unordered_map<std::string, std::vector<MemoryEntry>> entries_;
};

}  // namespace shizuru::core::testing
