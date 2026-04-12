#pragma once

#include <cstring>
#include <memory>
#include <stdexcept>

#include <oboe/Oboe.h>

#include "audio_device/audio_buffer.h"
#include "audio_device/audio_frame.h"
#include "audio_device/audio_recorder.h"

namespace shizuru::io {

class OboeRecorder : public AudioRecorder,
                     public oboe::AudioStreamDataCallback {
 public:
  explicit OboeRecorder(const RecorderConfig& config = {})
      : config_(config),
        buf_(config.buffer_capacity_samples, config.channel_count) {}

  ~OboeRecorder() override { Stop(); }

  OboeRecorder(const OboeRecorder&) = delete;
  OboeRecorder& operator=(const OboeRecorder&) = delete;

  void Start() override {
    if (recording_) { return; }

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input)
           ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
           ->setSharingMode(oboe::SharingMode::Exclusive)
           ->setFormat(oboe::AudioFormat::I16)
           ->setSampleRate(config_.sample_rate)
           ->setChannelCount(static_cast<int>(config_.channel_count))
           ->setFramesPerDataCallback(static_cast<int>(config_.frames_per_buffer))
           ->setInputPreset(oboe::InputPreset::VoiceCommunication)
           ->setDataCallback(this);

    oboe::Result result = builder.openStream(stream_);
    if (result != oboe::Result::OK) {
      throw std::runtime_error(
          std::string("Failed to open Oboe input stream: ") +
          oboe::convertToText(result));
    }

    result = stream_->requestStart();
    if (result != oboe::Result::OK) {
      stream_->close();
      stream_.reset();
      throw std::runtime_error(
          std::string("Failed to start Oboe input stream: ") +
          oboe::convertToText(result));
    }
    recording_ = true;
  }

  void Stop() override {
    if (!recording_ || !stream_) { return; }
    stream_->requestStop();
    stream_->close();
    stream_.reset();
    recording_ = false;
  }

  [[nodiscard]] bool IsRecording() const override { return recording_; }

  size_t Read(AudioFrame& frame) override {
    frame.sample_rate   = config_.sample_rate;
    frame.channel_count = config_.channel_count;
    const size_t request = (frame.sample_count > 0)
                               ? frame.sample_count
                               : config_.frames_per_buffer;
    const size_t read = buf_.Read(frame.data, request);
    frame.sample_count = read;
    return read;
  }

  void SetFrameCallback(FrameCallback cb) override {
    frame_callback_ = std::move(cb);
  }

  // oboe::AudioStreamDataCallback
  oboe::DataCallbackResult onAudioReady(
      oboe::AudioStream* /*stream*/, void* audio_data,
      int32_t num_frames) override {
    const auto* src = static_cast<const int16_t*>(audio_data);
    buf_.Write(src, static_cast<size_t>(num_frames));

    if (frame_callback_) {
      AudioFrame frame;
      frame.sample_rate   = config_.sample_rate;
      frame.channel_count = config_.channel_count;
      frame.sample_count  = static_cast<size_t>(num_frames);
      std::memcpy(frame.data, src,
                  static_cast<size_t>(num_frames) * config_.channel_count *
                      sizeof(int16_t));
      frame_callback_(frame);
    }
    return oboe::DataCallbackResult::Continue;
  }

 private:
  RecorderConfig config_;
  std::shared_ptr<oboe::AudioStream> stream_;
  bool recording_ = false;
  AudioBuffer<int16_t> buf_;
  FrameCallback frame_callback_;
};

}  // namespace shizuru::io
