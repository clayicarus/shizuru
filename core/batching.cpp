// core/batching.cpp — Core batching logic implementation.

#include "core/batching.h"

#include <utility>

namespace shizuru::core {

Batcher::Batcher(BatchingConfig config) : config_(std::move(config)) {}

void Batcher::SetBatchReadyCallback(BatchReadyCallback cb) {
  std::lock_guard<std::mutex> lock(cb_mutex_);
  on_batch_ready_ = std::move(cb);
}

void Batcher::Enqueue(ConversationItem item) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Capture conversation_id from the first item.
  if (pending_.empty() && !item.conversation_id.empty()) {
    conversation_id_ = item.conversation_id;
  }

  pending_.push_back(std::move(item));

  // Auto-flush if we hit the max pending items limit.
  if (pending_.size() >= config_.max_pending_items) {
    auto batch = FlushLocked(TriggerReason::kUserFlush);
    // Deliver outside the lock would be better, but for simplicity:
    BatchReadyCallback cb;
    {
      std::lock_guard<std::mutex> cb_lock(cb_mutex_);
      cb = on_batch_ready_;
    }
    if (cb) { cb(std::move(batch)); }
  }
}

InvokeBatch Batcher::Flush(TriggerReason reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  return FlushLocked(reason);
}

bool Batcher::HasPending() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !pending_.empty();
}

size_t Batcher::PendingCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pending_.size();
}

void Batcher::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  pending_.clear();
  conversation_id_.clear();
}

InvokeBatch Batcher::FlushLocked(TriggerReason reason) {
  InvokeBatch batch;
  batch.conversation_id = conversation_id_;
  batch.items = std::move(pending_);
  batch.reason = reason;
  batch.created_at = std::chrono::steady_clock::now();

  pending_.clear();
  conversation_id_.clear();

  return batch;
}

}  // namespace shizuru::core
