#include <rex/graphics/fh1_frame_replay.h>

#include <cstring>
#include <filesystem>
#include <fstream>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/types.h>

REXCVAR_DEFINE_STRING(fh1_frame_replay, "", "GPU",
                      "Replay this FH1 frame dump instead of running the title; the result is "
                      "written next to it")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics {

namespace {

constexpr char kMagic[8] = {'F', 'H', '1', 'F', 'R', 'M', '0', '1'};
constexpr uint32_t kPhysicalSize = 0x20000000;

uint32_t Swap(uint32_t value) { return rex::byte_swap(value); }

template <typename T>
bool Get(std::ifstream& in, T& value) {
  return bool(in.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

}  // namespace

int RunFh1FrameReplay(CommandProcessor& command_processor, RegisterFile& register_file,
                      memory::Memory& memory, SharedMemory& shared_memory,
                      const Fh1FrameReplayReadback& read_front_buffer) {
  const std::filesystem::path path(REXCVAR_GET(fh1_frame_replay));
  if (path.empty()) return -1;
  std::ifstream in(path, std::ios::binary);
  char magic[8] = {};
  uint64_t frame = 0;
  uint32_t register_count = 0;
  if (!in.read(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) ||
      !Get(in, frame) || !Get(in, register_count) ||
      register_count > RegisterFile::kRegisterCount) {
    REXGPU_ERROR("FH1 frame replay: {} is not a frame dump", path.string());
    return 2;
  }
  std::vector<uint32_t> registers(register_count);
  in.read(reinterpret_cast<char*>(registers.data()), register_count * 4);
  // Shaders active at the start, loaded like IM_LOAD_IMMEDIATE would.
  std::vector<uint32_t> packets;
  uint32_t shader_count = 0;
  Get(in, shader_count);
  for (uint32_t i = 0; i < shader_count; ++i) {
    uint32_t type = 0, size = 0;
    Get(in, type);
    Get(in, size);
    std::vector<uint32_t> ucode(size);
    in.read(reinterpret_cast<char*>(ucode.data()), size * 4);
    packets.push_back(Swap((3u << 30) | ((size + 2 - 1) << 16) |
                           (uint32_t(xenos::PM4_IM_LOAD_IMMEDIATE) << 8)));
    packets.push_back(Swap(type));
    packets.push_back(Swap(size));
    for (uint32_t dword : ucode) packets.push_back(Swap(dword));
  }
  uint32_t packet_count = 0;
  Get(in, packet_count);
  const size_t recorded_first = packets.size();
  packets.resize(recorded_first + packet_count);
  in.read(reinterpret_cast<char*>(packets.data() + recorded_first), packet_count * 4);
  uint32_t block_count = 0;
  Get(in, block_count);
  uint64_t block_bytes = 0;
  for (uint32_t i = 0; i < block_count; ++i) {
    uint32_t address = 0, length = 0;
    Get(in, address);
    Get(in, length);
    uint8_t* destination = memory.TranslatePhysical(address);
    if (!destination || uint64_t(address) + length > kPhysicalSize) {
      in.seekg(length, std::ios::cur);
      continue;
    }
    in.read(reinterpret_cast<char*>(destination), length);
    block_bytes += length;
  }
  uint32_t front_address = 0, front_length = 0, width = 0, height = 0, format = 0, tiled = 0;
  Get(in, front_address);
  Get(in, front_length);
  Get(in, width);
  Get(in, height);
  Get(in, format);
  Get(in, tiled);
  std::vector<uint8_t> expected(front_length);
  in.read(reinterpret_cast<char*>(expected.data()), front_length);
  if (!in) {
    REXGPU_ERROR("FH1 frame replay: {} is truncated", path.string());
    return 2;
  }

  // Everything written above is new CPU data for the GPU mirror.
  shared_memory.InvalidateAllPages();
  std::memcpy(register_file.values, registers.data(), registers.size() * 4);
  REXGPU_INFO("FH1 frame replay: frame {} from {} ({} packet dwords, {} blocks, {} MB)", frame,
              path.string(), packet_count, block_count, block_bytes >> 20);
  const bool executed =
      command_processor.ExecuteHostPackets(packets.data(), uint32_t(packets.size()));

  // Compare the front buffer the replay resolved with the recorded one.
  std::vector<uint8_t> actual(front_length);
  if (front_length && !read_front_buffer(front_address, front_length, actual)) {
    REXGPU_ERROR("FH1 frame replay: the front buffer could not be read back");
  }
  uint64_t differing_words = 0;
  for (uint32_t i = 0; i + 4 <= front_length; i += 4) {
    differing_words += std::memcmp(expected.data() + i, actual.data() + i, 4) != 0;
  }
  std::filesystem::path result = path;
  result += ".replay.bin";
  std::ofstream(result, std::ios::binary)
      .write(reinterpret_cast<const char*>(actual.data()), actual.size());
  std::filesystem::path json = path;
  json += ".replay.json";
  std::ofstream(json) << "{\"frame\":" << frame << ",\"executed\":" << (executed ? "true" : "false")
                      << ",\"packet_dwords\":" << packet_count << ",\"blocks\":" << block_count
                      << ",\"front_address\":" << front_address
                      << ",\"front_bytes\":" << front_length << ",\"width\":" << width
                      << ",\"height\":" << height << ",\"format\":" << format
                      << ",\"tiled\":" << tiled << ",\"differing_words\":" << differing_words
                      << "}\n";
  REXGPU_INFO("FH1 frame replay: done, {} of {} front buffer words differ ({})", differing_words,
              front_length / 4, json.string());
  return executed ? 0 : 1;
}

}  // namespace rex::graphics
