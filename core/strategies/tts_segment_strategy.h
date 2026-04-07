#pragma once

#include <cstddef>
#include <string>

namespace shizuru::core {

// Decides when accumulated streaming tokens form a chunk suitable for TTS.
//
// Injection point: Controller::HandleThinking, inside the streaming callback.
// The Controller feeds each token into this strategy.  When the strategy
// signals readiness, the Controller emits the buffered text as a TTS-ready
// frame via emit_frame_.
//
// Default implementation: punctuation-based segmentation.
// Advanced implementation: inject an auxiliary LlmClient for semantic
// completeness checking (the strategy owns its own LLM dependency).
class TtsSegmentStrategy {
 public:
  virtual ~TtsSegmentStrategy() = default;

  // Called for each streaming token.  The strategy accumulates internally.
  // Returns the number of leading characters from the internal buffer that
  // are ready to be flushed to TTS.  Returns 0 if not enough content yet.
  //
  // After the Controller reads a non-zero value, it will call Consume()
  // to remove those characters from the buffer.
  virtual size_t ReadyLength() = 0;

  // Append a new token to the internal buffer.
  virtual void Append(const std::string& token) = 0;

  // Remove the first `n` characters from the internal buffer.
  // Called by Controller after emitting a TTS-ready frame.
  virtual void Consume(size_t n) = 0;

  // Flush: return all remaining buffered text and clear the buffer.
  // Called when the LLM response is complete (end of streaming).
  virtual std::string Flush() = 0;

  // Reset internal state.  Called on interrupt or session end.
  virtual void Reset() = 0;
};

// Default: punctuation-based segmentation.
// Flushes at sentence-ending punctuation when buffer >= min_chars,
// or force-flushes when buffer >= max_chars.
class PunctuationSegmentStrategy : public TtsSegmentStrategy {
 public:
  struct Config {
    size_t min_chars;
    size_t max_chars;
    Config() : min_chars(10), max_chars(200) {}
  };

  explicit PunctuationSegmentStrategy(Config config = Config{})
      : config_(config) {}

  void Append(const std::string& token) override {
    buffer_ += token;
  }

  size_t ReadyLength() override {
    if (buffer_.size() >= config_.max_chars) {
      return buffer_.size();
    }
    if (buffer_.size() < config_.min_chars) {
      return 0;
    }
    // Scan for the latest sentence-ending punctuation.
    for (size_t i = buffer_.size(); i > 0; --i) {
      char c = buffer_[i - 1];
      if (c == '.' || c == '?' || c == '!' ||
          c == ';' || c == '\n') {
        return i;
      }
    }
    return 0;
  }

  void Consume(size_t n) override {
    if (n >= buffer_.size()) {
      buffer_.clear();
    } else {
      buffer_.erase(0, n);
    }
  }

  std::string Flush() override {
    std::string result = std::move(buffer_);
    buffer_.clear();
    return result;
  }

  void Reset() override {
    buffer_.clear();
  }

 private:
  Config config_;
  std::string buffer_;
};

}  // namespace shizuru::core
