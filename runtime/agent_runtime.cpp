#include "agent_runtime.h"

#include <chrono>
#include <utility>

#include <nlohmann/json.hpp>

#include "io/tool_dispatcher.h"
#include "llm/openai_client.h"
#include "memory/in_memory_store.h"
#include "audit/log_audit_sink.h"

namespace shizuru::runtime {

namespace {

std::string GenerateSessionId() {
  auto now = std::chrono::steady_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch())
                .count();
  return "session_" + std::to_string(ms);
}

bool IsAudioMimeType(const std::string& mime_type) {
  return mime_type.size() >= 6 && mime_type.rfind("audio/", 0) == 0;
}

std::string ExtractTextFromToolOutput(const std::string& output) {
  try {
    nlohmann::json j = nlohmann::json::parse(output);
    if (j.is_object()) {
      if (j.contains("text") && j["text"].is_string()) {
        return j["text"].get<std::string>();
      }
      if (j.contains("transcript") && j["transcript"].is_string()) {
        return j["transcript"].get<std::string>();
      }
    }
    if (j.is_string()) {
      return j.get<std::string>();
    }
  } catch (...) {
    // Tool output is not JSON, treat as plain text.
  }

  return output;
}

}  // namespace

AgentRuntime::AgentRuntime(RuntimeConfig config,
                           services::ToolRegistry& tools)
    : config_(std::move(config)), tools_(tools) {
  core::InitLogger(config_.logger);
}

AgentRuntime::~AgentRuntime() {
  if (session_) {
    Shutdown();
  }
}

std::string AgentRuntime::StartSession() {
  if (session_) {
    Shutdown();
  }

  std::string session_id = GenerateSessionId();

  auto llm = std::make_unique<services::OpenAiClient>(config_.llm);
  auto io = std::make_unique<services::ToolDispatcher>(tools_);
  auto memory = std::make_unique<services::InMemoryStore>();
  auto audit = std::make_unique<services::LogAuditSink>();

  session_ = std::make_unique<core::AgentSession>(
      session_id, config_.controller, config_.context, config_.policy,
      std::move(llm), std::move(io), std::move(memory), std::move(audit));

  // Observe assistant text responses and optionally synthesize audio.
  session_->GetController().OnResponse(
      [this](const core::ActionCandidate& response) {
        HandleAssistantResponse(response);
      });

  session_->Start();
  return session_id;
}

void AgentRuntime::SendMessage(const std::string& content) {
  SendInput(content, "text/plain");
}

void AgentRuntime::SendInput(const std::string& payload,
                             const std::string& mime_type) {
  if (!session_) {
    return;
  }

  LOG_DEBUG("[{}] SendInput mime_type={} payload_len={}",
            MODULE_NAME, mime_type, payload.size());

  if (IsAudioMimeType(mime_type)) {
    last_input_was_audio_.store(true);

    if (!PassesVadGate(payload, mime_type)) {
      RuntimeOutput output;
      output.text = "Audio received, but VAD detected no speech.";

      OutputCallback cb;
      {
        std::lock_guard<std::mutex> lock(output_cb_mutex_);
        cb = output_cb_;
      }
      if (cb) {
        cb(output);
      }
      return;
    }

    const std::string transcript = TranscribeAudioToText(payload, mime_type);
    if (transcript.empty()) {
      RuntimeOutput output;
      output.text = "Audio received, but transcription failed or STT tool is unavailable.";

      OutputCallback cb;
      {
        std::lock_guard<std::mutex> lock(output_cb_mutex_);
        cb = output_cb_;
      }
      if (cb) {
        cb(output);
      }
      return;
    }

    EnqueueUserText(transcript);
    return;
  }

  last_input_was_audio_.store(false);
  EnqueueUserText(payload);
}

void AgentRuntime::OnOutput(OutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  output_cb_ = std::move(cb);
}

void AgentRuntime::EnqueueUserText(const std::string& content) {
  if (!session_) {
    return;
  }

  LOG_INFO("[{}] Enqueue user text: \"{}\"", MODULE_NAME, content);
  core::Observation obs;
  obs.type = core::ObservationType::kUserMessage;
  obs.content = content;
  obs.source = "user";
  obs.timestamp = std::chrono::steady_clock::now();
  session_->EnqueueObservation(std::move(obs));
}

bool AgentRuntime::PassesVadGate(const std::string& payload,
                                 const std::string& mime_type) {
  const services::ToolFunction* vad_fn = tools_.Find("vad");
  if (!vad_fn) {
    // TODO(voice-tools): register a concrete "vad" tool in ToolRegistry.
    // Default pass-through keeps current behavior backward-compatible.
    return true;
  }

  nlohmann::json args;
  args["audio"] = payload;
  args["mime_type"] = mime_type;

  core::ActionResult result;
  try {
    result = (*vad_fn)(args.dump());
  } catch (...) {
    return false;
  }

  if (!result.success || result.output.empty()) {
    return false;
  }

  try {
    nlohmann::json j = nlohmann::json::parse(result.output);
    if (j.is_boolean()) {
      return j.get<bool>();
    }
    if (j.is_object()) {
      if (j.contains("is_speech") && j["is_speech"].is_boolean()) {
        return j["is_speech"].get<bool>();
      }
      if (j.contains("speech") && j["speech"].is_boolean()) {
        return j["speech"].get<bool>();
      }
      if (j.contains("score") && j["score"].is_number()) {
        return j["score"].get<double>() >= 0.5;
      }
    }
    if (j.is_string()) {
      const std::string s = j.get<std::string>();
      return s == "true" || s == "speech" || s == "1";
    }
  } catch (...) {
    // Non-JSON output: use conservative parsing below.
  }

  return result.output == "true" || result.output == "speech" ||
         result.output == "1";
}

std::string AgentRuntime::TranscribeAudioToText(const std::string& payload,
                                                const std::string& mime_type) {
  const services::ToolFunction* stt_fn = tools_.Find("stt");
  if (!stt_fn) {
    stt_fn = tools_.Find("asr");
  }
  if (!stt_fn) {
    // TODO(voice-tools): register "stt" or "asr" tool implementation.
    return "";
  }

  nlohmann::json args;
  args["audio"] = payload;
  args["mime_type"] = mime_type;

  core::ActionResult result;
  try {
    result = (*stt_fn)(args.dump());
  } catch (...) {
    return "";
  }

  if (!result.success || result.output.empty()) {
    return "";
  }

  return ExtractTextFromToolOutput(result.output);
}

bool AgentRuntime::ShouldSynthesizeAudio(
    const core::ActionCandidate& response) const {
  if (!config_.auto_tts_on_audio_input) {
    return false;
  }
  if (!last_input_was_audio_.load()) {
    return false;
  }
  if (response.type != core::ActionType::kResponse) {
    return false;
  }
  if (response.response_text.empty()) {
    return false;
  }
  return tools_.Has("tts") || tools_.Has("text_to_speech");
}

RuntimeOutput AgentRuntime::MaybeSynthesizeAudio(
    const RuntimeOutput& text_output,
    const core::ActionCandidate& response) {
  RuntimeOutput output = text_output;

  const services::ToolFunction* tts_fn = tools_.Find("tts");
  if (!tts_fn) {
    tts_fn = tools_.Find("text_to_speech");
  }
  if (!tts_fn) {
    // TODO(voice-tools): register "tts" or "text_to_speech" tool implementation.
    return output;
  }

  nlohmann::json args;
  args["text"] = response.response_text;

  core::ActionResult result;
  try {
    result = (*tts_fn)(args.dump());
  } catch (...) {
    return output;
  }

  if (!result.success || result.output.empty()) {
    return output;
  }

  // Accept both plain payload and JSON shape {audio, mime_type}.
  try {
    nlohmann::json j = nlohmann::json::parse(result.output);
    if (j.is_object()) {
      if (j.contains("audio") && j["audio"].is_string()) {
        output.audio_payload = j["audio"].get<std::string>();
      }
      if (j.contains("mime_type") && j["mime_type"].is_string()) {
        output.audio_mime_type = j["mime_type"].get<std::string>();
      }
      output.has_audio = !output.audio_payload.empty();
      return output;
    }
  } catch (...) {
    // Not JSON, treat as raw audio payload.
  }

  output.audio_payload = result.output;
  output.has_audio = true;
  return output;
}

void AgentRuntime::HandleAssistantResponse(const core::ActionCandidate& response) {
  if (response.type != core::ActionType::kResponse) {
    return;
  }

  LOG_INFO("[{}] Assistant response: \"{}\"", MODULE_NAME, response.response_text);

  RuntimeOutput output;
  output.text = response.response_text;

  if (ShouldSynthesizeAudio(response)) {
    output = MaybeSynthesizeAudio(output, response);
  }

  OutputCallback cb;
  {
    std::lock_guard<std::mutex> lock(output_cb_mutex_);
    cb = output_cb_;
  }
  if (cb) {
    cb(output);
  }
}

void AgentRuntime::Shutdown() {
  if (session_) {
    session_->Shutdown();
    session_.reset();
  }
  last_input_was_audio_.store(false);
  core::ShutdownLogger();
}

core::State AgentRuntime::GetState() const {
  if (!session_) {
    return core::State::kTerminated;
  }
  return session_->GetState();
}

bool AgentRuntime::HasActiveSession() const {
  return session_ != nullptr;
}

}  // namespace shizuru::runtime
