#include "energy_vad_device.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <utility>

namespace shizuru::io {

namespace {

AudioFrame AudioFrameFromDataFrame(const DataFrame& frame) {
  AudioFrame af;
  auto it = frame.metadata.find("sample_rate");
  if (it != frame.metadata.end()) {
    af.sample_rate = std::stoi(it->second);
  }
  it = frame.metadata.find("channel_count");
  if (it != frame.metadata.end()) {
    af.channel_count = static_cast<size_t>(std::stoi(it->second));
  }
  const size_t total_samples = frame.payload.size() / sizeof(int16_t);
  const size_t capped = std::min(total_samples, kMaxSamplesPerFrame);
  af.sample_count = af.channel_count == 0 ? 0 : capped / af.channel_count;
  std::memcpy(af.data, frame.payload.data(), capped * sizeof(int16_t));
  return af;
}

}  // namespace

EnergyVadDevice::EnergyVadDevice(EnergyVadConfig config, std::string device_id)
    : device_id_(std::move(device_id)), config_(config) {}

std::string EnergyVadDevice::GetDeviceId() const { return device_id_; }

std::vector<PortDescriptor> EnergyVadDevice::GetPortDescriptors() const {
  return {
      {kAudioIn,  PortDirection::kInput,  "audio/pcm",
                  runtime::PortPayloadKind::kAudioFrame},
      {kAudioOut, PortDirection::kOutput, "audio/pcm",
                  runtime::PortPayloadKind::kAudioFrame},
      {kVadOut,   PortDirection::kOutput, "vad/event"},
      {kInterruptSignalOut, PortDirection::kOutput, "",
                            runtime::PortPayloadKind::kControlSignal},
      {kControlSignalOut, PortDirection::kOutput, "",
                          runtime::PortPayloadKind::kControlSignal},
  };
}

void EnergyVadDevice::SetOutputCallback(OutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  output_cb_ = std::move(cb);
}

void EnergyVadDevice::SetAudioFrameOutputCallback(AudioFrameOutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  audio_output_cb_ = std::move(cb);
}

void EnergyVadDevice::SetControlSignalOutputCallback(ControlSignalOutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  signal_output_cb_ = std::move(cb);
}

void EnergyVadDevice::Start() {
  in_speech_        = false;
  onset_counter_    = 0;
  hangover_counter_ = 0;
  rms_window_.clear();
  pre_roll_audio_buf_.clear();
  active_.store(true);
}

void EnergyVadDevice::Stop() { active_.store(false); }

void EnergyVadDevice::OnInput(const std::string& port_name, DataFrame frame) {
  if (port_name != kAudioIn) { return; }
  if (frame.type != "audio/pcm" || frame.payload.empty()) { return; }
  OnAudioFrame(port_name, AudioFrameFromDataFrame(frame));
}

void EnergyVadDevice::OnAudioFrame(const std::string& port_name, AudioFrame frame) {
  if (!active_.load()) { return; }
  if (port_name != kAudioIn) { return; }
  if (frame.sample_count == 0) { return; }

  const float rms = ComputeRms(frame);

  rms_window_.push_back(rms);
  if (rms_window_.size() > config_.rms_window_frames) {
    rms_window_.pop_front();
  }
  const float window_max =
      *std::max_element(rms_window_.begin(), rms_window_.end());
  const bool is_speech = (window_max >= config_.energy_threshold);

  if (!in_speech_) {
    pre_roll_audio_buf_.push_back(frame);
    if (pre_roll_audio_buf_.size() > config_.pre_roll_frames) {
      pre_roll_audio_buf_.pop_front();
    }

    if (is_speech) {
      ++onset_counter_;
      hangover_counter_ = 0;
      if (onset_counter_ >= config_.speech_onset_frames) {
        in_speech_ = true;
        onset_counter_ = 0;
        EmitEvent("speech_start");
        EmitInterruptSignal();
        if (config_.pre_roll_frames == 0) {
          EmitAudio(std::move(frame));
        } else {
          FlushPreRollTyped();
        }
      }
    } else {
      onset_counter_ = 0;
    }
  } else {
    if (is_speech) {
      hangover_counter_ = 0;
      EmitAudio(std::move(frame));
      EmitEvent("speech_active");
    } else {
      ++hangover_counter_;
      EmitAudio(std::move(frame));
      if (hangover_counter_ >= config_.silence_hangover_frames) {
        in_speech_ = false;
        hangover_counter_ = 0;
        pre_roll_audio_buf_.clear();
        EmitEvent("speech_end");
        EmitFlushSignal();
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

float EnergyVadDevice::ComputeRms(const AudioFrame& frame) {
  const size_t num_samples = frame.NumSamples();
  if (num_samples == 0) { return 0.0F; }

  double sum_sq = 0.0;
  for (size_t i = 0; i < num_samples; ++i) {
    const double s = static_cast<double>(frame.data[i]);
    sum_sq += s * s;
  }
  return static_cast<float>(std::sqrt(sum_sq / static_cast<double>(num_samples)));
}

void EnergyVadDevice::EmitAudio(AudioFrame frame) {
  AudioFrameOutputCallback cb;
  {
    std::lock_guard<std::mutex> lock(output_cb_mutex_);
    cb = audio_output_cb_;
  }
  if (cb) { cb(device_id_, kAudioOut, std::move(frame)); }
}

void EnergyVadDevice::EmitEvent(const std::string& event) {
  DataFrame frame;
  frame.type    = "vad/event";
  frame.payload.assign(event.begin(), event.end());
  frame.source_device = device_id_;
  frame.source_port   = kVadOut;
  frame.timestamp     = std::chrono::steady_clock::now();

  OutputCallback cb;
  {
    std::lock_guard<std::mutex> lock(output_cb_mutex_);
    cb = output_cb_;
  }
  if (cb) { cb(device_id_, kVadOut, std::move(frame)); }
}

void EnergyVadDevice::EmitInterruptSignal() {
  ControlSignalOutputCallback cb;
  {
    std::lock_guard<std::mutex> lock(output_cb_mutex_);
    cb = signal_output_cb_;
  }
  if (cb) {
    cb(device_id_, kInterruptSignalOut, core::InterruptSignal{});
  }
}

void EnergyVadDevice::EmitFlushSignal() {
  ControlSignalOutputCallback cb;
  {
    std::lock_guard<std::mutex> lock(output_cb_mutex_);
    cb = signal_output_cb_;
  }
  if (cb) {
    cb(device_id_, kControlSignalOut, core::FlushSignal{});
  }
}

void EnergyVadDevice::FlushPreRollTyped() {
  for (auto& f : pre_roll_audio_buf_) {
    AudioFrameOutputCallback cb;
    {
      std::lock_guard<std::mutex> lock(output_cb_mutex_);
      cb = audio_output_cb_;
    }
    if (cb) { cb(device_id_, kAudioOut, f); }
  }
  pre_roll_audio_buf_.clear();
}

}  // namespace shizuru::io
