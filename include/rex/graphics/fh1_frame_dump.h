#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <rex/graphics/register_file.h>
#include <rex/memory.h>

namespace rex::graphics {

class CommandProcessor;
class Shader;

// Where a frame dump puts the guest ranges as the GPU mirror holds them: the
// backend copies them on the GPU into host-readable chunks, read back once
// the frame ends.
class Fh1FrameDumpMirror {
 public:
  virtual ~Fh1FrameDumpMirror() = default;
  // Adds a host-readable chunk of `size` bytes.
  virtual bool CreateChunk(uint32_t size) = 0;
  // Queues a copy of `length` guest bytes at `address` into `chunk` at
  // `offset` (256-byte aligned).
  virtual bool Copy(uint32_t chunk, uint32_t offset, uint32_t address, uint32_t length) = 0;
  // Waits for the copies and maps every chunk (nullptr for one that failed).
  virtual std::vector<const uint8_t*> AwaitAndMap() = 0;
  // Unmaps and frees the chunks.
  virtual void Release() = 0;
};

// FH1 frame dumps for offline executor regression tests.
//
// Recording (fh1_frame_dump_frame, fh1_frame_dump_path): from the swap before
// the requested frame to its own swap, every executed packet is kept with
// indirect buffers flattened, together with the registers and active shaders
// at the start, and every guest range the frame reads - shader and constant
// loads from guest memory as the CPU holds them, vertex, index and texture
// ranges as the GPU mirror holds them when the draw reads them. The front
// buffer as the mirror holds it after the swap is the expected output.
//
// Replay (fh1_frame_replay, RunFh1FrameReplay): with no title running, the
// ranges are written to guest memory, the registers and shaders restored and
// the packets executed; the resulting front buffer is compared with the
// recorded one and written next to the dump.
class Fh1FrameDump {
 public:
  Fh1FrameDump(CommandProcessor& command_processor, const RegisterFile& register_file,
               memory::Memory& memory, std::unique_ptr<Fh1FrameDumpMirror> mirror,
               uint64_t frame, std::filesystem::path path);
  ~Fh1FrameDump();

  // Frame requested by fh1_frame_dump_frame, 0 when none.
  static uint64_t RequestedFrame();
  static std::filesystem::path RequestedPath();

  bool recording() const { return recording_; }
  // At every swap, `frame` being the swap's frame.
  void OnSwap(uint64_t frame, uint32_t frontbuffer_address);
  // A range a draw is about to read from the GPU mirror (already requested).
  void RecordGpuRange(uint32_t address, uint32_t length);
  // A draw's vertex buffers, guest DMA indices and every texture level it
  // can reach, after the texture cache has requested them.
  void RecordDrawInputs(uint32_t used_texture_mask, const Shader& vertex_shader,
                        uint32_t guest_dma_index_offset, uint32_t guest_dma_index_size);
  // A copy's rectangle comes from vertex fetch 0, which the CPU reads.
  void RecordCopyInputs();

 private:
  struct Block {
    uint32_t address;
    uint32_t length;
    std::vector<uint8_t> data;  // CPU ranges
    uint32_t chunk = UINT32_MAX;  // GPU ranges: readback chunk and offset
    uint32_t offset = 0;
  };
  static constexpr uint32_t kPageSizeLog2 = 12;
  static constexpr uint32_t kChunkSize = 64u << 20;

  void Begin();
  void End(uint32_t frontbuffer_address);
  void OnPacket(const uint32_t* dwords, uint32_t count);
  void RecordCpuRange(uint32_t address, uint32_t length);
  // Page-aligned runs of `address, length` not recorded yet; marks them.
  std::vector<std::pair<uint32_t, uint32_t>> TakeNewPages(uint32_t address, uint32_t length);
  bool CopyFromMirror(uint32_t address, uint32_t length, uint32_t& chunk, uint32_t& offset);

  CommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  memory::Memory& memory_;
  std::unique_ptr<Fh1FrameDumpMirror> mirror_;
  uint64_t frame_;
  std::filesystem::path path_;
  bool recording_ = false;
  bool done_ = false;

  std::vector<uint32_t> registers_;
  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> shaders_;  // type, host ucode
  std::vector<uint32_t> packets_;
  std::vector<Block> blocks_;
  std::vector<uint8_t> recorded_pages_;
  uint32_t chunk_count_ = 0;
  uint32_t chunk_used_ = kChunkSize;
};

}  // namespace rex::graphics
