// Voice pipeline example: Microphone → VAD filter → ASR → TTS → Speaker
//
// Pipeline topology (all DMA paths, pure bus routing):
//
//   [AudioCaptureDevice] audio_out ──► [dump_capture] pass_in
//   [dump_capture]       pass_out  ──► [EnergyVadDevice] audio_in
//   [EnergyVadDevice]    audio_out ──► [dump_vad] pass_in
//   [EnergyVadDevice]    vad_out   ──► [log_vad] pass_in
//                                          └──► [asr_flush] vad_in
//   [dump_vad]           pass_out  ──► [BaiduAsrDevice] audio_in
//   [BaiduAsrDevice]     text_out  ──► [log_asr_tts] pass_in
//   [log_asr_tts]        pass_out  ──► [BaiduTtsDevice] text_in
//   [BaiduTtsDevice]     audio_out ──► [log_tts_play] pass_in
//   [log_tts_play]       pass_out  ──► [AudioPlayoutDevice] audio_in
//
// VAD behaviour:
//   - EnergyVadDevice is a self-contained VAD filter: it detects speech
//     internally and only forwards confirmed speech frames on audio_out.
//   - Pre-roll buffering ensures onset frames are not lost.
//   - vad_out emits speech_start/speech_active/speech_end for observability.
//   - VadEventDevice triggers asr.Flush() on speech_end.
//
// Usage:
//   export BAIDU_API_KEY=...
//   export BAIDU_SECRET_KEY=...
//   ./voice_pipeline           # INFO level
//   ./voice_pipeline --debug   # DEBUG level (logs every audio frame)
//
// Speak naturally — VAD handles segmentation. Ctrl+C to quit.

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
#include "io/probe/pcm_dump_device.h"
#include "audio/audio_capture_device.h"
#include "audio/audio_playout_device.h"
#include "audio_device/port_audio/pa_player.h"
#include "audio_device/port_audio/pa_recorder.h"
#include "io/asr/baidu/baidu_asr_device.h"
#include "io/tts/baidu/baidu_tts_device.h"
#include "io/vad/energy_vad_device.h"
#include "io/vad/vad_event_device.h"
#include "utils/baidu/baidu_config.h"
#include "runtime/route_table.h"

using namespace shizuru;

// ---------------------------------------------------------------------------
// SimpleBus: wires OutputCallback → RouteTable → OnInput (all DMA)
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

  void AddRoute(const runtime::PortAddress& src, runtime::PortAddress dst) {
    table_.AddRoute(src, std::move(dst), {.requires_control_plane = false});
  }

  void Start() { for (auto& [id, dev] : devices_) { dev->Start(); } }
  void Stop()  { for (auto& [id, dev] : devices_) { dev->Stop();  } }

 private:
  void Dispatch(const std::string& src_id, const std::string& src_port,
                io::DataFrame frame) {  // NOLINT(performance-unnecessary-value-param): fan-out copy
    for (const auto& [dst, _] : table_.Lookup({src_id, src_port})) {
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
  if (ak == nullptr || sk == nullptr) {
    std::fprintf(stderr,
                 "Error: set BAIDU_API_KEY and BAIDU_SECRET_KEY env vars.\n");
    return 1;
  }

  services::BaiduConfig cfg;
  cfg.api_key    = ak;
  cfg.secret_key = sk;
  cfg.aue        = 5;      // PCM 16kHz output from TTS
  cfg.per        = 0;      // female voice
  cfg.asr_format = "pcm";

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
  auto token_mgr = std::make_shared<services::BaiduTokenManager>(cfg);
  token_mgr->GetToken();  // pre-warm

  io::AudioCaptureDevice capture(std::make_unique<io::PaRecorder>(rec_cfg));
  io::AudioPlayoutDevice playout(std::make_unique<io::PaPlayer>(play_cfg));

  // VAD filter: detects speech and gates audio internally.
  // pre_roll_frames ensures onset frames are not lost.
  io::EnergyVadConfig vad_cfg;
  vad_cfg.energy_threshold        = 400.0F;
  vad_cfg.speech_onset_frames     = 3;   // ~60ms
  vad_cfg.silence_hangover_frames = 20;  // ~400ms
  vad_cfg.pre_roll_frames         = 3;   // replay onset frames on speech_start
  io::EnergyVadDevice vad(vad_cfg);

  io::BaiduAsrDevice asr(cfg, token_mgr);
  io::BaiduTtsDevice tts(cfg, token_mgr);

  // VadEventDevice: forwards vad events as DataFrames on vad_out.
  // speech_end → asr.Flush() is handled by routing vad_out → asr control_in.
  io::VadEventDevice asr_flush;

  // LogDevices for observability.
  io::LogDevice log_vad("log_vad",      spdlog::level::info);
  io::LogDevice log_asr("log_asr_tts",  spdlog::level::info);
  io::LogDevice log_tts("log_tts_play", spdlog::level::info);

  // PcmDumpDevices: raw s16le to disk for offline inspection.
  //   capture_dump.pcm — everything from the microphone (pre-VAD)
  //   vad_dump.pcm     — speech-only frames forwarded to ASR
  io::PcmDumpDevice dump_capture("capture_dump");
  io::PcmDumpDevice dump_vad("vad_dump");

  // ── Bus wiring ────────────────────────────────────────────────────────────
  SimpleBus bus;
  bus.Register(&capture);
  bus.Register(&dump_capture);
  bus.Register(&vad);
  bus.Register(&asr_flush);
  bus.Register(&log_vad);
  bus.Register(&dump_vad);
  bus.Register(&asr);
  bus.Register(&log_asr);
  bus.Register(&tts);
  bus.Register(&log_tts);
  bus.Register(&playout);

  // capture → dump_capture (raw mic) → vad filter
  bus.AddRoute({"audio_capture", "audio_out"},
               {"capture_dump",  io::PcmDumpDevice::kPassIn});
  bus.AddRoute({"capture_dump",  io::PcmDumpDevice::kPassOut},
               {"vad",           io::EnergyVadDevice::kAudioIn});

  // vad audio_out (speech only, with pre-roll) → dump_vad → asr
  bus.AddRoute({"vad",      io::EnergyVadDevice::kAudioOut},
               {"vad_dump", io::PcmDumpDevice::kPassIn});
  bus.AddRoute({"vad_dump", io::PcmDumpDevice::kPassOut},
               {"baidu_asr", "audio_in"});

  // vad vad_out (events) → log → asr_flush (triggers Flush on speech_end)
  bus.AddRoute({"vad",     io::EnergyVadDevice::kVadOut},
               {"log_vad", io::LogDevice::kPassIn});
  bus.AddRoute({"log_vad", io::LogDevice::kPassOut},
               {"vad_event", io::VadEventDevice::kVadIn});

  // asr → [log] → tts
  bus.AddRoute({"baidu_asr",   "text_out"},
               {"log_asr_tts", io::LogDevice::kPassIn});
  bus.AddRoute({"log_asr_tts", io::LogDevice::kPassOut},
               {"baidu_tts",   "text_in"});

  // tts → [log] → playout
  bus.AddRoute({"baidu_tts",    "audio_out"},
               {"log_tts_play", io::LogDevice::kPassIn});
  bus.AddRoute({"log_tts_play", io::LogDevice::kPassOut},
               {"audio_playout", "audio_in"});

  bus.Start();

  std::printf("=== Voice Pipeline (VAD filter + Baidu ASR + TTS) ===\n");
  std::printf("Log level : %s\n",
              debug_mode ? "debug (all frames)" : "info (events + text)");
  std::printf("Speak naturally — VAD detects speech automatically.\n");
  std::printf("Ctrl+C to quit.\n\n");

  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  bus.Stop();
  return 0;
}
