// Voice pipeline example: Microphone → Baidu ASR → Baidu TTS → Speaker
//
// Pipeline with LogDevices inserted at each hop:
//
//   [AudioCaptureDevice]
//          | audio/pcm
//   [log_cap_asr]          ← DEBUG: logs every captured audio frame
//          | audio/pcm
//   [BaiduAsrDevice]
//          | text/plain
//   [log_asr_tts]          ← INFO: logs the transcript
//          | text/plain
//   [BaiduTtsDevice]
//          | audio/pcm
//   [log_tts_play]         ← INFO: logs TTS audio size
//          | audio/pcm
//   [AudioPlayoutDevice]
//
// Usage:
//   export BAIDU_API_KEY=...
//   export BAIDU_SECRET_KEY=...
//   ./voice_pipeline           # INFO level (transcript + TTS size)
//   ./voice_pipeline --debug   # DEBUG level (all frames including audio)
//
// Press Enter to stop recording and trigger ASR → TTS → playout.
// Press Ctrl+C to quit.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include <spdlog/spdlog.h>
#include "async_logger.h"
#include "io/io_device.h"
#include "io/data_frame.h"
#include "io/probe/log_device.h"
#include "audio/audio_capture_device.h"
#include "audio/audio_playout_device.h"
#include "audio_device/port_audio/pa_player.h"
#include "audio_device/port_audio/pa_recorder.h"
#include "io/asr/baidu/baidu_asr_device.h"
#include "io/tts/baidu/baidu_tts_device.h"
#include "utils/baidu/baidu_config.h"
#include "runtime/route_table.h"

using namespace shizuru;

// ---------------------------------------------------------------------------
// Minimal device bus: wires OutputCallback → RouteTable → OnInput (DMA)
// ---------------------------------------------------------------------------
class SimpleBus {
 public:
  void Register(io::IoDevice* dev) {
    devices_[dev->GetDeviceId()] = dev;
    dev->SetOutputCallback([this](const std::string& src_id,
                                   const std::string& src_port,
                                   io::DataFrame frame) {
      Dispatch(src_id, src_port, std::move(frame));
    });
  }

  void AddRoute(runtime::PortAddress src, runtime::PortAddress dst) {
    table_.AddRoute(src, std::move(dst), {.requires_control_plane = false});
  }

  void Start() { for (auto& [id, dev] : devices_) { dev->Start(); } }
  void Stop()  { for (auto& [id, dev] : devices_) { dev->Stop();  } }

 private:
  void Dispatch(const std::string& src_id, const std::string& src_port,
                io::DataFrame frame) {
    for (auto& [dst, _] : table_.Lookup({src_id, src_port})) {
      auto it = devices_.find(dst.device_id);
      if (it != devices_.end()) { it->second->OnInput(dst.port_name, frame); }
    }
  }

  runtime::RouteTable table_;
  std::unordered_map<std::string, io::IoDevice*> devices_;
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
  bool debug_mode = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--debug") { debug_mode = true; }
  }

  shizuru::core::LoggerConfig log_cfg;
  log_cfg.level = debug_mode ? spdlog::level::debug : spdlog::level::info;
  shizuru::core::InitLogger(log_cfg);

  const char* ak = std::getenv("BAIDU_API_KEY");
  const char* sk = std::getenv("BAIDU_SECRET_KEY");
  if (!ak || !sk) {
    std::fprintf(stderr,
                 "Error: set BAIDU_API_KEY and BAIDU_SECRET_KEY env vars.\n");
    return 1;
  }

  services::BaiduConfig cfg;
  cfg.api_key    = ak;
  cfg.secret_key = sk;
  cfg.aue        = 5;      // PCM 16kHz output from TTS
  cfg.per        = 0;      // female voice
  cfg.asr_format = "pcm";  // ASR expects raw PCM

  // ── Audio config ──────────────────────────────────────────────────────────
  constexpr int    kRate = 16000;
  constexpr size_t kCh   = 1;
  constexpr size_t kFpb  = 320;  // 20ms at 16kHz

  io::RecorderConfig rec_cfg;
  rec_cfg.sample_rate             = kRate;
  rec_cfg.channel_count           = kCh;
  rec_cfg.frames_per_buffer       = kFpb;
  rec_cfg.buffer_capacity_samples = static_cast<size_t>(kRate) * 5;

  io::PlayerConfig play_cfg;
  play_cfg.sample_rate             = kRate;
  play_cfg.channel_count           = kCh;
  play_cfg.frames_per_buffer       = kFpb;
  play_cfg.buffer_capacity_samples = static_cast<size_t>(kRate) * 10;

  // ── Devices ───────────────────────────────────────────────────────────────
  // Share one BaiduTokenManager so both ASR and TTS reuse the same cached
  // token — avoids a redundant ~300ms refresh on every turn.
  auto token_mgr = std::make_shared<services::BaiduTokenManager>(cfg);
  token_mgr->GetToken();  // pre-warm: fetch token once at startup

  io::AudioCaptureDevice   capture(std::make_unique<io::PaRecorder>(rec_cfg));
  io::BaiduAsrDevice       asr(cfg, token_mgr);
  io::BaiduTtsDevice       tts(cfg, token_mgr);
  io::AudioPlayoutDevice   playout(std::make_unique<io::PaPlayer>(play_cfg));

  // LogDevices: inserted between each hop to observe data flow.
  // audio frames → DEBUG (high-frequency), text frames → INFO (low-frequency)
  io::LogDevice log_cap_asr("log_cap_asr",  spdlog::level::debug);
  io::LogDevice log_asr_tts("log_asr_tts",  spdlog::level::info);
  io::LogDevice log_tts_play("log_tts_play", spdlog::level::info);

  // ── Bus wiring ────────────────────────────────────────────────────────────
  SimpleBus bus;
  bus.Register(&capture);
  bus.Register(&log_cap_asr);
  bus.Register(&asr);
  bus.Register(&log_asr_tts);
  bus.Register(&tts);
  bus.Register(&log_tts_play);
  bus.Register(&playout);

  // capture → [log] → asr
  bus.AddRoute({"audio_capture", "audio_out"}, {"log_cap_asr",  io::LogDevice::kPassIn});
  bus.AddRoute({"log_cap_asr",   io::LogDevice::kPassOut}, {"baidu_asr", "audio_in"});
  // asr → [log] → tts
  bus.AddRoute({"baidu_asr",     "text_out"},  {"log_asr_tts",  io::LogDevice::kPassIn});
  bus.AddRoute({"log_asr_tts",   io::LogDevice::kPassOut}, {"baidu_tts", "text_in"});
  // tts → [log] → playout
  bus.AddRoute({"baidu_tts",     "audio_out"}, {"log_tts_play", io::LogDevice::kPassIn});
  bus.AddRoute({"log_tts_play",  io::LogDevice::kPassOut}, {"audio_playout", "audio_in"});

  bus.Start();

  std::printf("=== Voice Pipeline (Baidu ASR + TTS) ===\n");
  std::printf("Log level: %s\n", debug_mode ? "debug (audio + text frames)" : "info (text frames only)");
  std::printf("Speak into the microphone, then press Enter to transcribe.\n");
  std::printf("Ctrl+C to quit.\n\n");

  while (true) {
    std::printf("[Recording... press Enter to stop]\n");
    std::getchar();

    std::printf("[Transcribing...]\n");
    asr.Flush();  // ASR → text → TTS → audio → playout

    // Wait for TTS to finish synthesizing, then let playout drain
    tts.WaitDone(std::chrono::milliseconds(10000));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::printf("[Done. Press Enter to record again]\n\n");
  }

  bus.Stop();
  return 0;
}
