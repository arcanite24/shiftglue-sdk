/**
 * @file        graphics/d3d11/shared_memory.cpp
 * @brief       Direct3D 11 guest physical memory mirror.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/shared_memory.h>

#include <algorithm>

#include <rex/graphics/d3d11/command_processor.h>
#include <rex/logging.h>

namespace rex::graphics::d3d11 {

D3D11SharedMemory::D3D11SharedMemory(D3D11CommandProcessor& command_processor,
                                     memory::Memory& memory)
    : SharedMemory(memory), command_processor_(command_processor) {}

D3D11SharedMemory::~D3D11SharedMemory() { Shutdown(true); }

bool D3D11SharedMemory::Initialize() {
  InitializeCommon();

  ID3D11Device* device = command_processor_.GetDevice();

  D3D11_BUFFER_DESC buffer_desc = {};
  buffer_desc.ByteWidth = kBufferSize;
  buffer_desc.Usage = D3D11_USAGE_DEFAULT;
  // Guest DMA index buffers are read from the mirror directly.
  buffer_desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_INDEX_BUFFER;
  buffer_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  if (FAILED(device->CreateBuffer(&buffer_desc, nullptr, &buffer_))) {
    // A single resource may be at most max(128 MB, a quarter of the dedicated
    // video memory), so the full mirror needs at least 2 GB of it.
    REXGPU_ERROR(
        "Shared memory: Failed to create the {} MB buffer (Direct3D 11 limits one "
        "resource to a quarter of the video memory, so this needs 2 GB of it)",
        kBufferSize >> 20);
    Shutdown();
    return false;
  }

  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  srv_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
  srv_desc.BufferEx.FirstElement = 0;
  srv_desc.BufferEx.NumElements = kBufferSize >> 2;
  srv_desc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
  if (FAILED(device->CreateShaderResourceView(buffer_.Get(), &srv_desc, &raw_srv_))) {
    REXGPU_ERROR("Shared memory: Failed to create the raw SRV");
    Shutdown();
    return false;
  }
  D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
  uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uav_desc.Buffer.FirstElement = 0;
  uav_desc.Buffer.NumElements = kBufferSize >> 2;
  uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  if (FAILED(device->CreateUnorderedAccessView(buffer_.Get(), &uav_desc, &raw_uav_))) {
    REXGPU_ERROR("Shared memory: Failed to create the raw UAV");
    Shutdown();
    return false;
  }
  static constexpr DXGI_FORMAT kUintFormats[] = {
      DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32B32A32_UINT};
  for (uint32_t i = 0; i < 3; ++i) {
    D3D11_SHADER_RESOURCE_VIEW_DESC typed_srv_desc = {};
    typed_srv_desc.Format = kUintFormats[i];
    typed_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    typed_srv_desc.Buffer.FirstElement = 0;
    typed_srv_desc.Buffer.NumElements = kBufferSize >> (2 + i);
    if (FAILED(device->CreateShaderResourceView(buffer_.Get(), &typed_srv_desc, &uint_srvs_[i]))) {
      REXGPU_ERROR("Shared memory: Failed to create a typed SRV");
      Shutdown();
      return false;
    }
    D3D11_UNORDERED_ACCESS_VIEW_DESC typed_uav_desc = {};
    typed_uav_desc.Format = kUintFormats[i];
    typed_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    typed_uav_desc.Buffer.FirstElement = 0;
    typed_uav_desc.Buffer.NumElements = kBufferSize >> (2 + i);
    if (FAILED(device->CreateUnorderedAccessView(buffer_.Get(), &typed_uav_desc, &uint_uavs_[i]))) {
      REXGPU_ERROR("Shared memory: Failed to create a typed UAV");
      Shutdown();
      return false;
    }
  }
  return true;
}

void D3D11SharedMemory::Shutdown(bool from_destructor) {
  for (auto& view : uint_uavs_) {
    view.Reset();
  }
  for (auto& view : uint_srvs_) {
    view.Reset();
  }
  raw_uav_.Reset();
  raw_srv_.Reset();
  buffer_.Reset();
  // From the destructor, the SharedMemory destructor calls ShutdownCommon.
  if (!from_destructor) {
    ShutdownCommon();
  }
}

ID3D11ShaderResourceView* D3D11SharedMemory::GetUintPow2SRV(
    uint32_t element_size_bytes_log2) const {
  if (element_size_bytes_log2 < 2 || element_size_bytes_log2 > 4) {
    assert_unhandled_case(element_size_bytes_log2);
    return nullptr;
  }
  return uint_srvs_[element_size_bytes_log2 - 2].Get();
}

ID3D11UnorderedAccessView* D3D11SharedMemory::GetUintPow2UAV(
    uint32_t element_size_bytes_log2) const {
  if (element_size_bytes_log2 < 2 || element_size_bytes_log2 > 4) {
    assert_unhandled_case(element_size_bytes_log2);
    return nullptr;
  }
  return uint_uavs_[element_size_bytes_log2 - 2].Get();
}

bool D3D11SharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                         uint32_t length_allocations) {
  // The whole buffer is committed at creation.
  return true;
}

bool D3D11SharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
  ID3D11DeviceContext* context = command_processor_.GetContext();
  uint32_t page_size_log2_value = page_size_log2();
  for (auto upload_range : upload_page_ranges) {
    uint32_t start = upload_range.first << page_size_log2_value;
    uint32_t length = upload_range.second << page_size_log2_value;
    MakeRangeValid(start, length, false);
    // UpdateSubresource copies the data before returning, and orders the copy
    // after the work already submitted to the context.
    D3D11_BOX box = {};
    box.left = start;
    box.right = start + length;
    box.top = 0;
    box.bottom = 1;
    box.front = 0;
    box.back = 1;
    context->UpdateSubresource(buffer_.Get(), 0, &box, memory().TranslatePhysical(start), 0, 0);
  }
  return true;
}

}  // namespace rex::graphics::d3d11
