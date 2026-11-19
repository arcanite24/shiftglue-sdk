/**
 * @file        graphics/d3d11/shared_memory.h
 * @brief       Direct3D 11 guest physical memory mirror.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <utility>
#include <vector>

#include <rex/graphics/shared_memory.h>
#include <rex/memory.h>
#include <rex/ui/d3d11/d3d11_api.h>

namespace rex::graphics::d3d11 {

class D3D11CommandProcessor;

// The 512 MB guest physical memory as one ordinary DEFAULT buffer (feature
// level 11_0 has no tiled resources). Guest CPU writes are uploaded with
// UpdateSubresource as the base class requests page ranges; resolves write it
// on the GPU through the raw or typed UAVs, so it is the only up-to-date copy
// of resolve output, which texture loads and vertex fetch read.
//
// D3D11 hazard tracking: binding the buffer as a UAV unbinds its SRVs and the
// index buffer, and the debug layer warns. Users that bind a UAV of it must
// unbind it after their dispatch or draw (D3D11CommandProcessor helpers).
class D3D11SharedMemory : public SharedMemory {
 public:
  D3D11SharedMemory(D3D11CommandProcessor& command_processor, memory::Memory& memory);
  ~D3D11SharedMemory() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  ID3D11Buffer* GetBuffer() const { return buffer_.Get(); }

  // ByteAddressBuffer / RWByteAddressBuffer views of the whole buffer.
  ID3D11ShaderResourceView* GetRawSRV() const { return raw_srv_.Get(); }
  ID3D11UnorderedAccessView* GetRawUAV() const { return raw_uav_.Get(); }
  // R32_UINT, R32G32_UINT or R32G32B32A32_UINT views for element size log2 2,
  // 3 or 4 (the smallest typed elements are 32-bit because of the 2^27 element
  // limit).
  ID3D11ShaderResourceView* GetUintPow2SRV(uint32_t element_size_bytes_log2) const;
  ID3D11UnorderedAccessView* GetUintPow2UAV(uint32_t element_size_bytes_log2) const;

 protected:
  bool AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                        uint32_t length_allocations) override;
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;

 private:
  D3D11CommandProcessor& command_processor_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> buffer_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> raw_srv_;
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> raw_uav_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> uint_srvs_[3];
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uint_uavs_[3];
};

}  // namespace rex::graphics::d3d11
