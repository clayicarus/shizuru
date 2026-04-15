#pragma once

// app/memory/memory_types.h — Extended memory types for the product layer.
//
// The core MemoryEntryType covers conversation-level entries (user message,
// assistant message, tool call/result, summary).  The product layer needs
// additional structured memory types for followups, preferences, and notes.
//
// These are stored alongside conversation entries in the same MemoryStore,
// distinguished by type.  The persona/prompt layer reads them to inject
// relevant context into the system prompt.

#include <chrono>
#include <string>

namespace shizuru::app {

// A followup item — something Shizuru should check back on later.
struct FollowUp {
  std::string id;           // Unique identifier.
  std::string user_id;
  std::string content;      // What to follow up on ("面试结果", "和房东沟通").
  std::string source_turn;  // Which conversation turn created this.

  enum class Status { kActive, kCompleted, kCancelled };
  Status status = Status::kActive;

  // When to check in.  Zero = no specific time, just "later".
  std::chrono::system_clock::time_point check_in_time;

  std::chrono::system_clock::time_point created_at;
  std::chrono::system_clock::time_point updated_at;
};

// A user preference — learned from conversation, not explicitly set.
struct UserPreference {
  std::string key;      // e.g., "communication_style", "sleep_goal", "work_schedule"
  std::string value;    // e.g., "prefers direct feedback", "wants to sleep before 11pm"
  std::string source;   // Which conversation inferred this.
  std::chrono::system_clock::time_point learned_at;
};

// A quick note — user explicitly asked to save something.
struct Note {
  std::string id;
  std::string user_id;
  std::string content;
  std::chrono::system_clock::time_point created_at;
};

// A reminder — time-triggered notification.
struct Reminder {
  std::string id;
  std::string user_id;
  std::string message;
  std::chrono::system_clock::time_point trigger_time;

  enum class Status { kPending, kFired, kCancelled };
  Status status = Status::kPending;

  std::chrono::system_clock::time_point created_at;
};

}  // namespace shizuru::app
