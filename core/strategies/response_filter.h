#pragma once

#include <string>

#include "async_logger.h"

namespace shizuru::core {

// Transforms or filters the final response text before it is emitted
// to downstream consumers (display, TTS, etc.).
//
// Injection point: Controller::HandleResponding, before emitting the frame.
//
// Use cases:
//   - Strip <think>...</think> reasoning blocks
//   - Remove markdown formatting for voice output
//   - Suppress empty or whitespace-only responses
//
// Default implementation: pass through unchanged.
class ResponseFilter {
 public:
  virtual ~ResponseFilter() = default;

  // Transform the response text.  Return empty string to suppress output.
  virtual std::string Filter(const std::string& text) = 0;
};

// Default: no transformation.
class PassthroughFilter : public ResponseFilter {
 public:
  std::string Filter(const std::string& text) override { return text; }
};

// Strips <think>...</think> blocks from the response.
class StripThinkingFilter : public ResponseFilter {
 public:
  std::string Filter(const std::string& text) override {
    // Strip all structured blocks: <think>, <tool_call>, <tool_result>.
    std::string result = StripTag(text, "<think>", "</think>");
    result = StripTag(result, "<tool_call>", "</tool_call>");
    result = StripTag(result, "<tool_result>", "</tool_result>");
    return result;
  }

 private:
  static std::string StripTag(const std::string& text,
                               const std::string& open_tag,
                               const std::string& close_tag) {
    std::string result;
    result.reserve(text.size());
    size_t pos = 0;
    while (pos < text.size()) {
      auto open = text.find(open_tag, pos);
      if (open == std::string::npos) {
        result.append(text, pos, text.size() - pos);
        break;
      }
      result.append(text, pos, open - pos);
      auto close = text.find(close_tag, open);
      if (close == std::string::npos) {
        break;  // unclosed tag — strip to end
      }
      pos = close + close_tag.size();
    }
    return result;
  }
};

}  // namespace shizuru::core
