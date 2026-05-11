#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace shizuru::core {

struct TextPart {
  std::string text;
};

struct ImagePart {
  std::string url;
};

struct AudioPart {
  std::vector<uint8_t> data;
  std::string format;
};

struct ToolCallPart {
  std::string tool_call_id;
  std::string name;
  std::string arguments_json;
};

struct ToolResultPart {
  std::string tool_call_id;
  std::string tool_name;
  bool success = true;
  std::string result_json;
};

using ContentPart =
    std::variant<TextPart, ImagePart, AudioPart, ToolCallPart, ToolResultPart>;

using ContentParts = std::vector<ContentPart>;

}  // namespace shizuru::core
