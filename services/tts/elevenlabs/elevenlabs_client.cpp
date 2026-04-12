#include "elevenlabs_client.h"

#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "async_logger.h"
#include "services/utils/curl_helper.h"

namespace shizuru::services {

// ---------------------------------------------------------------------------
// File-scope helpers
// ---------------------------------------------------------------------------

[[noreturn]] static void ThrowApiError(int status, const std::string& body) {
  LOG_ERROR("[TTS] API error status={} body={}", status, body);
  std::string detail = body;
  try {
    auto j = nlohmann::json::parse(body);
    if (j.contains("detail")) {
      const auto& d = j["detail"];
      if (d.is_string()) {
        detail = d.get<std::string>();
      } else if (d.is_object() && d.contains("message")) {
        detail = d["message"].get<std::string>();
      } else {
        detail = d.dump();
      }
    }
  } catch (const std::exception& /*e*/) {
    (void)0;
  }
  throw std::runtime_error("ElevenLabs API error " +
                            std::to_string(status) + ": " + detail);
}

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

ElevenLabsClient::ElevenLabsClient(ElevenLabsConfig config)
    : config_(std::move(config)) {}

ElevenLabsClient::~ElevenLabsClient() = default;

// ---------------------------------------------------------------------------
// TtsClient interface — streaming endpoint
// ---------------------------------------------------------------------------

void ElevenLabsClient::Synthesize(const TtsRequest& request,
                                   TtsAudioCallback on_audio) {
  std::lock_guard<std::mutex> lock(request_mutex_);
  cancel_requested_.store(false);

  const std::string& voice_id = ResolveVoiceId(request);
  const std::string path =
      "/v1/text-to-speech/" + voice_id + "/stream" + QueryString();
  const std::string body = BuildBody(request);
  const std::string url = config_.base_url + path;

  LOG_DEBUG("[TTS] Synthesize (stream) voice={} format={} url={}",
            voice_id, TtsOutputFormatString(config_.output_format), url);

  size_t total_bytes = 0;
  bool first_chunk_logged = false;

  // Accumulate error body in case of non-200 status.
  std::string error_body;
  bool got_error = false;

  auto on_data = [&](const char* data, size_t len) -> bool {
    if (cancel_requested_.load()) { return false; }
    if (len > 0 && on_audio) {
      if (!first_chunk_logged) {
        LOG_INFO("[TTS] TTS first audio chunk: {} bytes (voice={})",
                 len, voice_id);
        first_chunk_logged = true;
      }
      on_audio(data, len);
      total_bytes += len;
    }
    return true;
  };

  long status = CurlPostStreaming(
      url,
      {"xi-api-key: " + config_.api_key,
       "Content-Type: application/json",
       "Accept: audio/*"},
      body,
      config_.connect_timeout,
      config_.read_timeout,
      on_data,
      cancel_requested_);

  if (cancel_requested_.load()) {
    LOG_WARN("[TTS] Synthesize cancelled (voice={})", voice_id);
    throw std::runtime_error("TTS request cancelled");
  }
  if (status != 200) {
    ThrowApiError(static_cast<int>(status), "streaming request failed");
  }

  LOG_INFO("[TTS] Synthesize complete: {} bytes (voice={})",
           total_bytes, voice_id);
}

void ElevenLabsClient::Synthesize(const std::string& text,
                                   TtsAudioCallback on_audio) {
  TtsRequest req;
  req.text = text;
  Synthesize(req, std::move(on_audio));
}

// ---------------------------------------------------------------------------
// Non-streaming endpoint
// ---------------------------------------------------------------------------

void ElevenLabsClient::SynthesizeFull(const TtsRequest& request,
                                       const TtsAudioCallback& on_audio) {
  std::lock_guard<std::mutex> lock(request_mutex_);
  cancel_requested_.store(false);

  const std::string& voice_id = ResolveVoiceId(request);
  const std::string path =
      "/v1/text-to-speech/" + voice_id + QueryString();
  const std::string body = BuildBody(request);
  const std::string url = config_.base_url + path;

  LOG_DEBUG("[TTS] SynthesizeFull voice={} format={} url={}",
            voice_id, TtsOutputFormatString(config_.output_format), url);

  auto res = CurlPost(
      url,
      {"xi-api-key: " + config_.api_key,
       "Content-Type: application/json",
       "Accept: audio/*"},
      body,
      config_.connect_timeout,
      config_.read_timeout);

  if (res.status_code != 200) {
    ThrowApiError(static_cast<int>(res.status_code), res.body);
  }

  if (!res.body.empty() && on_audio) {
    on_audio(res.body.data(), res.body.size());
  }

  LOG_INFO("[TTS] SynthesizeFull complete: {} bytes (voice={})",
           res.body.size(), voice_id);
}

// ---------------------------------------------------------------------------
// Cancel
// ---------------------------------------------------------------------------

void ElevenLabsClient::Cancel() {
  cancel_requested_.store(true);
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

std::string ElevenLabsClient::QueryString() const {
  std::string q = "?output_format=";
  q += TtsOutputFormatString(config_.output_format);
  if (config_.optimize_streaming_latency > 0) {
    q += "&optimize_streaming_latency=";
    q += std::to_string(config_.optimize_streaming_latency);
  }
  if (!config_.enable_logging) {
    q += "&enable_logging=false";
  }
  return q;
}

std::string ElevenLabsClient::BuildBody(const TtsRequest& request) const {
  nlohmann::json j;
  j["text"]     = request.text;
  j["model_id"] = request.model_id.empty() ? config_.model_id : request.model_id;

  const VoiceSettings& vs = request.voice_settings
                                ? *request.voice_settings
                                : VoiceSettings{};
  j["voice_settings"] = {
      {"stability",         vs.stability},
      {"similarity_boost",  vs.similarity_boost},
      {"style",             vs.style},
      {"use_speaker_boost", vs.use_speaker_boost},
  };

  if (!request.language_code.empty()) {
    j["language_code"] = request.language_code;
  }
  if (request.seed.has_value()) {
    j["seed"] = *request.seed;
  }
  if (!request.previous_text.empty()) {
    j["previous_text"] = request.previous_text;
  }
  if (!request.next_text.empty()) {
    j["next_text"] = request.next_text;
  }
  if (!request.previous_request_ids.empty()) {
    j["previous_request_ids"] = request.previous_request_ids;
  }
  if (!request.next_request_ids.empty()) {
    j["next_request_ids"] = request.next_request_ids;
  }
  j["apply_text_normalization"] =
      TextNormalizationString(request.apply_text_normalization);
  if (request.apply_language_text_normalization) {
    j["apply_language_text_normalization"] = true;
  }

  return j.dump();
}

const std::string& ElevenLabsClient::ResolveVoiceId(
    const TtsRequest& request) const {
  return request.voice_id.empty() ? config_.voice_id : request.voice_id;
}

}  // namespace shizuru::services
