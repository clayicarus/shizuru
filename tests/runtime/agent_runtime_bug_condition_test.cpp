// Bug condition test for AgentRuntime — concurrent Shutdown safety.
//
// Tests that concurrent Shutdown() calls don't crash or corrupt state.
// The original Bug 3 (concurrent SendMessage + Shutdown) is now the
// responsibility of AppRuntime, not the bus.  The bus guarantees that
// concurrent Shutdown() calls are safe (second call is a no-op).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "runtime/agent_runtime.h"
#include "mock_io_device.h"

namespace shizuru::runtime {
namespace {

using testing::MockIoDevice;

// ---------------------------------------------------------------------------
// Concurrent Shutdown calls must not crash or double-Stop devices.
// ---------------------------------------------------------------------------
TEST(AgentRuntimeBugCondition, ConcurrentShutdownDoesNotCrash) {
  AgentRuntime runtime;

  std::atomic<int> stop_count{0};
  for (int i = 0; i < 5; ++i) {
    auto dev = std::make_unique<MockIoDevice>("dev_" + std::to_string(i));
    dev->Start();
    runtime.RegisterDevice(std::move(dev));
  }

  // Launch 10 threads all calling Shutdown() concurrently.
  constexpr int kNumThreads = 10;
  std::vector<std::thread> threads;

  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&] {
      runtime.Shutdown();
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // After all threads complete, FindDevice should return nullptr for all.
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(runtime.FindDevice("dev_" + std::to_string(i)), nullptr);
  }
}

}  // namespace
}  // namespace shizuru::runtime
