#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include "io/data_frame.h"

namespace shizuru::io {

struct InterruptFrame {
  static constexpr char kType[] = "interrupt/request";
  static constexpr char kReasonBargeIn[] = "barge_in";

  static DataFrame Make(std::string_view reason,
                        std::string_view source = {}) {
    DataFrame frame;
    frame.type = kType;
    frame.payload.assign(reason.begin(), reason.end());
    frame.timestamp = std::chrono::steady_clock::now();
    if (!reason.empty()) {
      frame.metadata.emplace("reason", std::string(reason));
    }
    if (!source.empty()) {
      frame.metadata.emplace("source", std::string(source));
    }
    return frame;
  }

  static std::string ParseReason(const DataFrame& frame) {
    if (frame.type != kType) { return {}; }
    auto it = frame.metadata.find("reason");
    if (it != frame.metadata.end()) { return it->second; }
    return std::string(frame.payload.begin(), frame.payload.end());
  }

  static std::string ParseSource(const DataFrame& frame) {
    if (frame.type != kType) { return {}; }
    auto it = frame.metadata.find("source");
    if (it != frame.metadata.end()) { return it->second; }
    return {};
  }
};

}  // namespace shizuru::io
