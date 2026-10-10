/**
 * @file        host_music_test.cpp
 * @brief       Host music player track discovery and WAV decoding tests
 * @license     BSD 3-Clause License
 */

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/audio/host_music.h>

namespace {

namespace fs = std::filesystem;
using rex::audio::HostMusicPlayer;

void Put16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(uint8_t(value));
  out.push_back(uint8_t(value >> 8));
}

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  Put16(out, uint16_t(value));
  Put16(out, uint16_t(value >> 16));
}

void WriteWav(const fs::path& path, uint16_t format, uint16_t channels, uint32_t rate,
              uint16_t bits, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> out;
  out.insert(out.end(), {'R', 'I', 'F', 'F'});
  Put32(out, uint32_t(36 + data.size()));
  out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  Put32(out, 16);
  Put16(out, format);
  Put16(out, channels);
  Put32(out, rate);
  Put32(out, rate * channels * bits / 8);
  Put16(out, uint16_t(channels * bits / 8));
  Put16(out, bits);
  out.insert(out.end(), {'d', 'a', 't', 'a'});
  Put32(out, uint32_t(data.size()));
  out.insert(out.end(), data.begin(), data.end());
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(out.data()),
                                              std::streamsize(out.size()));
}

struct TempFolder {
  fs::path path = fs::temp_directory_path() / "rex_host_music_test";
  TempFolder() {
    fs::remove_all(path);
    fs::create_directories(path / "sub");
  }
  ~TempFolder() {
    std::error_code error;
    fs::remove_all(path, error);
  }
};

}  // namespace

TEST_CASE("Host music decodes mono PCM16 WAV to stereo float", "[audio][host_music]") {
  TempFolder folder;
  std::vector<uint8_t> data;
  for (int16_t sample : {int16_t(0), int16_t(16384), int16_t(-32768)}) {
    Put16(data, uint16_t(sample));
  }
  const auto file = folder.path / "mono.wav";
  WriteWav(file, 1, 1, 22050, 16, data);

  int rate = 0;
  const auto samples = HostMusicPlayer::DecodeForTest(file, rate);
  REQUIRE(rate == 22050);
  REQUIRE(samples.size() == 6);
  CHECK(samples[0] == 0.0f);
  CHECK(samples[2] == 0.5f);
  CHECK(samples[3] == 0.5f);
  CHECK(samples[4] == -1.0f);
}

TEST_CASE("Host music decodes stereo float WAV", "[audio][host_music]") {
  TempFolder folder;
  std::vector<uint8_t> data(4 * sizeof(float));
  const float values[] = {0.25f, -0.25f, 1.0f, -1.0f};
  std::memcpy(data.data(), values, sizeof(values));
  const auto file = folder.path / "float.wav";
  WriteWav(file, 3, 2, 48000, 32, data);

  int rate = 0;
  const auto samples = HostMusicPlayer::DecodeForTest(file, rate);
  REQUIRE(rate == 48000);
  REQUIRE(samples.size() == 4);
  CHECK(samples[0] == 0.25f);
  CHECK(samples[1] == -0.25f);
  CHECK(samples[3] == -1.0f);
}

TEST_CASE("Host music rejects files it cannot decode", "[audio][host_music]") {
  TempFolder folder;
  const auto file = folder.path / "broken.wav";
  std::ofstream(file, std::ios::binary) << "not a wave file";
  int rate = 0;
  CHECK(HostMusicPlayer::DecodeForTest(file, rate).empty());
}

TEST_CASE("Host music finds playable tracks recursively in name order", "[audio][host_music]") {
  TempFolder folder;
  std::vector<uint8_t> data(4);
  WriteWav(folder.path / "b.wav", 1, 1, 44100, 16, data);
  WriteWav(folder.path / "sub" / "a.WAV", 1, 1, 44100, 16, data);
  std::ofstream(folder.path / "notes.txt") << "x";
  std::ofstream(folder.path / "c.mp3") << "x";

  const auto tracks = HostMusicPlayer::FindTracks(folder.path);
  std::vector<fs::path> expected{folder.path / "b.wav"};
  if (HostMusicPlayer::CanDecodeMp3()) expected.push_back(folder.path / "c.mp3");
  expected.push_back(folder.path / "sub" / "a.WAV");
  CHECK(tracks == expected);
}
