#pragma once

#include <functional>
#include <string>
#include <vector>

#include "async_logger.h"
#include "core/content_part.h"
#include "core/control_signal.h"
#include "core/conversation_item.h"
#include "io/audio/audio_device/audio_frame.h"
#include "io/io_device.h"

namespace shizuru::io {

// A pass-through IoDevice that logs typed payloads and re-emits them unchanged
// on a matching output port so it can be chained in the bus.
//
// Port contract:
//   Input  "audio_in" / "item_in" / "signal_in"
//   Output "audio_out" / "item_out" / "signal_out"
//
// Usage: insert between any two devices in the RouteTable.
//
//   Before: A.out → B.in
//   After:  A.out → log.<kind>_in → log.<kind>_out → B.in
class LogDevice : public IoDevice {
 public:
  explicit LogDevice(std::string device_id,
                     spdlog::level::level_enum level = spdlog::level::info);

  std::string GetDeviceId() const override;
  std::vector<PortDescriptor> GetPortDescriptors() const override;
  void OnAudioFrame(const std::string& port_name, AudioFrame frame) override;
  void OnConversationItem(const std::string& port_name,
                          core::ConversationItem item) override;
  void OnControlSignal(const std::string& port_name,
                       core::ControlSignal signal) override;
  void SetAudioFrameOutputCallback(AudioFrameOutputCallback cb) override;
  void SetConversationItemOutputCallback(
      ConversationItemOutputCallback cb) override;
  void SetControlSignalOutputCallback(ControlSignalOutputCallback cb) override;
  void Start() override;
  void Stop() override;

  static constexpr char kAudioIn[] = "audio_in";
  static constexpr char kAudioOut[] = "audio_out";
  static constexpr char kItemIn[] = "item_in";
  static constexpr char kItemOut[] = "item_out";
  static constexpr char kSignalIn[] = "signal_in";
  static constexpr char kSignalOut[] = "signal_out";

 private:
  std::string FormatAudioFrame(const AudioFrame& frame) const;
  std::string FormatConversationItem(const core::ConversationItem& item) const;
  std::string FormatControlSignal(const core::ControlSignal& signal) const;

  std::string device_id_;
  spdlog::level::level_enum level_;
  AudioFrameOutputCallback audio_output_cb_;
  ConversationItemOutputCallback item_output_cb_;
  ControlSignalOutputCallback signal_output_cb_;
};

}  // namespace shizuru::io
