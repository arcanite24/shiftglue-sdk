#include <rex/graphics/d3d12/fh1_frame_dump.h>

#include <algorithm>
#include <cstring>
#include <fstream>

#include <rex/types.h>
#include <rex/cvar.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/shared_memory.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(fh1_frame_dump_frame, 0, "GPU/D3D12",
                     "Record this FH1 frame (executor swap number) for offline replay")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fh1_frame_dump_path, "", "GPU/D3D12",
                      "File the recorded FH1 frame dump is written to")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fh1_frame_replay, "", "GPU/D3D12",
                      "Replay this FH1 frame dump instead of running the title; the result is "
                      "written next to it")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::d3d12 {

namespace {

using namespace xenos;

constexpr char kMagic[8] = {'F', 'H', '1', 'F', 'R', 'M', '0', '1'};
constexpr uint32_t kPhysicalSize = 0x20000000;

uint32_t Swap(uint32_t value) { return rex::byte_swap(value); }

template <typename T>
void Put(std::ofstream& out, const T& value) {
  out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

template <typename T>
bool Get(std::ifstream& in, T& value) {
  return bool(in.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

// The front buffer's guest bytes from texture fetch constant 0, as set for
// the swap.
std::pair<uint32_t, uint32_t> FrontBufferRange(const RegisterFile& register_file,
                                               uint32_t frontbuffer_address) {
  const auto fetch = register_file.GetTextureFetch(0);
  const uint32_t length =
      texture_util::GetGuestTextureLayout(fetch.dimension, fetch.pitch, fetch.size_2d.width + 1,
                                          fetch.size_2d.height + 1, 1, fetch.tiled, fetch.format,
                                          fetch.packed_mips, true, 0)
          .base.level_data_extent_bytes;
  const uint32_t address = frontbuffer_address ? frontbuffer_address : fetch.base_address << 12;
  return {address & (kPhysicalSize - 1), length};
}

ID3D12Resource* CreateReadback(ID3D12Device* device, uint32_t size) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* resource = nullptr;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&resource)))) {
    return nullptr;
  }
  return resource;
}

}  // namespace

Fh1FrameDump::Fh1FrameDump(D3D12CommandProcessor& command_processor,
                           const RegisterFile& register_file, memory::Memory& memory,
                           D3D12SharedMemory& shared_memory, uint64_t frame,
                           std::filesystem::path path)
    : command_processor_(command_processor),
      register_file_(register_file),
      memory_(memory),
      shared_memory_(shared_memory),
      frame_(frame),
      path_(std::move(path)) {}

Fh1FrameDump::~Fh1FrameDump() {
  if (recording_) command_processor_.SetPacketRecorder(nullptr);
}

uint64_t Fh1FrameDump::RequestedFrame() {
  return uint64_t(std::max(REXCVAR_GET(fh1_frame_dump_frame), 0));
}

std::filesystem::path Fh1FrameDump::RequestedPath() {
  return std::filesystem::path(REXCVAR_GET(fh1_frame_dump_path));
}

void Fh1FrameDump::OnSwap(uint64_t frame, uint32_t frontbuffer_address) {
  if (done_) return;
  if (!recording_ && frame + 1 == frame_) {
    Begin();
  } else if (recording_ && frame == frame_) {
    End(frontbuffer_address);
  }
}

void Fh1FrameDump::Begin() {
  recording_ = true;
  registers_.assign(register_file_.values, register_file_.values + RegisterFile::kRegisterCount);
  for (Shader* shader : {command_processor_.active_vertex_shader(),
                         command_processor_.active_pixel_shader()}) {
    if (!shader) continue;
    shaders_.emplace_back(uint32_t(shader->type()),
                          std::vector<uint32_t>(shader->ucode_dwords(),
                                                shader->ucode_dwords() + shader->ucode_dword_count()));
  }
  recorded_pages_.assign(kPhysicalSize >> kPageSizeLog2, 0);
  // Bin mask and select are packet state, not registers: restore them with
  // packets at the start of the stream.
  const std::pair<uint32_t, uint64_t> bin_state[] = {
      {PM4_SET_BIN_MASK_LO, command_processor_.bin_mask()},
      {PM4_SET_BIN_MASK_HI, command_processor_.bin_mask() >> 32},
      {PM4_SET_BIN_SELECT_LO, command_processor_.bin_select()},
      {PM4_SET_BIN_SELECT_HI, command_processor_.bin_select() >> 32}};
  for (const auto& [opcode, value] : bin_state) {
    packets_.push_back(Swap((3u << 30) | (opcode << 8)));
    packets_.push_back(Swap(uint32_t(value)));
  }
  command_processor_.SetPacketRecorder(
      [this](const uint32_t* dwords, uint32_t count) { OnPacket(dwords, count); });
  REXGPU_INFO("FH1 frame dump: recording frame {}", frame_);
}

void Fh1FrameDump::OnPacket(const uint32_t* dwords, uint32_t count) {
  const size_t first = packets_.size();
  packets_.insert(packets_.end(), dwords, dwords + count);
  const uint32_t packet = Swap(dwords[0]);
  if ((packet >> 30) != 3) return;
  const uint32_t opcode = (packet >> 8) & 0x7F;
  switch (opcode) {
    case PM4_IM_LOAD:
      // Shader code loaded from guest memory.
      if (count >= 3) {
        RecordCpuRange(Swap(dwords[1]) & ~3u, (Swap(dwords[2]) & 0xFFFF) * 4);
      }
      break;
    case PM4_LOAD_ALU_CONSTANT:
      if (count >= 4) {
        RecordCpuRange(Swap(dwords[1]) & 0x3FFFFFFF, (Swap(dwords[3]) & 0xFFF) * 4);
      }
      break;
    case PM4_WAIT_REG_MEM:
    case PM4_WAIT_REG_EQ:
    case PM4_WAIT_REG_GTE:
    case PM4_INTERRUPT:
      // Waits on the title and interrupts to it have nothing to wait for or
      // deliver in a replay: keep the size, skip the packet.
      packets_[first] = Swap((packet & ~(0x7Fu << 8)) | (uint32_t(PM4_NOP) << 8));
      break;
    default:
      break;
  }
}

std::vector<std::pair<uint32_t, uint32_t>> Fh1FrameDump::TakeNewPages(uint32_t address,
                                                                      uint32_t length) {
  std::vector<std::pair<uint32_t, uint32_t>> runs;
  address &= kPhysicalSize - 1;
  if (!length) return runs;
  const uint32_t first = address >> kPageSizeLog2;
  const uint32_t end =
      std::min<uint64_t>((uint64_t(address) + length + (1u << kPageSizeLog2) - 1) >> kPageSizeLog2,
                         recorded_pages_.size());
  for (uint32_t page = first; page < end; ++page) {
    if (recorded_pages_[page]) continue;
    recorded_pages_[page] = 1;
    if (!runs.empty() && runs.back().first + runs.back().second == page << kPageSizeLog2) {
      runs.back().second += 1u << kPageSizeLog2;
    } else {
      runs.emplace_back(page << kPageSizeLog2, 1u << kPageSizeLog2);
    }
  }
  return runs;
}

void Fh1FrameDump::RecordCpuRange(uint32_t address, uint32_t length) {
  for (const auto& [start, size] : TakeNewPages(address, length)) {
    const uint8_t* source = memory_.TranslatePhysical(start);
    if (!source) continue;
    blocks_.push_back({start, size, std::vector<uint8_t>(source, source + size)});
  }
}

bool Fh1FrameDump::CopyFromMirror(uint32_t address, uint32_t length, uint32_t& chunk,
                                  uint32_t& offset) {
  if (length > kChunkSize) return false;
  if (chunk_used_ + length > kChunkSize) {
    ID3D12Resource* resource =
        CreateReadback(command_processor_.GetD3D12Provider().GetDevice(), kChunkSize);
    if (!resource) return false;
    chunks_.emplace_back().Attach(resource);
    chunk_used_ = 0;
  }
  chunk = uint32_t(chunks_.size() - 1);
  offset = chunk_used_;
  chunk_used_ += (length + 255) & ~255u;
  if (!shared_memory_.RequestRange(address, length)) return false;
  shared_memory_.UseAsCopySource();
  command_processor_.SubmitBarriers();
  command_processor_.GetDeferredCommandList().D3DCopyBufferRegion(
      chunks_[chunk].Get(), offset, shared_memory_.GetBuffer(), address, length);
  return true;
}

void Fh1FrameDump::RecordGpuRange(uint32_t address, uint32_t length) {
  if (!recording_) return;
  for (const auto& [start, size] : TakeNewPages(address, length)) {
    // Split into chunk-sized pieces.
    for (uint32_t piece = 0; piece < size; piece += kChunkSize) {
      Block block{start + piece, std::min(kChunkSize, size - piece), {}};
      if (CopyFromMirror(block.address, block.length, block.chunk, block.offset)) {
        blocks_.push_back(std::move(block));
      }
    }
  }
}

void Fh1FrameDump::RecordCopyInputs() {
  if (!recording_) return;
  const auto fetch = register_file_.GetVertexFetch(0);
  if (fetch.type == xenos::FetchConstantType::kVertex && fetch.size) {
    RecordCpuRange(fetch.address << 2, fetch.size << 2);
  }
}

void Fh1FrameDump::End(uint32_t frontbuffer_address) {
  command_processor_.SetPacketRecorder(nullptr);
  recording_ = false;
  done_ = true;
  const auto [front_address, front_length] = FrontBufferRange(register_file_, frontbuffer_address);
  Block front{front_address, front_length, {}};
  const bool have_front = front_length &&
                          CopyFromMirror(front.address, front.length, front.chunk, front.offset);
  command_processor_.Fh1AwaitAllQueueOperations();

  std::vector<uint8_t*> mapped(chunks_.size(), nullptr);
  for (size_t i = 0; i < chunks_.size(); ++i) {
    D3D12_RANGE range = {0, kChunkSize};
    void* pointer = nullptr;
    if (SUCCEEDED(chunks_[i]->Map(0, &range, &pointer))) mapped[i] = static_cast<uint8_t*>(pointer);
  }
  auto block_data = [&](const Block& block) -> const uint8_t* {
    if (block.chunk == UINT32_MAX) return block.data.data();
    return mapped[block.chunk] ? mapped[block.chunk] + block.offset : nullptr;
  };

  std::filesystem::create_directories(path_.parent_path());
  std::ofstream out(path_, std::ios::binary);
  out.write(kMagic, sizeof(kMagic));
  Put(out, frame_);
  Put(out, uint32_t(registers_.size()));
  out.write(reinterpret_cast<const char*>(registers_.data()), registers_.size() * 4);
  Put(out, uint32_t(shaders_.size()));
  for (const auto& [type, ucode] : shaders_) {
    Put(out, type);
    Put(out, uint32_t(ucode.size()));
    out.write(reinterpret_cast<const char*>(ucode.data()), ucode.size() * 4);
  }
  Put(out, uint32_t(packets_.size()));
  out.write(reinterpret_cast<const char*>(packets_.data()), packets_.size() * 4);
  uint32_t block_count = 0;
  uint64_t block_bytes = 0;
  for (const Block& block : blocks_) block_count += block_data(block) ? 1 : 0;
  Put(out, block_count);
  for (const Block& block : blocks_) {
    const uint8_t* data = block_data(block);
    if (!data) continue;
    Put(out, block.address);
    Put(out, block.length);
    out.write(reinterpret_cast<const char*>(data), block.length);
    block_bytes += block.length;
  }
  const auto fetch = register_file_.GetTextureFetch(0);
  Put(out, front.address);
  Put(out, have_front ? front.length : 0u);
  Put(out, uint32_t(fetch.size_2d.width + 1));
  Put(out, uint32_t(fetch.size_2d.height + 1));
  Put(out, uint32_t(fetch.format));
  Put(out, uint32_t(fetch.tiled));
  if (have_front) {
    if (const uint8_t* data = block_data(front)) {
      out.write(reinterpret_cast<const char*>(data), front.length);
    }
  }
  for (size_t i = 0; i < chunks_.size(); ++i) {
    if (mapped[i]) {
      D3D12_RANGE written = {0, 0};
      chunks_[i]->Unmap(0, &written);
    }
  }
  chunks_.clear();
  REXGPU_INFO(
      "FH1 frame dump: frame {} written to {} ({} packet dwords, {} blocks, {} MB, front buffer "
      "{:08X}+{})",
      frame_, path_.string(), packets_.size(), block_count, block_bytes >> 20, front.address,
      front.length);
  packets_.clear();
  blocks_.clear();
}

int Fh1FrameDump::RunRequestedReplay(D3D12CommandProcessor& command_processor,
                                      RegisterFile& register_file, memory::Memory& memory,
                                      D3D12SharedMemory& shared_memory) {
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
    packets.push_back(
        Swap((3u << 30) | ((size + 2 - 1) << 16) | (uint32_t(PM4_IM_LOAD_IMMEDIATE) << 8)));
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
  const bool executed = command_processor.ExecuteHostPackets(packets.data(), uint32_t(packets.size()));

  // Compare the front buffer the replay resolved with the recorded one.
  std::vector<uint8_t> actual(front_length);
  // The swap closed the frame's submission; the readback needs its own.
  if (front_length && command_processor.Fh1BeginSubmission() &&
      shared_memory.RequestRange(front_address, front_length)) {
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    readback.Attach(CreateReadback(command_processor.GetD3D12Provider().GetDevice(),
                                   (front_length + 0xFFFF) & ~0xFFFFu));
    if (readback) {
      shared_memory.UseAsCopySource();
      command_processor.SubmitBarriers();
      command_processor.GetDeferredCommandList().D3DCopyBufferRegion(
          readback.Get(), 0, shared_memory.GetBuffer(), front_address, front_length);
      command_processor.Fh1AwaitAllQueueOperations();
      void* mapped = nullptr;
      D3D12_RANGE range = {0, front_length};
      if (SUCCEEDED(readback->Map(0, &range, &mapped))) {
        std::memcpy(actual.data(), mapped, front_length);
        D3D12_RANGE written = {0, 0};
        readback->Unmap(0, &written);
      }
    }
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

}  // namespace rex::graphics::d3d12
