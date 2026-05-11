#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "core/control_signal.h"
#include "core/conversation_item.h"
#include "io/audio/audio_device/audio_frame.h"
#include "io/io_device.h"

namespace shizuru::runtime::testing {

// Hand-written mock for IoDevice.
// Records all OnInput calls, tracks Start/Stop state, and can emit frames
// via EmitOutput() for testing routing behavior.
class MockIoDevice : public io::IoDevice {
 public:
  explicit MockIoDevice(std::string device_id,
                        std::vector<io::PortDescriptor> ports = {})
      : device_id_(std::move(device_id)), ports_(std::move(ports)) {}

  std::string GetDeviceId() const override { return device_id_; }

  std::vector<io::PortDescriptor> GetPortDescriptors() const override {
    return ports_;
  }

  void OnInput(const std::string& port_name, io::DataFrame frame) override {
    if (!active_.load()) { return; }
    std::lock_guard<std::mutex> lock(mu_);
    received_frames_.push_back({port_name, std::move(frame)});
  }

  void OnAudioFrame(const std::string& port_name, io::AudioFrame frame) override {
    if (!active_.load()) { return; }
    std::lock_guard<std::mutex> lock(mu_);
    received_audio_frames_.push_back({port_name, std::move(frame)});
  }

  void OnConversationItem(const std::string& port_name,
                          core::ConversationItem item) override {
    if (!active_.load()) { return; }
    std::lock_guard<std::mutex> lock(mu_);
    received_items_.push_back({port_name, std::move(item)});
  }

  void OnControlSignal(const std::string& port_name,
                       core::ControlSignal signal) override {
    if (!active_.load()) { return; }
    std::lock_guard<std::mutex> lock(mu_);
    received_signals_.push_back({port_name, std::move(signal)});
  }

  void SetOutputCallback(io::OutputCallback cb) override {
    std::lock_guard<std::mutex> lock(mu_);
    output_cb_ = std::move(cb);
  }

  void SetAudioFrameOutputCallback(io::AudioFrameOutputCallback cb) override {
    std::lock_guard<std::mutex> lock(mu_);
    audio_output_cb_ = std::move(cb);
  }

  void SetConversationItemOutputCallback(
      io::ConversationItemOutputCallback cb) override {
    std::lock_guard<std::mutex> lock(mu_);
    item_output_cb_ = std::move(cb);
  }

  void SetControlSignalOutputCallback(
      io::ControlSignalOutputCallback cb) override {
    std::lock_guard<std::mutex> lock(mu_);
    signal_output_cb_ = std::move(cb);
  }

  void Start() override {
    active_.store(true);
    ++start_count;
  }

  void Stop() override {
    active_.store(false);
    ++stop_count;
  }

  // Test helper: emit a frame as if this device produced it.
  void EmitOutput(const std::string& port_name, io::DataFrame frame) {
    io::OutputCallback cb;
    {
      std::lock_guard<std::mutex> lock(mu_);
      cb = output_cb_;
    }
    if (cb) { cb(device_id_, port_name, std::move(frame)); }
  }

  void EmitAudioOutput(const std::string& port_name, io::AudioFrame frame) {
    io::AudioFrameOutputCallback cb;
    {
      std::lock_guard<std::mutex> lock(mu_);
      cb = audio_output_cb_;
    }
    if (cb) { cb(device_id_, port_name, std::move(frame)); }
  }

  void EmitConversationItemOutput(const std::string& port_name,
                                  core::ConversationItem item) {
    io::ConversationItemOutputCallback cb;
    {
      std::lock_guard<std::mutex> lock(mu_);
      cb = item_output_cb_;
    }
    if (cb) { cb(device_id_, port_name, std::move(item)); }
  }

  void EmitControlSignalOutput(const std::string& port_name,
                               core::ControlSignal signal) {
    io::ControlSignalOutputCallback cb;
    {
      std::lock_guard<std::mutex> lock(mu_);
      cb = signal_output_cb_;
    }
    if (cb) { cb(device_id_, port_name, std::move(signal)); }
  }

  // Accessors for test assertions.
  bool IsActive() const { return active_.load(); }

  std::vector<std::pair<std::string, io::DataFrame>> ReceivedFrames() const {
    std::lock_guard<std::mutex> lock(mu_);
    return received_frames_;
  }

  std::vector<std::pair<std::string, io::AudioFrame>> ReceivedAudioFrames() const {
    std::lock_guard<std::mutex> lock(mu_);
    return received_audio_frames_;
  }

  std::vector<std::pair<std::string, core::ConversationItem>> ReceivedItems() const {
    std::lock_guard<std::mutex> lock(mu_);
    return received_items_;
  }

  std::vector<std::pair<std::string, core::ControlSignal>> ReceivedSignals() const {
    std::lock_guard<std::mutex> lock(mu_);
    return received_signals_;
  }

  size_t ReceivedCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return received_frames_.size();
  }

  std::atomic<int> start_count{0};
  std::atomic<int> stop_count{0};

 private:
  std::string device_id_;
  std::vector<io::PortDescriptor> ports_;
  std::atomic<bool> active_{false};
  mutable std::mutex mu_;
  io::OutputCallback output_cb_;
  io::AudioFrameOutputCallback audio_output_cb_;
  io::ConversationItemOutputCallback item_output_cb_;
  io::ControlSignalOutputCallback signal_output_cb_;
  std::vector<std::pair<std::string, io::DataFrame>> received_frames_;
  std::vector<std::pair<std::string, io::AudioFrame>> received_audio_frames_;
  std::vector<std::pair<std::string, core::ConversationItem>> received_items_;
  std::vector<std::pair<std::string, core::ControlSignal>> received_signals_;
};

}  // namespace shizuru::runtime::testing
