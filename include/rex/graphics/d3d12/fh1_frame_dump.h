#pragma once

#include <cstdint>
#include <vector>

#include <rex/graphics/fh1_frame_dump.h>
#include <rex/graphics/register_file.h>
#include <rex/memory.h>
#include <rex/ui/d3d12/d3d12_api.h>

namespace rex::graphics::d3d12 {

class D3D12CommandProcessor;
class D3D12SharedMemory;

// Frame dump chunks as readback buffers filled by copies from the shared
// memory buffer.
class D3D12Fh1FrameDumpMirror : public Fh1FrameDumpMirror {
 public:
  D3D12Fh1FrameDumpMirror(D3D12CommandProcessor& command_processor,
                          D3D12SharedMemory& shared_memory)
      : command_processor_(command_processor), shared_memory_(shared_memory) {}
  ~D3D12Fh1FrameDumpMirror() override { Release(); }

  bool CreateChunk(uint32_t size) override;
  bool Copy(uint32_t chunk, uint32_t offset, uint32_t address, uint32_t length) override;
  std::vector<const uint8_t*> AwaitAndMap() override;
  void Release() override;

 private:
  D3D12CommandProcessor& command_processor_;
  D3D12SharedMemory& shared_memory_;
  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> chunks_;
  std::vector<bool> mapped_;
};

// Runs fh1_frame_replay when set, on the command processor thread. Returns
// the process exit code, or -1 when no replay was requested.
int RunRequestedFh1FrameReplay(D3D12CommandProcessor& command_processor,
                               RegisterFile& register_file, memory::Memory& memory,
                               D3D12SharedMemory& shared_memory);

}  // namespace rex::graphics::d3d12
