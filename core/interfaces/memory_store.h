#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "context/types.h"

namespace shizuru::core {

// Abstract interface for memory persistence backends.
class MemoryStore {
 public:
  virtual ~MemoryStore() = default;

  // Append a memory entry for the given session.
  virtual void Append(const std::string& session_id,
                      const MemoryEntry& entry) = 0;

  // Retrieve the N most recent entries for a session.
  virtual std::vector<MemoryEntry> GetRecent(const std::string& session_id,
                                              size_t count) = 0;

  // Count entries for a session without loading them.
  virtual size_t Count(const std::string& session_id) = 0;

  // Retrieve all entries for a session.
  // Test-only escape hatch: production / business code must not depend on
  // unbounded history reads. Use GetRecent() or Count() instead.
  virtual std::vector<MemoryEntry> GetAll(const std::string& session_id) = 0;

  // Replace a range of entries with a summary entry (for summarization).
  virtual void Summarize(const std::string& session_id,
                         size_t start_index, size_t end_index,
                         const MemoryEntry& summary) = 0;

  // Remove all entries for a session.
  virtual void Clear(const std::string& session_id) = 0;
};

}  // namespace shizuru::core
