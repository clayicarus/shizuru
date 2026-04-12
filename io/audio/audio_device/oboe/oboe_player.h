#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

#include <oboe/Oboe.h>

#include "async_logger.h"
#include "audio_device/audio_buffer.h"
#include "audio_device/audio_frame.h"
#include "audio_device/audio_player.h"

namespace shizuru::io {

class OboePlayer : public AudioPlayer,
                   public oboe::AudioStreamDataCallback {
 public:
  explicit OboePlayer(const PlayerConfig& config = {})
      : config_(config),
        buf_(config.buffer_capacity_samples, config.channel_count) {}

  ~OboePlayer() override { Stop(); }

  OboePlayer(const OboePlayer&) = delete;
  OboePlayer& operator=(const OboePlayer&) = delete;

  void Start() override {
    if (playing_) { return; }

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output)
           ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
           ->setSharingMode(oboe::SharingMode::Exclusive)
           ->setFormat(oboe::AudioFormat::I16)
           ->setSampleRate(config_.sample_rate)
           ->setChannelCount(static_cast<int>(config_.channel_count))
           ->setFramesPerDataCallback(static_cast<int>(config_.frames_per_buffer))
           ->setUsage(oboe::Usage::VoiceCommunication)
           ->setDataCallback(this);

    oboe::Result result = builder.openStream(stream_);
    if (result != oboe::Result::OK) {
      throw std::runtime_error(
          std::string("Failed to open Oboe output stream: ") +
          oboe::convertToText(result));
    }

    result = stream_->requestStart();
    if (result != oboe::Result::OK) {
      stream_->close();
      stream_.reset();
      throw std::runtime_error(
          std::string("Failed to start Oboe output stream: ") +
          oboe::convertToText(result));
    }
    playing_ = true;
  }

  void Stop() override {
    if (!playing_ || !stream_) { return; }
    stream_->requestStop();
    stream_->close();
    stream_.reset();
    playing_ = false;
  }

  [[nodiscard]] bool IsPlaying() const override { return playing_; }

  size_t Write(const AudioFrame& frame) override {
    if (!first_write_time_.has_value()) {
      first_write_time_ = Clock::now();
    }

    // Backpressure: wait for the callback to drain the buffer.
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

    if (flushed_.load(std::memory_order_acquire)) {
      return 0;
    }

    const size_t written = buf_.Write(frame.data, frame.sample_count);
    if (written < frame.sample_count) {
      LOG_WARN("OboePlayer: playout buffer overflow — dropped {} of {} samples "
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

  // oboe::AudioStreamDataCallback
  oboe::DataCallbackResult onAudioReady(
      oboe::AudioStream* /*stream*/, void* audio_data,
      int32_t num_frames) override {
    auto* out = static_cast<int16_t*>(audio_data);
    const size_t read = buf_.Read(out, static_cast<size_t>(num_frames));

    if (read > 0 && !first_callback_time_.has_value()) {
      first_callback_time_ = Clock::now();
      auto latency = PlayoutLatency();
      if (latency.has_value()) {
        LOG_INFO("OboePlayer: first audio playout, buffer latency={}ms",
                 latency->count());
      }
    }

    if (read < static_cast<size_t>(num_frames)) {
      std::memset(out + read * config_.channel_count, 0,
                  (static_cast<size_t>(num_frames) - read) *
                      config_.channel_count * sizeof(int16_t));
    }
    return oboe::DataCallbackResult::Continue;
  }

 private:
  using Clock = std::chrono::steady_clock;

  PlayerConfig config_;
  std::shared_ptr<oboe::AudioStream> stream_;
  bool playing_ = false;
  AudioBuffer<int16_t> buf_;
  std::atomic<bool> flushed_{false};

  std::optional<Clock::time_point> first_write_time_;
  std::optional<Clock::time_point> first_callback_time_;
};

}  // namespace shizuru::io
