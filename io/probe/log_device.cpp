#include "log_device.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace shizuru::io {

LogDevice::LogDevice(std::string device_id,
                     spdlog::level::level_enum level,
                     Formatter formatter)
    : device_id_(std::move(device_id)),
      level_(level),
      formatter_(std::move(formatter)) {}

std::string LogDevice::GetDeviceId() const { return device_id_; }

std::vector<PortDescriptor> LogDevice::GetPortDescriptors() const {
  return {
      {kPassIn,  PortDirection::kInput,  "*"},
      {kPassOut, PortDirection::kOutput, "*"},
  };
}

void LogDevice::OnInput(const std::string& port_name, DataFrame frame) {
  if (port_name != kPassIn) { return; }

  const std::string detail = formatter_ ? formatter_(frame) : FormatFrame(frame);
  core::GetLogger()->log(level_, "[{}] {}", device_id_, detail);

  if (output_cb_) {
    output_cb_(device_id_, kPassOut, std::move(frame));
  }
}

void LogDevice::SetOutputCallback(OutputCallback cb) {
  output_cb_ = std::move(cb);
}

void LogDevice::Start() {}
void LogDevice::Stop()  {}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

std::string LogDevice::FormatFrame(const DataFrame& frame) const {
  if (frame.type == "text/plain" || frame.type == "text/json") {
    const std::string text(
        reinterpret_cast<const char*>(frame.payload.data()),
        std::min(frame.payload.size(), size_t{120}));
    const bool truncated = frame.payload.size() > 120;
    return frame.type + " \"" + text + (truncated ? "..." : "") + "\"";
  }
  return frame.type + " " + std::to_string(frame.payload.size()) + " bytes"
       + " [src=" + frame.source_device + "." + frame.source_port + "]";
}

}  // namespace shizuru::io
