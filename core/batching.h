#pragma once

// core/batching.h — Core batching logic.
//
// Maintains a pending queue of ConversationItems and constructs InvokeBatch
// objects based on turn policy decisions.  Core is the sole cross-message
// batching decision maker (requirement 7.1, 7.2).

#include <chrono>
#include <functional>
#include <mutex>
#include <vector>

#include "core/conversation_item.h"
#include "core/invoke_batch.h"

namespace shizuru::core {

// Callback invoked when a batch is ready to be sent to the LLM.
using BatchReadyCallback = std::function<void(InvokeBatch)>;

// Configuration for batching behavior.
struct BatchingConfig {
  // Maximum time to wait for additional items before flushing.
  std::chrono::milliseconds debounce_timeout{500};

  // Maximum number of items to accumulate before auto-flushing.
  size_t max_pending_items = 10;
};

// Batching logic for Core.
// Thread-safe: all methods can be called from any thread.
class Batcher {
 public:
  explicit Batcher(BatchingConfig config = {});

  // Set the callback invoked when a batch is ready.
  void SetBatchReadyCallback(BatchReadyCallback cb);

  // Add a ConversationItem to the pending queue.
  // May trigger an immediate flush depending on the item kind and policy.
  void Enqueue(ConversationItem item);

  // Force-flush all pending items into a batch with the given reason.
  // Returns the batch, or nullopt if nothing is pending.
  InvokeBatch Flush(TriggerReason reason);

  // Check if there are pending items.
  bool HasPending() const;

  // Get the number of pending items.
  size_t PendingCount() const;

  // Clear all pending items without flushing.
  void Clear();

 private:
  InvokeBatch FlushLocked(TriggerReason reason);

  BatchingConfig config_;

  mutable std::mutex mutex_;
  std::vector<ConversationItem> pending_;
  std::string conversation_id_;  // From the first item in the batch.

  std::mutex cb_mutex_;
  BatchReadyCallback on_batch_ready_;
};

}  // namespace shizuru::core
