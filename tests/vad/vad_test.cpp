// Unit tests for VAD IoDevice implementations:
//   - EnergyVadDevice  (VAD filter: state machine + audio gating + pre-roll)
//   - VadEventDevice   (VAD signal adapter)

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "io/control_frame.h"
#include "io/interrupt_frame.h"
#include "io/vad/energy_vad_device.h"
#include "io/vad/vad_event_device.h"

namespace shizuru::io {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

DataFrame MakeAudioFrame(int16_t sample_value, size_t num_samples = 320) {
  DataFrame frame;
  frame.type = "audio/pcm";
  frame.payload.resize(num_samples * sizeof(int16_t));
  auto* samples = reinterpret_cast<int16_t*>(frame.payload.data());
  for (size_t i = 0; i < num_samples; ++i) { samples[i] = sample_value; }
  frame.source_device = "test";
  frame.source_port   = "audio_out";
  return frame;
}

DataFrame MakeVadEvent(const std::string& event) {
  DataFrame frame;
  frame.type = "vad/event";
  frame.payload.assign(event.begin(), event.end());
  frame.source_device = "vad";
  frame.source_port   = "vad_out";
  return frame;
}

std::string ExtractEvent(const DataFrame& frame) {
  const std::string_view json(
      reinterpret_cast<const char*>(frame.payload.data()),
      frame.payload.size());
  for (const auto* ev : {"speech_start", "speech_active", "speech_end"}) {
    if (json.find(ev) != std::string_view::npos) { return ev; }
  }
  return "";
}

// Feed frames into vad, collect emitted vad/event strings.
std::vector<std::string> CollectEvents(EnergyVadDevice& vad,
                                       const std::vector<DataFrame>& frames) {
  std::vector<std::string> events;
  vad.SetOutputCallback([&](const std::string&, const std::string& port,
                             DataFrame f) {
    if (port == EnergyVadDevice::kVadOut) {
      events.push_back(ExtractEvent(f));
    }
  });
  vad.Start();
  for (const auto& f : frames) { vad.OnInput(EnergyVadDevice::kAudioIn, f); }
  return events;
}

// Feed frames into vad, collect forwarded audio frames on audio_out.
std::vector<DataFrame> CollectAudio(EnergyVadDevice& vad,
                                    const std::vector<DataFrame>& frames) {
  std::vector<DataFrame> audio;
  vad.SetOutputCallback([&](const std::string&, const std::string& port,
                             DataFrame f) {
    if (port == EnergyVadDevice::kAudioOut) {
      audio.push_back(std::move(f));
    }
  });
  vad.Start();
  for (const auto& f : frames) { vad.OnInput(EnergyVadDevice::kAudioIn, f); }
  return audio;
}

// ---------------------------------------------------------------------------
// EnergyVadDevice — VAD state machine (window=1 for crisp transitions)
// ---------------------------------------------------------------------------

class EnergyVadTest : public ::testing::Test {
 protected:
  // rms_window_frames=1 preserves single-frame behavior for state-machine tests.
  EnergyVadConfig cfg{
      .energy_threshold        = 1000.0F,
      .rms_window_frames       = 1,
      .speech_onset_frames     = 2,
      .silence_hangover_frames = 3,
      .pre_roll_frames         = 0,  // no pre-roll so audio counts are exact
  };
};

TEST_F(EnergyVadTest, PortDescriptors) {
  EnergyVadDevice vad;
  const auto ports = vad.GetPortDescriptors();
  ASSERT_EQ(ports.size(), 3U);

  bool has_audio_in = false, has_audio_out = false, has_vad_out = false;
  for (const auto& p : ports) {
    if (p.name == EnergyVadDevice::kAudioIn) {
      EXPECT_EQ(p.direction, PortDirection::kInput);
      has_audio_in = true;
    }
    if (p.name == EnergyVadDevice::kAudioOut) {
      EXPECT_EQ(p.direction, PortDirection::kOutput);
      EXPECT_EQ(p.data_type, "audio/pcm");
      has_audio_out = true;
    }
    if (p.name == EnergyVadDevice::kVadOut) {
      EXPECT_EQ(p.direction, PortDirection::kOutput);
      EXPECT_EQ(p.data_type, "vad/event");
      has_vad_out = true;
    }
  }
  EXPECT_TRUE(has_audio_in);
  EXPECT_TRUE(has_audio_out);
  EXPECT_TRUE(has_vad_out);
}

TEST_F(EnergyVadTest, NoEventsBeforeStart) {
  EnergyVadDevice vad(cfg);
  int call_count = 0;
  vad.SetOutputCallback([&](auto, auto, auto) { ++call_count; });
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(5000));
  EXPECT_EQ(call_count, 0);
}

TEST_F(EnergyVadTest, SilenceProducesNoOutput) {
  EnergyVadDevice vad(cfg);
  const auto events = CollectEvents(vad, {
      MakeAudioFrame(0), MakeAudioFrame(0), MakeAudioFrame(0),
  });
  EXPECT_TRUE(events.empty());
}

TEST_F(EnergyVadTest, SilenceProducesNoAudio) {
  EnergyVadDevice vad(cfg);
  const auto audio = CollectAudio(vad, {
      MakeAudioFrame(0), MakeAudioFrame(0), MakeAudioFrame(0),
  });
  EXPECT_TRUE(audio.empty());
}

TEST_F(EnergyVadTest, SpeechStartAfterOnsetFrames) {
  EnergyVadDevice vad(cfg);
  const auto events = CollectEvents(vad, {
      MakeAudioFrame(5000),  // onset 1
      MakeAudioFrame(5000),  // onset 2 → speech_start
  });
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0], "speech_start");
}

TEST_F(EnergyVadTest, OnsetCounterResetsOnSilence) {
  EnergyVadDevice vad(cfg);
  const auto events = CollectEvents(vad, {
      MakeAudioFrame(5000),  // onset 1
      MakeAudioFrame(0),     // silence → reset
      MakeAudioFrame(5000),  // onset 1 again
  });
  EXPECT_TRUE(events.empty());
}

TEST_F(EnergyVadTest, SpeechActiveEmittedDuringSpeech) {
  EnergyVadDevice vad(cfg);
  const auto events = CollectEvents(vad, {
      MakeAudioFrame(5000),  // onset 1
      MakeAudioFrame(5000),  // onset 2 → speech_start
      MakeAudioFrame(5000),  // speech_active
      MakeAudioFrame(5000),  // speech_active
  });
  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[0], "speech_start");
  EXPECT_EQ(events[1], "speech_active");
  EXPECT_EQ(events[2], "speech_active");
}

TEST_F(EnergyVadTest, SpeechEndAfterHangoverFrames) {
  EnergyVadDevice vad(cfg);
  const auto events = CollectEvents(vad, {
      MakeAudioFrame(5000), MakeAudioFrame(5000),  // speech_start
      MakeAudioFrame(0), MakeAudioFrame(0), MakeAudioFrame(0),  // speech_end
  });
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0], "speech_start");
  EXPECT_EQ(events[1], "speech_end");
}

TEST_F(EnergyVadTest, HangoverCounterResetsOnLoudFrame) {
  EnergyVadDevice vad(cfg);
  const auto events = CollectEvents(vad, {
      MakeAudioFrame(5000), MakeAudioFrame(5000),  // speech_start
      MakeAudioFrame(0), MakeAudioFrame(0),         // hangover 1,2
      MakeAudioFrame(5000),                         // loud → reset, speech_active
      MakeAudioFrame(0), MakeAudioFrame(0),         // hangover 1,2 (no speech_end yet)
  });
  EXPECT_EQ(events[0], "speech_start");
  EXPECT_EQ(events[1], "speech_active");
  EXPECT_EQ(events.size(), 2U);
}

TEST_F(EnergyVadTest, FullCycle) {
  EnergyVadDevice vad(cfg);
  const auto events = CollectEvents(vad, {
      MakeAudioFrame(5000), MakeAudioFrame(5000),  // speech_start
      MakeAudioFrame(5000),                         // speech_active
      MakeAudioFrame(0), MakeAudioFrame(0), MakeAudioFrame(0),  // speech_end
  });
  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[0], "speech_start");
  EXPECT_EQ(events[1], "speech_active");
  EXPECT_EQ(events[2], "speech_end");
}

TEST_F(EnergyVadTest, MultipleSpeechSegments) {
  EnergyVadDevice vad(cfg);
  const auto events = CollectEvents(vad, {
      MakeAudioFrame(5000), MakeAudioFrame(5000),
      MakeAudioFrame(0), MakeAudioFrame(0), MakeAudioFrame(0),
      MakeAudioFrame(5000), MakeAudioFrame(5000),
      MakeAudioFrame(0), MakeAudioFrame(0), MakeAudioFrame(0),
  });
  ASSERT_EQ(events.size(), 4U);
  EXPECT_EQ(events[0], "speech_start");
  EXPECT_EQ(events[1], "speech_end");
  EXPECT_EQ(events[2], "speech_start");
  EXPECT_EQ(events[3], "speech_end");
}

TEST_F(EnergyVadTest, StartResetsState) {
  EnergyVadDevice vad(cfg);
  std::vector<std::string> events;
  vad.SetOutputCallback([&](auto, auto, DataFrame f) {
    if (f.type == "vad/event") { events.push_back(ExtractEvent(f)); }
  });

  vad.Start();
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(5000));
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(5000));  // speech_start
  vad.Stop();

  events.clear();
  vad.Start();
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(0));
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(0));
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(0));
  EXPECT_TRUE(events.empty());
}

TEST_F(EnergyVadTest, UnknownPortIgnored) {
  EnergyVadDevice vad(cfg);
  int call_count = 0;
  vad.SetOutputCallback([&](auto, auto, auto) { ++call_count; });
  vad.Start();
  vad.OnInput("wrong_port", MakeAudioFrame(5000));
  EXPECT_EQ(call_count, 0);
}

TEST_F(EnergyVadTest, EmptyPayloadIgnored) {
  EnergyVadDevice vad(cfg);
  int call_count = 0;
  vad.SetOutputCallback([&](auto, auto, auto) { ++call_count; });
  vad.Start();
  DataFrame empty;
  empty.type = "audio/pcm";
  vad.OnInput(EnergyVadDevice::kAudioIn, empty);
  EXPECT_EQ(call_count, 0);
}

// ---------------------------------------------------------------------------
// EnergyVadDevice — audio gating (audio_out only during speech)
// ---------------------------------------------------------------------------

TEST_F(EnergyVadTest, AudioNotForwardedBeforeSpeechStart) {
  EnergyVadDevice vad(cfg);
  // onset=2: first loud frame does not yet trigger speech_start
  const auto audio = CollectAudio(vad, {
      MakeAudioFrame(5000),  // onset 1 — not yet confirmed
  });
  EXPECT_TRUE(audio.empty());
}

TEST_F(EnergyVadTest, AudioForwardedAfterSpeechStart) {
  EnergyVadDevice vad(cfg);
  const auto audio = CollectAudio(vad, {
      MakeAudioFrame(5000),  // onset 1
      MakeAudioFrame(5000),  // onset 2 → speech_start, this frame forwarded
      MakeAudioFrame(5000),  // speech_active, forwarded
  });
  EXPECT_EQ(audio.size(), 2U);
}

TEST_F(EnergyVadTest, AudioForwardedDuringHangover) {
  EnergyVadDevice vad(cfg);
  // hangover=3: silent frames during hangover are still forwarded.
  // onset=2, pre_roll=0: frame 1 not emitted, frame 2 → speech_start (1 emitted)
  // then 3 hangover frames emitted = 4 total
  const auto audio = CollectAudio(vad, {
      MakeAudioFrame(5000), MakeAudioFrame(5000),  // speech_start (frame 2 emitted)
      MakeAudioFrame(0),                            // hangover 1 — forwarded
      MakeAudioFrame(0),                            // hangover 2 — forwarded
      MakeAudioFrame(0),                            // hangover 3 → speech_end, forwarded
  });
  EXPECT_EQ(audio.size(), 4U);
}

TEST_F(EnergyVadTest, AudioNotForwardedAfterSpeechEnd) {
  EnergyVadDevice vad(cfg);
  const auto audio = CollectAudio(vad, {
      MakeAudioFrame(5000), MakeAudioFrame(5000),  // speech_start (frame 2 emitted)
      MakeAudioFrame(0), MakeAudioFrame(0), MakeAudioFrame(0),  // 3 hangover → speech_end
      MakeAudioFrame(0),  // after speech_end — dropped
      MakeAudioFrame(0),  // dropped
  });
  // 1 speech_start frame + 3 hangover = 4 forwarded; 2 post-end dropped
  EXPECT_EQ(audio.size(), 4U);
}

TEST_F(EnergyVadTest, AudioSourceFieldsRewritten) {
  EnergyVadDevice vad(cfg);
  const auto audio = CollectAudio(vad, {
      MakeAudioFrame(5000), MakeAudioFrame(5000),
  });
  ASSERT_EQ(audio.size(), 1U);
  EXPECT_EQ(audio[0].source_device, vad.GetDeviceId());
  EXPECT_EQ(audio[0].source_port,   EnergyVadDevice::kAudioOut);
}

// ---------------------------------------------------------------------------
// EnergyVadDevice — pre-roll buffer
// ---------------------------------------------------------------------------

TEST(EnergyVadPreRollTest, PreRollFramesFlushedOnSpeechStart) {
  // pre_roll=2, onset=2: the 2 onset frames are buffered and replayed
  // when speech_start fires, so audio_out gets them before the trigger frame.
  EnergyVadConfig cfg{
      .energy_threshold        = 1000.0F,
      .rms_window_frames       = 1,
      .speech_onset_frames     = 2,
      .silence_hangover_frames = 3,
      .pre_roll_frames         = 2,
  };
  EnergyVadDevice vad(cfg);
  const auto audio = CollectAudio(vad, {
      MakeAudioFrame(5000),  // onset 1 — buffered in pre-roll
      MakeAudioFrame(5000),  // onset 2 → speech_start: flush pre-roll (1 frame)
                             //           then emit this frame → 2 total
  });
  // pre-roll flushes frame 1, then frame 2 is emitted directly
  EXPECT_EQ(audio.size(), 2U);
}

TEST(EnergyVadPreRollTest, PreRollClearedOnStop) {
  EnergyVadConfig cfg{
      .energy_threshold        = 1000.0F,
      .rms_window_frames       = 1,
      .speech_onset_frames     = 2,
      .silence_hangover_frames = 3,
      .pre_roll_frames         = 3,
  };
  EnergyVadDevice vad(cfg);
  std::vector<DataFrame> audio;
  vad.SetOutputCallback([&](auto, const std::string& port, DataFrame f) {
    if (port == EnergyVadDevice::kAudioOut) { audio.push_back(std::move(f)); }
  });

  // First run: push a loud frame into pre-roll buffer.
  vad.Start();
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(5000));
  vad.Stop();

  // Second run: pre-roll must be cleared — silence should produce nothing.
  audio.clear();
  vad.Start();
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(0));
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(0));
  EXPECT_TRUE(audio.empty());
}

// ---------------------------------------------------------------------------
// EnergyVadDevice — sliding window RMS max-filter
// ---------------------------------------------------------------------------

static EnergyVadConfig WindowCfg(size_t window_frames) {
  return EnergyVadConfig{
      .energy_threshold        = 1000.0F,
      .rms_window_frames       = window_frames,
      .speech_onset_frames     = 1,
      .silence_hangover_frames = 1,
      .pre_roll_frames         = 0,
  };
}

TEST(EnergyVadWindowTest, SingleLoudFrameInWindowKeepsSpeechTrue) {
  EnergyVadDevice vad(WindowCfg(3));
  const auto events = CollectEvents(vad, {
      MakeAudioFrame(5000),  // loud → speech_start
      MakeAudioFrame(0),     // quiet, loud still in window → speech_active
      MakeAudioFrame(0),     // quiet, loud still in window → speech_active
  });
  ASSERT_GE(events.size(), 1U);
  EXPECT_EQ(events[0], "speech_start");
  for (const auto& ev : events) { EXPECT_NE(ev, "speech_end"); }
}

TEST(EnergyVadWindowTest, SpeechEndsAfterWindowFlushes) {
  EnergyVadDevice vad(WindowCfg(3));
  const auto events = CollectEvents(vad, {
      MakeAudioFrame(5000),  // speech_start
      MakeAudioFrame(0),
      MakeAudioFrame(0),
      MakeAudioFrame(0),     // loud evicted → speech_end
  });
  EXPECT_EQ(events.front(), "speech_start");
  EXPECT_EQ(events.back(), "speech_end");
}

TEST(EnergyVadWindowTest, WindowSizeOneMatchesSingleFrameBehavior) {
  EnergyVadDevice vad(WindowCfg(1));
  const auto events = CollectEvents(vad, {
      MakeAudioFrame(5000),  // speech_start
      MakeAudioFrame(0),     // speech_end
  });
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0], "speech_start");
  EXPECT_EQ(events[1], "speech_end");
}

TEST(EnergyVadWindowTest, StartClearsWindow) {
  EnergyVadDevice vad(WindowCfg(3));
  std::vector<std::string> events;
  vad.SetOutputCallback([&](auto, auto, DataFrame f) {
    if (f.type == "vad/event") { events.push_back(ExtractEvent(f)); }
  });

  vad.Start();
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(5000));
  vad.Stop();

  events.clear();
  vad.Start();
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(0));
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(0));
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(0));
  EXPECT_TRUE(events.empty());
}

// ---------------------------------------------------------------------------
// VadEventDevice
// ---------------------------------------------------------------------------

class VadEventTest : public ::testing::Test {};

// Helper: collect payloads emitted on a specific VadEventDevice output port.
static std::vector<std::string> CollectVadOutput(
    VadEventDevice& dev,
    const std::string& output_port,
    const std::vector<std::string>& events) {
  std::vector<std::string> out;
  dev.SetOutputCallback([&](auto, const std::string& port, DataFrame f) {
    if (port != output_port) { return; }
    if (port == VadEventDevice::kControlOut) {
      out.push_back(ControlFrame::Parse(f));
      return;
    }
    if (port == VadEventDevice::kInterruptOut) {
      out.push_back(InterruptFrame::ParseReason(f));
      return;
    }
    out.emplace_back(f.payload.begin(), f.payload.end());
  });
  dev.Start();
  for (const auto& ev : events) {
    dev.OnInput(VadEventDevice::kVadIn, MakeVadEvent(ev));
  }
  return out;
}

TEST_F(VadEventTest, PortDescriptors) {
  VadEventDevice dev;
  const auto ports = dev.GetPortDescriptors();
  ASSERT_EQ(ports.size(), 4U);
  bool has_in = false;
  bool has_vad_out = false;
  bool has_interrupt_out = false;
  bool has_control_out = false;
  for (const auto& p : ports) {
    if (p.name == VadEventDevice::kVadIn) {
      has_in = true;
      EXPECT_EQ(p.direction, PortDirection::kInput);
    }
    if (p.name == VadEventDevice::kVadOut) {
      has_vad_out = true;
      EXPECT_EQ(p.direction, PortDirection::kOutput);
    }
    if (p.name == VadEventDevice::kInterruptOut) {
      has_interrupt_out = true;
      EXPECT_EQ(p.direction, PortDirection::kOutput);
    }
    if (p.name == VadEventDevice::kControlOut) {
      has_control_out = true;
      EXPECT_EQ(p.direction, PortDirection::kOutput);
    }
  }
  EXPECT_TRUE(has_in);
  EXPECT_TRUE(has_vad_out);
  EXPECT_TRUE(has_interrupt_out);
  EXPECT_TRUE(has_control_out);
}

TEST_F(VadEventTest, AllEventsStillPassThroughOnVadOut) {
  VadEventDevice dev;
  auto out = CollectVadOutput(
      dev, VadEventDevice::kVadOut,
      {"speech_start", "speech_active", "speech_end"});
  ASSERT_EQ(out.size(), 3U);
  EXPECT_NE(out[0].find("speech_start"), std::string::npos);
  EXPECT_NE(out[1].find("speech_active"), std::string::npos);
  EXPECT_NE(out[2].find("speech_end"), std::string::npos);
}

TEST_F(VadEventTest, SpeechStartEmitsInterruptRequest) {
  VadEventDevice dev;
  auto out = CollectVadOutput(
      dev, VadEventDevice::kInterruptOut, {"speech_start"});
  ASSERT_EQ(out.size(), 1U);
  EXPECT_EQ(out[0], InterruptFrame::kReasonBargeIn);
}

TEST_F(VadEventTest, SpeechEndEmitsFlushControl) {
  VadEventDevice dev;
  auto out = CollectVadOutput(
      dev, VadEventDevice::kControlOut, {"speech_end"});
  ASSERT_EQ(out.size(), 1U);
  EXPECT_EQ(out[0], ControlFrame::kCommandFlush);
}

TEST_F(VadEventTest, UnknownPortIgnored) {
  VadEventDevice dev;
  std::vector<std::string> out;
  dev.SetOutputCallback([&](auto, auto, DataFrame f) {
    out.emplace_back(f.payload.begin(), f.payload.end());
  });
  dev.Start();
  dev.OnInput("wrong_port", MakeVadEvent("speech_end"));
  EXPECT_TRUE(out.empty());
}

TEST_F(VadEventTest, SetOutputCallbackDoesNotThrow) {
  VadEventDevice dev;
  EXPECT_NO_THROW(dev.SetOutputCallback([](auto, auto, auto) {}));
}

// ---------------------------------------------------------------------------
// Integration: EnergyVadDevice vad_out → VadEventDevice
// ---------------------------------------------------------------------------

TEST(VadIntegrationTest, VadEventDeviceEmitsFlushOnSpeechEnd) {
  EnergyVadConfig cfg{
      .energy_threshold        = 1000.0F,
      .rms_window_frames       = 1,
      .speech_onset_frames     = 2,
      .silence_hangover_frames = 3,
      .pre_roll_frames         = 0,
  };
  EnergyVadDevice vad(cfg);
  VadEventDevice flusher;

  int speech_end_count = 0;
  flusher.SetOutputCallback([&](auto, const std::string& port, DataFrame f) {
    if (port == VadEventDevice::kControlOut &&
        ControlFrame::Parse(f) == ControlFrame::kCommandFlush) {
      ++speech_end_count;
    }
  });

  vad.SetOutputCallback([&](auto, const std::string& port, DataFrame f) {
    if (port == EnergyVadDevice::kVadOut) {
      flusher.OnInput(VadEventDevice::kVadIn, std::move(f));
    }
  });

  vad.Start();
  flusher.Start();

  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(5000));
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(5000));  // speech_start
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(0));
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(0));
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(0));     // speech_end
  EXPECT_EQ(speech_end_count, 1);

  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(5000));
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(5000));
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(0));
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(0));
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(0));
  EXPECT_EQ(speech_end_count, 2);
}

TEST(VadIntegrationTest, AudioOutOnlyContainsSpeechFrames) {
  EnergyVadConfig cfg{
      .energy_threshold        = 1000.0F,
      .rms_window_frames       = 1,
      .speech_onset_frames     = 2,
      .silence_hangover_frames = 2,
      .pre_roll_frames         = 0,
  };
  EnergyVadDevice vad(cfg);
  std::vector<DataFrame> audio_out;
  vad.SetOutputCallback([&](auto, const std::string& port, DataFrame f) {
    if (port == EnergyVadDevice::kAudioOut) { audio_out.push_back(std::move(f)); }
  });
  vad.Start();

  // 3 silence frames — nothing forwarded
  for (int i = 0; i < 3; ++i) {
    vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(0));
  }
  EXPECT_TRUE(audio_out.empty());

  // 2 loud (speech_start) + 1 loud (speech_active) + 2 silence (speech_end)
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(5000));
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(5000));
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(5000));
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(0));
  vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(0));
  // onset frame 1 not emitted (pre_roll=0, onset not yet confirmed)
  // onset frame 2 → speech_start, emitted (1)
  // loud frame 3  → speech_active, emitted (2)
  // silence 1     → hangover, emitted (3)
  // silence 2     → speech_end, emitted (4)
  EXPECT_EQ(audio_out.size(), 4U);

  // 3 more silence — nothing forwarded
  const size_t before = audio_out.size();
  for (int i = 0; i < 3; ++i) {
    vad.OnInput(EnergyVadDevice::kAudioIn, MakeAudioFrame(0));
  }
  EXPECT_EQ(audio_out.size(), before);
}

}  // namespace
}  // namespace shizuru::io
