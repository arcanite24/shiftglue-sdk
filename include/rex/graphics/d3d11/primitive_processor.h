/**
 * @file        graphics/d3d11/primitive_processor.h
 * @brief       Direct3D 11 primitive processor (index buffer conversion).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include <rex/graphics/primitive_processor.h>
#include <rex/ui/d3d11/d3d11_api.h>

namespace rex::graphics::d3d11 {

class D3D11CommandProcessor;

// Feature level 11_0 has full 32-bit indices, and no triangle fans or line
// loops; point sprites, rectangle lists and quad lists are expanded by
// geometry shaders, as on Direct3D 12.
//
// Converted index buffers are written by the base class into CPU memory, and
// uploaded into a dynamic ring buffer (Map with NO_OVERWRITE, DISCARD on wrap)
// when the command processor binds them.
class D3D11PrimitiveProcessor final : public PrimitiveProcessor {
 public:
  D3D11PrimitiveProcessor(const RegisterFile& register_file, memory::Memory& memory,
                          SharedMemory& shared_memory, D3D11CommandProcessor& command_processor)
      : PrimitiveProcessor(register_file, memory, shared_memory),
        command_processor_(command_processor) {}
  ~D3D11PrimitiveProcessor();

  bool Initialize();
  void Shutdown(bool from_destructor = false);
  void ClearCache() {}

  void BeginFrame() {}
  void EndFrame();

  ID3D11Buffer* GetBuiltinIndexBuffer() const { return builtin_index_buffer_.Get(); }
  // Byte offset of a built-in index buffer handle in GetBuiltinIndexBuffer().
  static uint32_t GetBuiltinIndexBufferOffset(size_t handle) {
    return uint32_t(GetBuiltinIndexBufferOffsetBytes(handle));
  }
  // Uploads a converted index buffer of the current frame into the ring
  // buffer and returns where it is. Returns false on failure.
  bool UploadConvertedIndexBuffer(size_t handle, ID3D11Buffer*& buffer_out,
                                  uint32_t& offset_bytes_out);

 protected:
  bool InitializeBuiltinIndexBuffer(size_t size_bytes,
                                    std::function<void(void*)> fill_callback) override;

  void* RequestHostConvertedIndexBufferForCurrentFrame(xenos::IndexFormat format,
                                                       uint32_t index_count, bool coalign_for_simd,
                                                       uint32_t coalignment_original_address,
                                                       size_t& backend_handle_out) override;

 private:
  static constexpr uint32_t kRingBufferSize = 16 * 1024 * 1024;

  D3D11CommandProcessor& command_processor_;

  Microsoft::WRL::ComPtr<ID3D11Buffer> builtin_index_buffer_;

  struct ConvertedIndexBuffer {
    std::vector<uint8_t> data;
    // Offset of the indices in data (SIMD coalignment).
    uint32_t data_offset = 0;
    uint32_t size = 0;
  };
  // Indexed by the backend handles.
  std::deque<ConvertedIndexBuffer> frame_index_buffers_;

  Microsoft::WRL::ComPtr<ID3D11Buffer> ring_buffer_;
  uint32_t ring_buffer_position_ = 0;
};

}  // namespace rex::graphics::d3d11
