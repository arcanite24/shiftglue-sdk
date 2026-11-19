/**
 * @file        graphics/d3d11/primitive_processor.cpp
 * @brief       Direct3D 11 primitive processor (index buffer conversion).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/primitive_processor.h>

#include <algorithm>
#include <cstring>

#include <rex/assert.h>
#include <rex/graphics/d3d11/command_processor.h>
#include <rex/logging.h>
#include <rex/math.h>

namespace rex::graphics::d3d11 {

D3D11PrimitiveProcessor::~D3D11PrimitiveProcessor() { Shutdown(true); }

bool D3D11PrimitiveProcessor::Initialize() {
  if (!InitializeCommon(true, false, false, true, true, true)) {
    Shutdown();
    return false;
  }
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = kRingBufferSize;
  desc.Usage = D3D11_USAGE_DYNAMIC;
  desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(command_processor_.GetDevice()->CreateBuffer(&desc, nullptr, &ring_buffer_))) {
    REXGPU_ERROR("D3D11 primitive processor: Failed to create the converted index ring buffer");
    Shutdown();
    return false;
  }
  // The first map discards.
  ring_buffer_position_ = kRingBufferSize;
  return true;
}

void D3D11PrimitiveProcessor::Shutdown(bool from_destructor) {
  frame_index_buffers_.clear();
  ring_buffer_.Reset();
  builtin_index_buffer_.Reset();
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void D3D11PrimitiveProcessor::EndFrame() {
  ClearPerFrameCache();
  frame_index_buffers_.clear();
}

bool D3D11PrimitiveProcessor::InitializeBuiltinIndexBuffer(
    size_t size_bytes, std::function<void(void*)> fill_callback) {
  assert_not_zero(size_bytes);
  assert_null(builtin_index_buffer_);
  std::vector<uint8_t> data(size_bytes);
  fill_callback(data.data());
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = UINT(size_bytes);
  desc.Usage = D3D11_USAGE_IMMUTABLE;
  desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
  D3D11_SUBRESOURCE_DATA initial_data = {};
  initial_data.pSysMem = data.data();
  if (FAILED(command_processor_.GetDevice()->CreateBuffer(&desc, &initial_data,
                                                          &builtin_index_buffer_))) {
    REXGPU_ERROR(
        "D3D11 primitive processor: Failed to create the built-in index buffer with {} bytes",
        size_bytes);
    return false;
  }
  return true;
}

void* D3D11PrimitiveProcessor::RequestHostConvertedIndexBufferForCurrentFrame(
    xenos::IndexFormat format, uint32_t index_count, bool coalign_for_simd,
    uint32_t coalignment_original_address, size_t& backend_handle_out) {
  uint32_t index_size = format == xenos::IndexFormat::kInt16 ? sizeof(uint16_t) : sizeof(uint32_t);
  ConvertedIndexBuffer& converted = frame_index_buffers_.emplace_back();
  converted.size = index_size * index_count;
  converted.data.resize(converted.size +
                        (coalign_for_simd ? XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE : 0));
  uint8_t* mapping = converted.data.data();
  if (coalign_for_simd) {
    ptrdiff_t coalignment_offset = GetSimdCoalignmentOffset(mapping, coalignment_original_address);
    converted.data_offset = uint32_t(coalignment_offset);
    mapping += coalignment_offset;
  }
  backend_handle_out = frame_index_buffers_.size() - 1;
  return mapping;
}

bool D3D11PrimitiveProcessor::UploadConvertedIndexBuffer(size_t handle,
                                                         ID3D11Buffer*& buffer_out,
                                                         uint32_t& offset_bytes_out) {
  if (handle >= frame_index_buffers_.size()) {
    return false;
  }
  const ConvertedIndexBuffer& converted = frame_index_buffers_[handle];
  if (!converted.size || converted.size > kRingBufferSize) {
    return false;
  }
  ID3D11DeviceContext* context = command_processor_.GetContext();
  // Index buffer offsets must be aligned to the index size; keep 16 bytes.
  uint32_t offset = rex::align(ring_buffer_position_, uint32_t(16));
  D3D11_MAP map_type = D3D11_MAP_WRITE_NO_OVERWRITE;
  if (offset > kRingBufferSize || kRingBufferSize - offset < converted.size) {
    offset = 0;
    map_type = D3D11_MAP_WRITE_DISCARD;
  }
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(context->Map(ring_buffer_.Get(), 0, map_type, 0, &mapped))) {
    REXGPU_ERROR("D3D11 primitive processor: Failed to map the converted index ring buffer");
    return false;
  }
  std::memcpy(static_cast<uint8_t*>(mapped.pData) + offset,
              converted.data.data() + converted.data_offset, converted.size);
  context->Unmap(ring_buffer_.Get(), 0);
  ring_buffer_position_ = offset + converted.size;
  buffer_out = ring_buffer_.Get();
  offset_bytes_out = offset;
  return true;
}

}  // namespace rex::graphics::d3d11
