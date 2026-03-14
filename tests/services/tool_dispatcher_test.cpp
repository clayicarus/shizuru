// Unit tests for services::ToolDispatcher and ToolRegistry

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "controller/types.h"
#include "interfaces/io_bridge.h"
#include "io/tool_dispatcher.h"
#include "io/tool_registry.h"

namespace shizuru::services {
namespace {

core::ActionCandidate MakeToolCall(const std::string& name,
                                   const std::string& args = "{}") {
  core::ActionCandidate ac;
  ac.type = core::ActionType::kToolCall;
  ac.action_name = name;
  ac.arguments = args;
  return ac;
}

// ---------------------------------------------------------------------------
// ToolRegistry
// ---------------------------------------------------------------------------

TEST(ToolRegistryTest, RegisterAndFind) {
  ToolRegistry registry;
  registry.Register("echo", [](const std::string& args) -> core::ActionResult {
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
  registry.Register("tool", [](const std::string&) -> core::ActionResult {
    return {true, "", ""};
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

// ---------------------------------------------------------------------------
// ToolDispatcher
// ---------------------------------------------------------------------------

TEST(ToolDispatcherTest, ExecuteKnownTool) {
  ToolRegistry registry;
  registry.Register("add", [](const std::string& args) -> core::ActionResult {
    return {true, "result: 42", ""};
  });

  ToolDispatcher dispatcher(registry);
  auto result = dispatcher.Execute(MakeToolCall("add", "{\"a\":1,\"b\":2}"));

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.output, "result: 42");
  EXPECT_TRUE(result.error_message.empty());
}

TEST(ToolDispatcherTest, ExecuteUnknownTool) {
  ToolRegistry registry;
  ToolDispatcher dispatcher(registry);

  auto result = dispatcher.Execute(MakeToolCall("nonexistent"));

  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.output.empty());
  EXPECT_NE(result.error_message.find("Unknown tool"), std::string::npos);
}

TEST(ToolDispatcherTest, ExecuteToolThatThrows) {
  ToolRegistry registry;
  registry.Register("bad_tool",
                     [](const std::string&) -> core::ActionResult {
                       throw std::runtime_error("internal error");
                     });

  ToolDispatcher dispatcher(registry);
  auto result = dispatcher.Execute(MakeToolCall("bad_tool"));

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("internal error"), std::string::npos);
}

TEST(ToolDispatcherTest, CancelDoesNotCrash) {
  ToolRegistry registry;
  ToolDispatcher dispatcher(registry);
  EXPECT_NO_THROW(dispatcher.Cancel());
}

}  // namespace
}  // namespace shizuru::services
