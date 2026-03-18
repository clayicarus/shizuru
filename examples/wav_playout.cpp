#include <cassert>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/fcntl.h>
#include <thread>

#include "audio_device/audio_frame.h"
#include "audio_device/port_audio/pa_player.h"

// ---------------------------------------------------------------------------
// Minimal WAV header parser (PCM only)
// ---------------------------------------------------------------------------
struct WavHeader {
  int    sample_rate;
  int    channel_count;
  int    bits_per_sample;
  size_t data_offset;  // byte offset of the first PCM sample in the file
  size_t data_size;    // byte count of the data chunk (from header)
};

static WavHeader ParseWav(FILE* fp) {
  auto read_u16 = [&]() -> uint16_t {
    uint8_t b[2];
    if (fread(b, 1, 2, fp) != 2) { throw std::runtime_error("Unexpected EOF"); }
    return static_cast<uint16_t>(b[0] | (b[1] << 8));
  };
  auto read_u32 = [&]() -> uint32_t {
    uint8_t b[4];
    if (fread(b, 1, 4, fp) != 4) { throw std::runtime_error("Unexpected EOF"); }
    return static_cast<uint32_t>(b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24));
  };
  auto read_tag = [&](char out[4]) {
    if (fread(out, 1, 4, fp) != 4) { throw std::runtime_error("Unexpected EOF"); }
  };

  char tag[4];
  read_tag(tag);
  if (std::memcmp(tag, "RIFF", 4) != 0) { throw std::runtime_error("Not a RIFF file"); }
  read_u32();  // RIFF chunk size
  read_tag(tag);
  if (std::memcmp(tag, "WAVE", 4) != 0) { throw std::runtime_error("Not a WAVE file"); }

  WavHeader hdr{};
  bool found_fmt = false, found_data = false;

  while (!found_data) {
    read_tag(tag);
    const uint32_t chunk_size = read_u32();

    if (std::memcmp(tag, "fmt ", 4) == 0) {
      const uint16_t audio_format = read_u16();
      if (audio_format != 1) { throw std::runtime_error("Only PCM WAV supported"); }
      hdr.channel_count  = read_u16();
      hdr.sample_rate    = static_cast<int>(read_u32());
      read_u32();  // byte rate
      read_u16();  // block align
      hdr.bits_per_sample = read_u16();
      if (hdr.bits_per_sample != 16) {
        throw std::runtime_error("Only 16-bit PCM WAV supported");
      }
      // Skip any extra fmt bytes
      if (chunk_size > 16) { fseek(fp, static_cast<long>(chunk_size - 16), SEEK_CUR); }
      found_fmt = true;
    } else if (std::memcmp(tag, "data", 4) == 0) {
      if (!found_fmt) { throw std::runtime_error("data chunk before fmt chunk"); }
      hdr.data_size   = chunk_size;
      hdr.data_offset = static_cast<size_t>(ftell(fp));
      found_data = true;
    } else {
      // Skip unknown chunk
      fseek(fp, static_cast<long>(chunk_size), SEEK_CUR);
    }
  }
  return hdr;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: wav_playout <file.wav>\n");
    return 1;
  }

  try {
    FILE* fp = fopen(argv[1], "rb");
    if (!fp) { throw std::runtime_error(std::string("Cannot open: ") + argv[1]); }

    const WavHeader hdr = ParseWav(fp);
    std::printf("File:     %s\n", argv[1]);
    std::printf("Format:   s16le, %d Hz, %d ch\n", hdr.sample_rate, hdr.channel_count);

    // 20ms frames
    const size_t samples_per_frame =
        static_cast<size_t>(hdr.sample_rate) * 20 / 1000;

    shizuru::io::PlayerConfig cfg;
    cfg.sample_rate       = hdr.sample_rate;
    cfg.channel_count     = static_cast<size_t>(hdr.channel_count);
    cfg.frames_per_buffer = samples_per_frame;
    // Ring buffer: 2 seconds
    cfg.buffer_capacity_samples =
        static_cast<size_t>(hdr.sample_rate) * 2;

    shizuru::io::PaPlayer player(cfg);
    player.Start();
    std::printf("Playing...\n");

    shizuru::io::AudioFrame frame;
    frame.sample_rate   = hdr.sample_rate;
    frame.channel_count = static_cast<size_t>(hdr.channel_count);
    frame.sample_count  = samples_per_frame;

    const size_t elems_per_frame = samples_per_frame * static_cast<size_t>(hdr.channel_count);

    size_t bytes_remaining = hdr.data_size;
    while (bytes_remaining > 0) {
      const size_t bytes_to_read =
          std::min(elems_per_frame * sizeof(int16_t), bytes_remaining);
      const size_t read = fread(frame.data, 1, bytes_to_read, fp);
      if (read == 0) { break; }
      bytes_remaining -= read;
      frame.sample_count = (read / sizeof(int16_t)) / static_cast<size_t>(hdr.channel_count);

      // Back-pressure: wait if ring buffer is nearly full
      while (player.Buffered() + frame.sample_count > cfg.buffer_capacity_samples - 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      player.Write(frame);
    }
    fclose(fp);

    // Drain: wait until the ring buffer is empty
    while (player.Buffered() > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Give PortAudio one extra callback cycle to flush
    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    player.Stop();
    std::printf("Done.\n");

  } catch (const std::exception& e) {
    std::fprintf(stderr, "Error: %s\n", e.what());
    return 1;
  }
  return 0;
}
