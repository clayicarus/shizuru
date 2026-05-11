#pragma once

#include <string>
#include <variant>

namespace shizuru::core {

struct FlushSignal {};

struct CancelSignal {};

struct InterruptSignal {};

struct ThinkStartSignal {};

struct ToolCallStartSignal {
  std::string tool_call_id;
  std::string name;
  std::string arguments;
};

struct FinalAnswerStartSignal {};

struct TurnCompleteSignal {};

struct ToolResultSignal {
  std::string tool_call_id;
  std::string content;
  bool success = true;
};

using ControlSignal = std::variant<FlushSignal,
                                   CancelSignal,
                                   InterruptSignal,
                                   ThinkStartSignal,
                                   ToolCallStartSignal,
                                   FinalAnswerStartSignal,
                                   TurnCompleteSignal,
                                   ToolResultSignal>;

}  // namespace shizuru::core
