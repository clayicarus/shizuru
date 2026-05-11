// context_strategy_test.cpp — Stubbed after unified pipeline refactoring.
// TODO: Rewrite tests to use new HistoryStore-based ContextStrategy.

#include <gtest/gtest.h>

#include "context/context_strategy.h"
#include "tests/agent/mocks/mock_memory_store.h"

namespace shizuru::core {
namespace {

TEST(ContextStrategyTest, InitAndReleaseSession) {
  testing::MockHistoryStore store;
  ContextStrategy strategy(ContextConfig{}, store);

  strategy.InitSession("s1", "You are helpful.");
  EXPECT_EQ(strategy.GetSystemInstruction("s1"), "You are helpful.");

  strategy.ReleaseSession("s1");
  // After release, should return default.
  EXPECT_EQ(strategy.GetSystemInstruction("s1"),
            ContextConfig{}.default_system_instruction);
}

TEST(ContextStrategyTest, DefaultSystemInstruction) {
  testing::MockHistoryStore store;
  ContextConfig config;
  config.default_system_instruction = "Default prompt.";
  ContextStrategy strategy(config, store);

  strategy.InitSession("s1");
  EXPECT_EQ(strategy.GetSystemInstruction("s1"), "Default prompt.");
}

}  // namespace
}  // namespace shizuru::core
