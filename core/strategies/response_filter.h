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
    std::string result;
    std::string stripped;
    result.reserve(text.size());
    size_t pos = 0;
    while (pos < text.size()) {
      auto open = text.find("<think>", pos);
      if (open == std::string::npos) {
        result.append(text, pos, text.size() - pos);
        break;
      }
      result.append(text, pos, open - pos);
      auto close = text.find("</think>", open);
      if (close == std::string::npos) {
        stripped.append(text, open + 7, text.size() - open - 7);
        break;  // unclosed tag — strip to end
      }
      stripped.append(text, open + 7, close - open - 7);
      pos = close + 8;
    }
    if (!stripped.empty()) {
      LOG_DEBUG("[ResponseFilter] Stripped thinking: \"{}\"", stripped);
    }
    return result;
  }
};

}  // namespace shizuru::core
