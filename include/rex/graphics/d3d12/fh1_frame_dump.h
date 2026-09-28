#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <rex/graphics/register_file.h>
#include <rex/memory.h>
#include <rex/ui/d3d12/d3d12_api.h>

namespace rex::graphics::d3d12 {

class D3D12CommandProcessor;
class D3D12SharedMemory;

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
// Replay (fh1_frame_replay): with no title running, the ranges are written to
// guest memory, the registers and shaders restored and the packets executed;
// the resulting front buffer is compared with the recorded one and written
// next to the dump.
class Fh1FrameDump {
 public:
  Fh1FrameDump(D3D12CommandProcessor& command_processor, const RegisterFile& register_file,
               memory::Memory& memory, D3D12SharedMemory& shared_memory, uint64_t frame,
               std::filesystem::path path);
  ~Fh1FrameDump();

  // Frame requested by fh1_frame_dump_frame, 0 when none.
  static uint64_t RequestedFrame();
  static std::filesystem::path RequestedPath();

  bool recording() const { return recording_; }
  // After every swap, `frame` being the swap just completed.
  void OnSwap(uint64_t frame, uint32_t frontbuffer_address);
  // A range a draw is about to read from the GPU mirror (already requested).
  void RecordGpuRange(uint32_t address, uint32_t length);
  // A copy's rectangle comes from vertex fetch 0, which the CPU reads.
  void RecordCopyInputs();

  // Runs fh1_frame_replay when set, on the command processor thread. Returns
  // the process exit code, or -1 when no replay was requested.
  static int RunRequestedReplay(D3D12CommandProcessor& command_processor,
                                 RegisterFile& register_file, memory::Memory& memory,
                                 D3D12SharedMemory& shared_memory);

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

  D3D12CommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  memory::Memory& memory_;
  D3D12SharedMemory& shared_memory_;
  uint64_t frame_;
  std::filesystem::path path_;
  bool recording_ = false;
  bool done_ = false;

  std::vector<uint32_t> registers_;
  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> shaders_;  // type, host ucode
  std::vector<uint32_t> packets_;
  std::vector<Block> blocks_;
  std::vector<uint8_t> recorded_pages_;
  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> chunks_;
  uint32_t chunk_used_ = kChunkSize;
};

}  // namespace rex::graphics::d3d12
