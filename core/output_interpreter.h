#pragma once

// core/output_interpreter.h — Core output interpretation.
//
// Interprets LLM raw output (streaming response) and produces:
// - ConversationItem (assistant message, tool call history items)
// - ControlSignal (ThinkStart, ToolCallStart, FinalAnswerStart, TurnComplete)
//
// The LLM raw output is NOT directly a ConversationItem — Core must interpret
// it (requirement 8.1, 8.2, 8.3).

#include <functional>
#include <string>
#include <vector>

#include "core/content_part.h"
#include "core/control_signal.h"
#include "core/conversation_item.h"
#include "core/invoke_batch.h"
#include "controller/types.h"

namespace shizuru::core {

// Callbacks for output interpretation results.
struct OutputInterpreterCallbacks {
  // Called when an assistant message ConversationItem is produced.
  std::function<void(ConversationItem)> on_item;

  // Called when a control signal is produced.
  std::function<void(ControlSignal)> on_signal;
};

// Interprets an LLM ActionCandidate (the result of a completed LLM call)
// into ConversationItems and ControlSignals.
class OutputInterpreter {
 public:
  explicit OutputInterpreter(OutputInterpreterCallbacks callbacks);

  // Interpret a completed LLM result.
  // Produces:
  // - For kResponse: assistant ConversationItem + TurnCompleteSignal
  // - For kToolCall: tool call ConversationItem + ToolCallStartSignal(s)
  // - For kContinue: continuation signal
  void Interpret(const ActionCandidate& candidate,
                 const std::string& conversation_id);

 private:
  OutputInterpreterCallbacks callbacks_;
};

}  // namespace shizuru::core
