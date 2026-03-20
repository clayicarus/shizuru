#pragma once

#include <functional>
#include <string>
#include <vector>

#include "io/io_device.h"
#include "async_logger.h"

namespace shizuru::io {

// A pass-through IoDevice that logs every DataFrame it receives, then
// re-emits it unchanged on "pass_out" so it can be chained in the bus.
//
// Port contract:
//   Input  "pass_in"  — accepts any DataFrame type
//   Output "pass_out" — re-emits the same DataFrame unchanged
//
// Usage: insert between any two devices in the RouteTable.
//
//   Before: A.out → B.in
//   After:  A.out → log.pass_in → log.pass_out → B.in
class LogDevice : public IoDevice {
 public:
  // Formatter: given a DataFrame, return a string to append to the log line.
  // Default formatter prints type + byte count (+ text preview for text/plain).
  using Formatter = std::function<std::string(const DataFrame&)>;

  explicit LogDevice(std::string device_id,
                     spdlog::level::level_enum level = spdlog::level::info,
                     Formatter formatter = nullptr);

  std::string GetDeviceId() const override;
  std::vector<PortDescriptor> GetPortDescriptors() const override;
  void OnInput(const std::string& port_name, DataFrame frame) override;
  void SetOutputCallback(OutputCallback cb) override;
  void Start() override;
  void Stop() override;

  static constexpr char kPassIn[]  = "pass_in";
  static constexpr char kPassOut[] = "pass_out";

 private:
  std::string FormatFrame(const DataFrame& frame) const;

  std::string device_id_;
  spdlog::level::level_enum level_;
  Formatter formatter_;
  OutputCallback output_cb_;
};

}  // namespace shizuru::io
