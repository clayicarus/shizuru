#pragma once

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "interfaces/memory_store.h"

namespace shizuru::services {

// Thread-safe in-memory implementation of core::MemoryStore.
// Suitable for single-process use. Data is lost on process exit.
class InMemoryStore : public core::MemoryStore {
 public:
  void Append(const std::string& session_id,
              const core::MemoryEntry& entry) override {
    std::lock_guard<std::mutex> lock(mu_);
    entries_[session_id].push_back(entry);
  }

  std::vector<core::MemoryEntry> GetRecent(const std::string& session_id,
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
    return std::vector<core::MemoryEntry>(
        all.end() - static_cast<ptrdiff_t>(count), all.end());
  }

  std::vector<core::MemoryEntry> GetAll(
      const std::string& session_id) override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(session_id);
    if (it == entries_.end()) {
      return {};
    }
    return it->second;
  }

  void Summarize(const std::string& session_id, size_t start_index,
                 size_t end_index,
                 const core::MemoryEntry& summary) override {
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
  std::unordered_map<std::string, std::vector<core::MemoryEntry>> entries_;
};

}  // namespace shizuru::services
