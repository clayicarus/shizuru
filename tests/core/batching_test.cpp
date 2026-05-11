// tests/core/batching_test.cpp — Batching tests.
//
// Task 46: Verifies that group chat A/B/A message ordering is preserved
// through the batching layer (requirement 7.3).

#include <gtest/gtest.h>

#include "core/batching.h"
#include "core/conversation_item.h"

namespace shizuru::core {
namespace {

TEST(Batching, GroupChatABAOrderPreserved) {
  // Simulate group chat: A sends, B sends, A sends again.
  // All three should appear in the batch in order.
  Batcher batcher;

  ConversationItem msg_a1;
  msg_a1.item_id = "1";
  msg_a1.conversation_id = "group:123";
  msg_a1.kind = ConversationItemKind::kUserMessage;
  msg_a1.actor = ActorRef{"user_a", "Alice", ActorKind::kHuman};
  msg_a1.parts.emplace_back(TextPart{"Hello from A"});

  ConversationItem msg_b;
  msg_b.item_id = "2";
  msg_b.conversation_id = "group:123";
  msg_b.kind = ConversationItemKind::kUserMessage;
  msg_b.actor = ActorRef{"user_b", "Bob", ActorKind::kHuman};
  msg_b.parts.emplace_back(TextPart{"Hello from B"});

  ConversationItem msg_a2;
  msg_a2.item_id = "3";
  msg_a2.conversation_id = "group:123";
  msg_a2.kind = ConversationItemKind::kUserMessage;
  msg_a2.actor = ActorRef{"user_a", "Alice", ActorKind::kHuman};
  msg_a2.parts.emplace_back(TextPart{"A again"});

  batcher.Enqueue(std::move(msg_a1));
  batcher.Enqueue(std::move(msg_b));
  batcher.Enqueue(std::move(msg_a2));

  EXPECT_EQ(batcher.PendingCount(), 3);

  InvokeBatch batch = batcher.Flush(TriggerReason::kUserFlush);

  ASSERT_EQ(batch.items.size(), 3);
  EXPECT_EQ(batch.items[0].actor.actor_id, "user_a");
  EXPECT_EQ(batch.items[1].actor.actor_id, "user_b");
  EXPECT_EQ(batch.items[2].actor.actor_id, "user_a");

  // Verify ordering is A, B, A — not aggregated or reordered.
  EXPECT_EQ(batch.items[0].item_id, "1");
  EXPECT_EQ(batch.items[1].item_id, "2");
  EXPECT_EQ(batch.items[2].item_id, "3");
}

TEST(Batching, ConversationIdFromFirstItem) {
  Batcher batcher;

  ConversationItem msg;
  msg.item_id = "1";
  msg.conversation_id = "group:456";
  msg.kind = ConversationItemKind::kUserMessage;
  msg.actor = ActorRef{"user_a", "Alice", ActorKind::kHuman};
  msg.parts.emplace_back(TextPart{"hi"});

  batcher.Enqueue(std::move(msg));
  InvokeBatch batch = batcher.Flush(TriggerReason::kDebounceTimeout);

  EXPECT_EQ(batch.conversation_id, "group:456");
  EXPECT_EQ(batch.reason, TriggerReason::kDebounceTimeout);
}

TEST(Batching, FlushEmptyReturnsEmptyBatch) {
  Batcher batcher;
  InvokeBatch batch = batcher.Flush(TriggerReason::kUserFlush);
  EXPECT_TRUE(batch.items.empty());
  EXPECT_TRUE(batch.conversation_id.empty());
}

TEST(Batching, ClearRemovesPendingItems) {
  Batcher batcher;

  ConversationItem msg;
  msg.item_id = "1";
  msg.conversation_id = "conv1";
  msg.kind = ConversationItemKind::kUserMessage;
  msg.parts.emplace_back(TextPart{"hi"});

  batcher.Enqueue(std::move(msg));
  EXPECT_TRUE(batcher.HasPending());

  batcher.Clear();
  EXPECT_FALSE(batcher.HasPending());
  EXPECT_EQ(batcher.PendingCount(), 0);
}

}  // namespace
}  // namespace shizuru::core
