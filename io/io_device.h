#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/control_signal.h"
#include "core/conversation_item.h"
#include "io/audio/audio_device/audio_frame.h"
#include "runtime/port_payload_kind.h"

namespace shizuru::io {

// Raw data frame for perception-layer transport (audio, video, raw events).
// This is NOT a semantic type — it carries raw bytes between IO devices.
struct DataFrame {
  std::string type;                                    // MIME-like: "audio/pcm", "text/plain"
  std::vector<uint8_t> payload;                        // Raw bytes
  std::string source_device;
  std::string source_port;
  std::chrono::steady_clock::time_point timestamp;
  std::unordered_map<std::string, std::string> metadata;
};

enum class PortDirection { kInput, kOutput };

struct PortDescriptor {
  std::string name;       // e.g., "audio_in", "text_out"
  PortDirection direction;
  std::string data_type;  // MIME-like: "audio/pcm", "text/plain", etc.
  runtime::PortPayloadKind payload_kind =
      runtime::PortPayloadKind::kLegacyFrame;
};

using OutputCallback = std::function<void(
    const std::string& device_id,
    const std::string& port_name,
    DataFrame frame)>;

using AudioFrameOutputCallback = std::function<void(
    const std::string& device_id,
    const std::string& port_name,
    AudioFrame frame)>;

using ConversationItemOutputCallback = std::function<void(
    const std::string& device_id,
    const std::string& port_name,
    core::ConversationItem item)>;

using ControlSignalOutputCallback = std::function<void(
    const std::string& device_id,
    const std::string& port_name,
    core::ControlSignal signal)>;

class IoDevice {
 public:
  virtual ~IoDevice() = default;

  // Unique identifier for this device instance.
  virtual std::string GetDeviceId() const = 0;

  // Ports this device exposes.
  virtual std::vector<PortDescriptor> GetPortDescriptors() const = 0;

  // Accept an incoming data frame on a named input port.
  virtual void OnInput(const std::string& port_name, DataFrame frame) = 0;

  // Typed semantic/control/raw-plane callbacks used by the unified pipeline.
  // Default no-op implementations let each device override only the planes it
  // participates in.
  virtual void OnAudioFrame(const std::string& port_name, AudioFrame frame) {
    (void)port_name;
    (void)frame;
  }

  virtual void OnConversationItem(const std::string& port_name,
                                  core::ConversationItem item) {
    (void)port_name;
    (void)item;
  }

  virtual void OnControlSignal(const std::string& port_name,
                               core::ControlSignal signal) {
    (void)port_name;
    (void)signal;
  }

  // Register the callback the device uses to emit output frames.
  virtual void SetOutputCallback(OutputCallback cb) = 0;

  virtual void SetAudioFrameOutputCallback(AudioFrameOutputCallback cb) {
    (void)cb;
  }

  virtual void SetConversationItemOutputCallback(
      ConversationItemOutputCallback cb) {
    (void)cb;
  }

  virtual void SetControlSignalOutputCallback(ControlSignalOutputCallback cb) {
    (void)cb;
  }

  // Lifecycle.
  virtual void Start() = 0;
  virtual void Stop() = 0;
};

}  // namespace shizuru::io
