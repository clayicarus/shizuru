// Unit tests for runtime::ToolRegistry

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <string>

#include "runtime/tool_registry.h"

namespace shizuru::runtime {
namespace {

// ---------------------------------------------------------------------------
// ToolRegistry
// ---------------------------------------------------------------------------

TEST(ToolRegistryTest, RegisterAndFind) {
  ToolRegistry registry;
  registry.Register("echo", [](const nlohmann::json& args) -> ToolResult {
    return {true, args, ""};
  });

  EXPECT_TRUE(registry.Has("echo"));
  EXPECT_FALSE(registry.Has("unknown"));

  const auto* fn = registry.Find("echo");
  ASSERT_NE(fn, nullptr);

  auto result = (*fn)("hello");
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.output, "hello");
}

TEST(ToolRegistryTest, Unregister) {
  ToolRegistry registry;
  registry.Register("tool", [](const nlohmann::json&) -> ToolResult {
    return {true, nlohmann::json(), ""};
  });

  EXPECT_TRUE(registry.Has("tool"));
  registry.Unregister("tool");
  EXPECT_FALSE(registry.Has("tool"));
  EXPECT_EQ(registry.Find("tool"), nullptr);
}

TEST(ToolRegistryTest, FindUnknownReturnsNull) {
  ToolRegistry registry;
  EXPECT_EQ(registry.Find("nonexistent"), nullptr);
}

}  // namespace
}  // namespace shizuru::runtime
