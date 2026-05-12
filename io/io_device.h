#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/control_signal.h"
#include "core/conversation_item.h"
#include "io/audio/audio_device/audio_frame.h"
#include "runtime/port_payload_kind.h"

namespace shizuru::io {

enum class PortDirection { kInput, kOutput };

struct PortDescriptor {
  std::string name;       // e.g., "audio_in", "text_out"
  PortDirection direction;
  std::string data_type;  // MIME-like: "audio/pcm", "text/plain", etc.
  runtime::PortPayloadKind payload_kind;
};

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
