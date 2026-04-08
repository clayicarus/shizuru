#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include <portaudio.h>

#include "async_logger.h"
#include "audio_device/audio_buffer.h"
#include "audio_device/audio_frame.h"
#include "audio_device/audio_player.h"
#include "audio_device/port_audio/pa_init.h"

namespace shizuru::io {

class PaPlayer : public AudioPlayer {
 public:
  explicit PaPlayer(const PlayerConfig& config = {})
      : config_(config),
        buf_(config.buffer_capacity_samples, config.channel_count) {
    EnsurePaInitialized();
    // Debug PCM dumps: before ring buffer (Write input) and after (callback output).
    dump_pre_ringbuf_  = std::fopen("pa_pre_ringbuf.pcm", "wb");
    dump_post_ringbuf_ = std::fopen("pa_post_ringbuf.pcm", "wb");
  }

  ~PaPlayer() override {
    Stop();
    if (dump_pre_ringbuf_ != nullptr)  { std::fclose(dump_pre_ringbuf_); }
    if (dump_post_ringbuf_ != nullptr) { std::fclose(dump_post_ringbuf_); }
  }

  PaPlayer(const PaPlayer&) = delete;
  PaPlayer& operator=(const PaPlayer&) = delete;

  void Start() override {
    if (playing_) { return; }

    PaStreamParameters params{};
    params.device = (config_.device_id < 0) ? Pa_GetDefaultOutputDevice()
                                            : config_.device_id;
    if (params.device == paNoDevice) {
      throw std::runtime_error("No output device available");
    }
    params.channelCount          = static_cast<int>(config_.channel_count);
    params.sampleFormat          = paInt16;
    params.suggestedLatency      =
        Pa_GetDeviceInfo(params.device)->defaultLowOutputLatency;
    params.hostApiSpecificStreamInfo = nullptr;

    PaError err = Pa_OpenStream(&stream_, nullptr, &params,
                                static_cast<double>(config_.sample_rate),
                                config_.frames_per_buffer,
                                paClipOff, PaCallback, this);
    if (err != paNoError) {
      throw std::runtime_error(
          std::string("Failed to open output stream: ") + Pa_GetErrorText(err));
    }
    err = Pa_StartStream(stream_);
    if (err != paNoError) {
      Pa_CloseStream(stream_);
      stream_ = nullptr;
      throw std::runtime_error(
          std::string("Failed to start output stream: ") + Pa_GetErrorText(err));
    }
    playing_ = true;
  }

  void Stop() override {
    if (!playing_ || stream_ == nullptr) { return; }
    Pa_StopStream(stream_);
    Pa_CloseStream(stream_);
    stream_  = nullptr;
    playing_ = false;
  }

  [[nodiscard]] bool IsPlaying() const override { return playing_; }

  size_t Write(const AudioFrame& frame) override {
    // Record the timestamp of the first write for playout latency measurement.
    if (!first_write_time_.has_value()) {
      first_write_time_ = Clock::now();
    }
    // Dump raw samples BEFORE they enter the ring buffer.
    if (dump_pre_ringbuf_ != nullptr) {
      std::fwrite(frame.data, sizeof(int16_t), frame.sample_count,
                  dump_pre_ringbuf_);
    }

    // Backpressure: if the ring buffer doesn't have enough space, wait for
    // the PortAudio callback to consume data instead of dropping samples.
    // Sleep in small increments (~5ms) to let the real-time consumer drain.
    // Give up after ~2s to avoid deadlock if playback is stalled.
    // Also bail immediately if Flush() is called (cancel感知).
    flushed_.store(false, std::memory_order_relaxed);
    constexpr int kMaxWaitMs = 2000;
    constexpr int kSleepMs   = 5;
    int waited_ms = 0;
    while (buf_.AvailableWrite() < frame.sample_count &&
           waited_ms < kMaxWaitMs && playing_ &&
           !flushed_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMs));
      waited_ms += kSleepMs;
    }

    // Flush was called while we were waiting — discard this write entirely.
    if (flushed_.load(std::memory_order_acquire)) {
      return 0;
    }

    const size_t written = buf_.Write(frame.data, frame.sample_count);
    if (written < frame.sample_count) {
      LOG_WARN("PaPlayer: playout buffer overflow — dropped {} of {} samples "
               "(buffered={}, capacity={}, waited={}ms)",
               frame.sample_count - written, frame.sample_count,
               buf_.AvailableRead(), config_.buffer_capacity_samples,
               waited_ms);
    }
    return written;
  }

  void Flush() override {
    flushed_.store(true, std::memory_order_release);
    buf_.Reset();
    ResetLatencyCounters();
  }

  [[nodiscard]] size_t Buffered() const override { return buf_.AvailableRead(); }

  // Returns the playout latency: time from first Write() to first callback
  // consumption. Returns nullopt if playback hasn't started consuming yet.
  [[nodiscard]] std::optional<std::chrono::milliseconds> PlayoutLatency() const {
    if (!first_write_time_.has_value() || !first_callback_time_.has_value()) {
      return std::nullopt;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        *first_callback_time_ - *first_write_time_);
  }

  void ResetLatencyCounters() {
    first_write_time_.reset();
    first_callback_time_.reset();
  }

 private:
  using Clock = std::chrono::steady_clock;

  static int PaCallback(const void* /*input*/, void* output,
                        unsigned long frame_count,
                        const PaStreamCallbackTimeInfo* /*time_info*/,
                        PaStreamCallbackFlags /*flags*/, void* user_data) {
    auto* self = static_cast<PaPlayer*>(user_data);
    auto* out  = static_cast<int16_t*>(output);

    const size_t read = self->buf_.Read(out, frame_count);

    // Record the first moment the callback actually consumes real audio data.
    if (read > 0 && !self->first_callback_time_.has_value()) {
      self->first_callback_time_ = Clock::now();
      auto latency = self->PlayoutLatency();
      if (latency.has_value()) {
        LOG_INFO("PaPlayer: first audio playout, buffer latency={}ms",
                 latency->count());
      }
    }

    if (read < frame_count) {
      // Underrun — fill remainder with silence.
      std::memset(out + read * self->config_.channel_count, 0,
                  (frame_count - read) * self->config_.channel_count *
                      sizeof(int16_t));
    }

    // Dump samples AFTER ring buffer read (what PortAudio actually plays).
    if (self->dump_post_ringbuf_ != nullptr && read > 0) {
      std::fwrite(out, sizeof(int16_t), read * self->config_.channel_count,
                  self->dump_post_ringbuf_);
    }

    return paContinue;
  }

  PlayerConfig              config_;
  PaStream*                 stream_  = nullptr;
  bool                      playing_ = false;
  AudioBuffer<int16_t>      buf_;
  std::atomic<bool>         flushed_{false};  // Signals Write() to abort on cancel

  std::optional<Clock::time_point> first_write_time_;
  std::optional<Clock::time_point> first_callback_time_;

  // Debug: PCM dump file handles (raw s16le, same sample rate as config_).
  std::FILE* dump_pre_ringbuf_  = nullptr;  // Write() input — before ring buffer
  std::FILE* dump_post_ringbuf_ = nullptr;  // PaCallback output — after ring buffer
};

}  // namespace shizuru::io
