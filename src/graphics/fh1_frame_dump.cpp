#include <rex/graphics/fh1_frame_dump.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>

#include <rex/types.h>
#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(fh1_frame_dump_frame, 0, "GPU",
                     "Record this FH1 frame (executor swap number) for offline replay")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fh1_frame_dump_path, "", "GPU",
                      "File the recorded FH1 frame dump is written to")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics {

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

}  // namespace

Fh1FrameDump::Fh1FrameDump(CommandProcessor& command_processor,
                           const RegisterFile& register_file, memory::Memory& memory,
                           std::unique_ptr<Fh1FrameDumpMirror> mirror, uint64_t frame,
                           std::filesystem::path path)
    : command_processor_(command_processor),
      register_file_(register_file),
      memory_(memory),
      mirror_(std::move(mirror)),
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
    if (!mirror_->CreateChunk(kChunkSize)) return false;
    ++chunk_count_;
    chunk_used_ = 0;
  }
  chunk = chunk_count_ - 1;
  offset = chunk_used_;
  chunk_used_ += (length + 255) & ~255u;
  return mirror_->Copy(chunk, offset, address, length);
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
  const std::vector<const uint8_t*> mapped = mirror_->AwaitAndMap();
  auto block_data = [&](const Block& block) -> const uint8_t* {
    if (block.chunk == UINT32_MAX) return block.data.data();
    return block.chunk < mapped.size() && mapped[block.chunk] ? mapped[block.chunk] + block.offset
                                                               : nullptr;
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
  mirror_->Release();
  chunk_count_ = 0;
  chunk_used_ = kChunkSize;
  REXGPU_INFO(
      "FH1 frame dump: frame {} written to {} ({} packet dwords, {} blocks, {} MB, front buffer "
      "{:08X}+{})",
      frame_, path_.string(), packets_.size(), block_count, block_bytes >> 20, front.address,
      front.length);
  packets_.clear();
  blocks_.clear();
}

void Fh1FrameDump::RecordDrawInputs(uint32_t used_texture_mask, const Shader& vertex_shader,
                                    uint32_t guest_dma_index_offset,
                                    uint32_t guest_dma_index_size) {
  if (!recording_) return;
  for (const Shader::VertexBinding& binding : vertex_shader.vertex_bindings()) {
    const xenos::xe_gpu_vertex_fetch_t fetch =
        register_file_.GetVertexFetch(binding.fetch_constant);
    if (fetch.type != xenos::FetchConstantType::kVertex || !fetch.size) continue;
    RecordGpuRange(fetch.address << 2, fetch.size << 2);
  }
  if (guest_dma_index_size) {
    RecordGpuRange(guest_dma_index_offset, guest_dma_index_size);
  }
  // Every level the fetch can reach, as the texture cache just loaded it.
  for (uint32_t mask = used_texture_mask; mask; mask &= mask - 1) {
    const auto fetch = register_file_.GetTextureFetch(uint32_t(std::countr_zero(mask)));
    uint32_t width = 1, height = 1, depth = 1;
    switch (fetch.dimension) {
      case xenos::DataDimension::k1D:
        width = fetch.size_1d.width + 1;
        break;
      case xenos::DataDimension::k3D:
        width = fetch.size_3d.width + 1;
        height = fetch.size_3d.height + 1;
        depth = fetch.size_3d.depth + 1;
        break;
      default:
        width = fetch.size_2d.width + 1;
        height = fetch.size_2d.height + 1;
        depth = fetch.size_2d.stack_depth + 1;
        break;
    }
    const auto layout = texture_util::GetGuestTextureLayout(
        fetch.dimension, fetch.pitch, width, height, depth, fetch.tiled, fetch.format,
        fetch.packed_mips, true, fetch.mip_max_level);
    const uint32_t base_size = layout.base.level_data_extent_bytes;
    const uint32_t mip_size = layout.mips_total_extent_bytes;
    if (fetch.base_address && base_size) {
      RecordGpuRange(fetch.base_address << 12, base_size);
    }
    if (fetch.mip_address && mip_size) {
      RecordGpuRange(fetch.mip_address << 12, mip_size);
    }
  }
}

}  // namespace rex::graphics
