/**
 * @file        rex/audio/host_music.h
 *
 * @brief       A host music player for the player's own files, mixed over the
 *              title's audio on its own output stream.
 */

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rex::audio {

// Plays MP3 and WAV files from a folder (recursively, in name order or
// shuffled) on its own SDL output stream. Decoding runs on the player's own
// thread; every call is safe from any thread. Taking playback over is how a
// title learns that the system's music player is active (see XmpApp).
// True while the host player has the music, for the XMP app made after the
// player started (it then reports the system's player as the controller).
bool HostMusicHasTheMusic();

class HostMusicPlayer {
 public:
  struct Status {
    bool playing = false;
    bool paused = false;
    size_t index = 0;   // into tracks
    size_t count = 0;
    std::string title;  // the current file's name without its extension
  };

  static HostMusicPlayer& Get();

  // The files under `folder` that can be played (.mp3 where the build's
  // FFmpeg has the decoder, and .wav), sorted by path.
  static std::vector<std::filesystem::path> FindTracks(const std::filesystem::path& folder);
  static bool CanDecodeMp3();

  // Scans `folder` and starts its first track (or a random one when
  // shuffling). False when it holds nothing to play.
  bool Play(const std::filesystem::path& folder);
  void Stop();
  void SetPaused(bool paused);
  void Next();
  void Previous();
  void SetVolume(float volume);  // 0..1
  void SetShuffle(bool shuffle);
  Status GetStatus() const;

  // Called on the player's thread when playback starts or stops (not on
  // pause), so the title can be told the system player has the music.
  void SetActiveCallback(std::function<void(bool active)> callback);

  // Stops the thread and closes the output stream, before SDL's audio shuts
  // down; the player does nothing afterwards.
  void Close();

  // Decodes a whole file to interleaved stereo float at its own rate, for
  // tests. Empty when it cannot be decoded.
  static std::vector<float> DecodeForTest(const std::filesystem::path& file, int& sample_rate);

  ~HostMusicPlayer();

 private:
  HostMusicPlayer();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rex::audio
