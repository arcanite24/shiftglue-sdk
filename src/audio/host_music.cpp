/**
 * @file        audio/host_music.cpp
 *
 * @brief       Host music player: the player's own MP3 and WAV files on an
 *              SDL output stream of their own.
 */

#include <rex/filesystem.h>
#include <rex/audio/host_music.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>

#include <SDL3/SDL.h>

#include <rex/audio/downmix.h>
#include <rex/audio/flags.h>
#include <rex/cvar.h>
#include <rex/logging.h>

extern "C" {
#if REX_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4101 4244 5033)
#endif
#include "libavcodec/avcodec.h"
#if REX_COMPILER_MSVC
#pragma warning(pop)
#endif
}  // extern "C"

namespace rex::audio {

namespace {

std::atomic<bool> g_has_the_music{false};

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return text;
}

// A decoder yields interleaved stereo float at its file's rate.
class Decoder {
 public:
  virtual ~Decoder() = default;
  // Appends up to about `frames` stereo frames; false at the end.
  virtual bool Read(std::vector<float>& out, size_t frames) = 0;
  int rate = 0;
};

uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t U32(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24); }

// PCM 16-bit and 32-bit float WAV, mono or stereo (more channels: the first
// two).
class WavDecoder final : public Decoder {
 public:
  bool Open(std::vector<uint8_t> data) {
    data_ = std::move(data);
    if (data_.size() < 12 || std::memcmp(data_.data(), "RIFF", 4) ||
        std::memcmp(data_.data() + 8, "WAVE", 4)) {
      return false;
    }
    size_t at = 12;
    bool have_format = false;
    while (at + 8 <= data_.size()) {
      const uint32_t size = U32(&data_[at + 4]);
      const uint8_t* body = &data_[at + 8];
      if (!std::memcmp(&data_[at], "fmt ", 4) && size >= 16 && at + 8 + 16 <= data_.size()) {
        format_ = U16(body);
        channels_ = U16(body + 2);
        rate = int(U32(body + 4));
        bits_ = U16(body + 14);
        if (format_ == 0xFFFE && size >= 26) format_ = U16(body + 24);  // extensible
        have_format = true;
      } else if (!std::memcmp(&data_[at], "data", 4)) {
        begin_ = at + 8;
        end_ = std::min<size_t>(data_.size(), begin_ + size);
        break;
      }
      at += 8 + size + (size & 1);
    }
    position_ = begin_;
    return have_format && begin_ && channels_ && rate > 0 &&
           ((format_ == 1 && bits_ == 16) || (format_ == 3 && bits_ == 32));
  }

  bool Read(std::vector<float>& out, size_t frames) override {
    const size_t stride = size_t(channels_) * bits_ / 8;
    for (size_t i = 0; i < frames && position_ + stride <= end_; ++i, position_ += stride) {
      float sample[2];
      for (int c = 0; c < 2; ++c) {
        const uint8_t* p = &data_[position_ + size_t(std::min(c, channels_ - 1)) * bits_ / 8];
        if (format_ == 1) {
          sample[c] = float(int16_t(U16(p))) / 32768.0f;
        } else {
          uint32_t bits = U32(p);
          std::memcpy(&sample[c], &bits, 4);
        }
      }
      out.push_back(sample[0]);
      out.push_back(sample[1]);
    }
    return position_ + stride <= end_;
  }

 private:
  std::vector<uint8_t> data_;
  size_t begin_ = 0, end_ = 0, position_ = 0;
  int format_ = 0, channels_ = 0, bits_ = 0;
};

// MPEG audio through the build's FFmpeg (the decoder and parser the XMA
// path already compiles where they are enabled).
class Mp3Decoder final : public Decoder {
 public:
  ~Mp3Decoder() override {
    av_frame_free(&frame_);
    av_packet_free(&packet_);
    if (parser_) av_parser_close(parser_);
    avcodec_free_context(&context_);
  }

  bool Open(std::vector<uint8_t> data) {
    data_ = std::move(data);
    // An ID3v2 tag in front: its size is four 7-bit bytes.
    if (data_.size() > 10 && !std::memcmp(data_.data(), "ID3", 3)) {
      position_ = 10 + (size_t(data_[6] & 0x7F) << 21 | size_t(data_[7] & 0x7F) << 14 |
                        size_t(data_[8] & 0x7F) << 7 | size_t(data_[9] & 0x7F));
      if (data_[5] & 0x10) position_ += 10;  // footer
    }
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MP3);
    if (!codec) return false;
    context_ = avcodec_alloc_context3(codec);
    parser_ = av_parser_init(AV_CODEC_ID_MP3);
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!context_ || !parser_ || !packet_ || !frame_ || avcodec_open2(context_, codec, nullptr) < 0) {
      return false;
    }
    // The rate comes with the first decoded frame.
    std::vector<float> first;
    while (!rate && Read(first, 1)) {
    }
    pending_ = std::move(first);
    return rate > 0;
  }

  bool Read(std::vector<float>& out, size_t frames) override {
    const size_t wanted = out.size() + frames * 2;
    if (!pending_.empty()) {
      out.insert(out.end(), pending_.begin(), pending_.end());
      pending_.clear();
    }
    while (out.size() < wanted) {
      if (Receive(out)) continue;
      if (flushed_) return false;
      if (position_ >= data_.size()) {
        // Drain the parser, then the decoder.
        uint8_t* packet_data = nullptr;
        int packet_size = 0;
        av_parser_parse2(parser_, context_, &packet_data, &packet_size, nullptr, 0,
                         AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (packet_size) Send(packet_data, packet_size);
        avcodec_send_packet(context_, nullptr);
        flushed_ = true;
        continue;
      }
      uint8_t* packet_data = nullptr;
      int packet_size = 0;
      const int used = av_parser_parse2(
          parser_, context_, &packet_data, &packet_size, data_.data() + position_,
          int(std::min<size_t>(data_.size() - position_, 4096)), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
      if (used < 0) return false;
      position_ += size_t(used);
      if (packet_size) Send(packet_data, packet_size);
    }
    return true;
  }

 private:
  void Send(uint8_t* data, int size) {
    packet_->data = data;
    packet_->size = size;
    // A damaged frame is skipped, as players do.
    avcodec_send_packet(context_, packet_);
  }

  bool Receive(std::vector<float>& out) {
    if (avcodec_receive_frame(context_, frame_) < 0) return false;
    rate = frame_->sample_rate;
    const int channels = std::max(1, frame_->channels);
    const auto format = AVSampleFormat(frame_->format);
    for (int i = 0; i < frame_->nb_samples; ++i) {
      for (int c = 0; c < 2; ++c) {
        const int channel = std::min(c, channels - 1);
        float sample = 0;
        switch (format) {
          case AV_SAMPLE_FMT_FLTP:
            sample = reinterpret_cast<const float*>(frame_->extended_data[channel])[i];
            break;
          case AV_SAMPLE_FMT_FLT:
            sample = reinterpret_cast<const float*>(frame_->extended_data[0])[i * channels + channel];
            break;
          case AV_SAMPLE_FMT_S16P:
            sample = reinterpret_cast<const int16_t*>(frame_->extended_data[channel])[i] / 32768.0f;
            break;
          case AV_SAMPLE_FMT_S16:
            sample = reinterpret_cast<const int16_t*>(frame_->extended_data[0])[i * channels + channel] /
                     32768.0f;
            break;
          default:
            break;
        }
        out.push_back(sample);
      }
    }
    av_frame_unref(frame_);
    return true;
  }

  std::vector<uint8_t> data_;
  size_t position_ = 0;
  bool flushed_ = false;
  std::vector<float> pending_;
  AVCodecContext* context_ = nullptr;
  AVCodecParserContext* parser_ = nullptr;
  AVPacket* packet_ = nullptr;
  AVFrame* frame_ = nullptr;
};

std::unique_ptr<Decoder> OpenDecoder(const std::filesystem::path& path) {
  const std::string extension = Lower(rex::path_to_utf8(path.extension()));
  auto data = ReadFile(path);
  if (extension == ".wav") {
    auto decoder = std::make_unique<WavDecoder>();
    if (decoder->Open(std::move(data))) return decoder;
  } else if (extension == ".mp3") {
    auto decoder = std::make_unique<Mp3Decoder>();
    if (decoder->Open(std::move(data))) return decoder;
  }
  return nullptr;
}

}  // namespace

struct HostMusicPlayer::Impl {
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::thread thread;
  bool quit = false;

  std::vector<std::filesystem::path> tracks;
  std::vector<size_t> order;  // play order into tracks
  size_t position = 0;        // into order
  bool active = false;        // a folder is playing (or paused)
  bool paused = false;
  bool shuffle = false;
  float volume = 1.0f;
  int skip = 0;               // tracks to move by, from Next/Previous
  bool restart = false;       // start order[position] again
  std::function<void(bool)> on_active;

  SDL_AudioStream* stream = nullptr;

  void Reorder() {
    order.resize(tracks.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    if (shuffle) {
      std::shuffle(order.begin(), order.end(), std::mt19937(std::random_device{}()));
    }
  }

  bool OpenStream() {
    if (stream) return true;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      REXAPU_ERROR("Host music: SDL audio is unavailable: {}", SDL_GetError());
      return false;
    }
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, 48000};
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream) {
      REXAPU_ERROR("Host music: cannot open an output stream: {}", SDL_GetError());
      return false;
    }
    SDL_ResumeAudioStreamDevice(stream);
    return true;
  }

  void Run() {
    std::unique_ptr<Decoder> decoder;
    std::vector<float> buffer;
    bool told_active = false;
    size_t failures = 0;
    std::unique_lock lock(mutex);
    while (!quit) {
      if (told_active != active) {
        told_active = active;
        g_has_the_music = told_active;
        if (auto callback = on_active) {
          lock.unlock();
          callback(told_active);
          lock.lock();
        }
        continue;
      }
      if (!active || tracks.empty()) {
        decoder.reset();
        if (stream) SDL_ClearAudioStream(stream);
        wake.wait(lock);
        continue;
      }
      if (skip || restart) {
        const auto count = int64_t(order.size());
        position = size_t(((int64_t(position) + skip) % count + count) % count);
        if (restart) failures = 0;
        skip = 0;
        restart = false;
        decoder.reset();
        if (stream) SDL_ClearAudioStream(stream);
      }
      if (!stream && !OpenStream()) {
        active = false;
        continue;
      }
      // Under the master volume, and silent where audio is muted (hidden
      // route runs).
      SDL_SetAudioStreamGain(stream, paused || REXCVAR_GET(audio_mute)
                                         ? 0.0f
                                         : volume * GetOutputGain());
      if (paused) {
        SDL_PauseAudioStreamDevice(stream);
        wake.wait(lock);
        SDL_ResumeAudioStreamDevice(stream);
        continue;
      }
      if (!decoder) {
        const auto path = tracks[order[position]];
        lock.unlock();
        decoder = OpenDecoder(path);
        lock.lock();
        if (!decoder) {
          REXAPU_WARN("Host music: cannot play {}", rex::path_to_utf8(path));
          // Nothing in the folder plays: stop instead of trying forever.
          if (++failures >= tracks.size()) {
            active = false;
          }
          skip = 1;
          continue;
        }
        failures = 0;
        const SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, decoder->rate};
        SDL_SetAudioStreamFormat(stream, &spec, nullptr);
        REXAPU_INFO("Host music: playing {}", rex::path_to_utf8(path.filename()));
      }
      // Keep about a quarter second queued.
      const int queued = SDL_GetAudioStreamQueued(stream);
      const int target = decoder->rate * 2 * int(sizeof(float)) / 4;
      if (queued >= target) {
        wake.wait_for(lock, std::chrono::milliseconds(20));
        continue;
      }
      buffer.clear();
      lock.unlock();
      const bool more = decoder->Read(buffer, size_t(decoder->rate) / 20);
      lock.lock();
      if (!buffer.empty()) {
        SDL_PutAudioStreamData(stream, buffer.data(), int(buffer.size() * sizeof(float)));
      }
      if (!more) {
        // The track ended: the next one once this one has played out.
        decoder.reset();
        while (!quit && stream && SDL_GetAudioStreamQueued(stream) > 0 && !restart && !skip &&
               active) {
          wake.wait_for(lock, std::chrono::milliseconds(20));
        }
        if (!restart && !skip) skip = 1;
      }
    }
  }
};

bool HostMusicHasTheMusic() {
  return g_has_the_music.load(std::memory_order_relaxed);
}

HostMusicPlayer& HostMusicPlayer::Get() {
  static HostMusicPlayer player;
  return player;
}

HostMusicPlayer::HostMusicPlayer() : impl_(std::make_unique<Impl>()) {
  impl_->thread = std::thread([impl = impl_.get()] { impl->Run(); });
}

HostMusicPlayer::~HostMusicPlayer() { Close(); }

void HostMusicPlayer::Close() {
  {
    std::lock_guard lock(impl_->mutex);
    impl_->quit = true;
  }
  impl_->wake.notify_all();
  if (impl_->thread.joinable()) impl_->thread.join();
  if (impl_->stream) {
    SDL_DestroyAudioStream(impl_->stream);
    impl_->stream = nullptr;
  }
}

bool HostMusicPlayer::CanDecodeMp3() {
  return avcodec_find_decoder(AV_CODEC_ID_MP3) != nullptr;
}

std::vector<std::filesystem::path> HostMusicPlayer::FindTracks(
    const std::filesystem::path& folder) {
  std::vector<std::filesystem::path> tracks;
  const bool mp3 = CanDecodeMp3();
  std::error_code error;
  for (auto it = std::filesystem::recursive_directory_iterator(
           folder, std::filesystem::directory_options::skip_permission_denied, error);
       !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
    if (!it->is_regular_file(error)) continue;
    const std::string extension = Lower(rex::path_to_utf8(it->path().extension()));
    if (extension == ".wav" || (mp3 && extension == ".mp3")) tracks.push_back(it->path());
  }
  std::sort(tracks.begin(), tracks.end());
  return tracks;
}

bool HostMusicPlayer::Play(const std::filesystem::path& folder) {
  auto tracks = FindTracks(folder);
  {
    std::lock_guard lock(impl_->mutex);
    impl_->tracks = std::move(tracks);
    impl_->Reorder();
    impl_->position = 0;
    impl_->restart = true;
    impl_->paused = false;
    impl_->active = !impl_->tracks.empty();
  }
  impl_->wake.notify_all();
  return GetStatus().count > 0;
}

void HostMusicPlayer::Stop() {
  {
    std::lock_guard lock(impl_->mutex);
    impl_->active = false;
    impl_->paused = false;
  }
  impl_->wake.notify_all();
}

void HostMusicPlayer::SetPaused(bool paused) {
  {
    std::lock_guard lock(impl_->mutex);
    impl_->paused = paused && impl_->active;
  }
  impl_->wake.notify_all();
}

void HostMusicPlayer::Next() {
  {
    std::lock_guard lock(impl_->mutex);
    ++impl_->skip;
  }
  impl_->wake.notify_all();
}

void HostMusicPlayer::Previous() {
  {
    std::lock_guard lock(impl_->mutex);
    --impl_->skip;
  }
  impl_->wake.notify_all();
}

void HostMusicPlayer::SetVolume(float volume) {
  {
    std::lock_guard lock(impl_->mutex);
    impl_->volume = std::clamp(volume, 0.0f, 1.0f);
  }
  impl_->wake.notify_all();
}

void HostMusicPlayer::SetShuffle(bool shuffle) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->shuffle == shuffle) return;
  const size_t current = impl_->order.empty() ? 0 : impl_->order[impl_->position];
  impl_->shuffle = shuffle;
  impl_->Reorder();
  // Keep the current track playing; the new order continues from it.
  const auto it = std::find(impl_->order.begin(), impl_->order.end(), current);
  impl_->position = it == impl_->order.end() ? 0 : size_t(it - impl_->order.begin());
}

HostMusicPlayer::Status HostMusicPlayer::GetStatus() const {
  std::lock_guard lock(impl_->mutex);
  Status status;
  status.playing = impl_->active && !impl_->paused;
  status.paused = impl_->active && impl_->paused;
  status.count = impl_->tracks.size();
  if (!impl_->order.empty()) {
    status.index = impl_->order[impl_->position];
    status.title = impl_->tracks[status.index].stem().string();
  }
  return status;
}

void HostMusicPlayer::SetActiveCallback(std::function<void(bool active)> callback) {
  std::lock_guard lock(impl_->mutex);
  impl_->on_active = std::move(callback);
}

std::vector<float> HostMusicPlayer::DecodeForTest(const std::filesystem::path& file,
                                                  int& sample_rate) {
  std::vector<float> samples;
  auto decoder = OpenDecoder(file);
  sample_rate = decoder ? decoder->rate : 0;
  while (decoder && decoder->Read(samples, 4096)) {
  }
  return samples;
}

}  // namespace rex::audio
