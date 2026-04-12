#pragma once

#include <cstring>
#include <stdexcept>
#include <string>

#include <portaudio.h>

#include "audio_device/audio_buffer.h"
#include "audio_device/audio_frame.h"
#include "audio_device/audio_recorder.h"
#include "audio_device/port_audio/pa_init.h"

namespace shizuru::io {

class PaRecorder : public AudioRecorder {
 public:
  explicit PaRecorder(const RecorderConfig& config = {})
      : config_(config),
        buf_(config.buffer_capacity_samples, config.channel_count) {}

  ~PaRecorder() override { Stop(); }

  PaRecorder(const PaRecorder&) = delete;
  PaRecorder& operator=(const PaRecorder&) = delete;

  void Start() override {
    if (recording_) { return; }
    EnsurePaInitialized();

    PaStreamParameters params{};
    params.device = (config_.device_id < 0) ? Pa_GetDefaultInputDevice()
                                            : config_.device_id;
    if (params.device == paNoDevice) {
      throw std::runtime_error("No input device available");
    }
    params.channelCount          = static_cast<int>(config_.channel_count);
    params.sampleFormat          = paInt16;
    params.suggestedLatency      =
        Pa_GetDeviceInfo(params.device)->defaultLowInputLatency;
    params.hostApiSpecificStreamInfo = nullptr;

    PaError err = Pa_OpenStream(&stream_, &params, nullptr,
                                static_cast<double>(config_.sample_rate),
                                config_.frames_per_buffer,
                                paClipOff, PaCallback, this);
    if (err != paNoError) {
      throw std::runtime_error(
          std::string("Failed to open input stream: ") + Pa_GetErrorText(err));
    }
    err = Pa_StartStream(stream_);
    if (err != paNoError) {
      Pa_CloseStream(stream_);
      stream_ = nullptr;
      throw std::runtime_error(
          std::string("Failed to start input stream: ") + Pa_GetErrorText(err));
    }
    recording_ = true;
  }

  void Stop() override {
    if (!recording_ || stream_ == nullptr) { return; }
    Pa_StopStream(stream_);
    Pa_CloseStream(stream_);
    stream_    = nullptr;
    recording_ = false;
  }

  [[nodiscard]] bool IsRecording() const override { return recording_; }

  size_t Read(AudioFrame& frame) override {
    frame.sample_rate   = config_.sample_rate;
    frame.channel_count = config_.channel_count;
    // If caller didn't specify how many samples to read, use frames_per_buffer.
    const size_t request = (frame.sample_count > 0)
                               ? frame.sample_count
                               : config_.frames_per_buffer;
    const size_t read    = buf_.Read(frame.data, request);
    frame.sample_count   = read;
    return read;
  }

  void SetFrameCallback(FrameCallback cb) override {
    frame_callback_ = std::move(cb);
  }

 private:
  static int PaCallback(const void* input, void* /*output*/,
                        unsigned long frame_count,
                        const PaStreamCallbackTimeInfo* /*time_info*/,
                        PaStreamCallbackFlags /*flags*/, void* user_data) {
    auto* self = static_cast<PaRecorder*>(user_data);
    if (input == nullptr) { return paContinue; }

    const auto* src = static_cast<const int16_t*>(input);
    self->buf_.Write(src, frame_count);

    if (self->frame_callback_) {
      AudioFrame frame;
      frame.sample_rate   = self->config_.sample_rate;
      frame.channel_count = self->config_.channel_count;
      frame.sample_count  = frame_count;
      std::memcpy(frame.data, src,
                  frame_count * self->config_.channel_count * sizeof(int16_t));
      self->frame_callback_(frame);
    }
    return paContinue;
  }

  RecorderConfig        config_;
  PaStream*             stream_    = nullptr;
  bool                  recording_ = false;
  AudioBuffer<int16_t>  buf_;
  FrameCallback         frame_callback_;
};

}  // namespace shizuru::io
