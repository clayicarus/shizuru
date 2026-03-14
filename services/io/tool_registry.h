#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include "interfaces/io_bridge.h"

namespace shizuru::services {

// A registered tool's execution function.
// Takes JSON arguments string, returns ActionResult.
using ToolFunction =
    std::function<core::ActionResult(const std::string& arguments)>;

// Registry of available tools that the agent can invoke.
class ToolRegistry {
 public:
  // Register a tool by name.
  void Register(const std::string& name, ToolFunction fn) {
    tools_[name] = std::move(fn);
  }

  // Unregister a tool by name.
  void Unregister(const std::string& name) { tools_.erase(name); }

  // Check if a tool is registered.
  bool Has(const std::string& name) const {
    return tools_.count(name) > 0;
  }

  // Look up a tool function. Returns nullptr if not found.
  const ToolFunction* Find(const std::string& name) const {
    auto it = tools_.find(name);
    if (it == tools_.end()) {
      return nullptr;
    }
    return &it->second;
  }

 private:
  std::unordered_map<std::string, ToolFunction> tools_;
};

}  // namespace shizuru::services
