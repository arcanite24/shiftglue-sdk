#include <rex/graphics/d3d12/fh1_frame_dump.h>

#include <cstring>

#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/shared_memory.h>
#include <rex/graphics/fh1_frame_replay.h>

namespace rex::graphics::d3d12 {

namespace {

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

bool D3D12Fh1FrameDumpMirror::CreateChunk(uint32_t size) {
  ID3D12Resource* resource =
      CreateReadback(command_processor_.GetD3D12Provider().GetDevice(), size);
  if (!resource) return false;
  chunks_.emplace_back().Attach(resource);
  mapped_.push_back(false);
  return true;
}

bool D3D12Fh1FrameDumpMirror::Copy(uint32_t chunk, uint32_t offset, uint32_t address,
                                   uint32_t length) {
  if (chunk >= chunks_.size() || !shared_memory_.RequestRange(address, length)) return false;
  shared_memory_.UseAsCopySource();
  command_processor_.SubmitBarriers();
  command_processor_.GetDeferredCommandList().D3DCopyBufferRegion(
      chunks_[chunk].Get(), offset, shared_memory_.GetBuffer(), address, length);
  return true;
}

std::vector<const uint8_t*> D3D12Fh1FrameDumpMirror::AwaitAndMap() {
  command_processor_.Fh1AwaitAllQueueOperations();
  std::vector<const uint8_t*> mapped(chunks_.size(), nullptr);
  for (size_t i = 0; i < chunks_.size(); ++i) {
    const D3D12_RESOURCE_DESC desc = chunks_[i]->GetDesc();
    D3D12_RANGE range = {0, SIZE_T(desc.Width)};
    void* pointer = nullptr;
    if (SUCCEEDED(chunks_[i]->Map(0, &range, &pointer))) {
      mapped[i] = static_cast<const uint8_t*>(pointer);
      mapped_[i] = true;
    }
  }
  return mapped;
}

void D3D12Fh1FrameDumpMirror::Release() {
  for (size_t i = 0; i < chunks_.size(); ++i) {
    if (mapped_[i]) {
      D3D12_RANGE written = {0, 0};
      chunks_[i]->Unmap(0, &written);
    }
  }
  chunks_.clear();
  mapped_.clear();
}

int RunRequestedFh1FrameReplay(D3D12CommandProcessor& command_processor,
                               RegisterFile& register_file, memory::Memory& memory,
                               D3D12SharedMemory& shared_memory) {
  return RunFh1FrameReplay(
      command_processor, register_file, memory, shared_memory,
      [&](uint32_t address, uint32_t length, std::vector<uint8_t>& out) {
        // The swap closed the frame's submission; the readback needs its own.
        if (!command_processor.Fh1BeginSubmission() ||
            !shared_memory.RequestRange(address, length)) {
          return false;
        }
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        readback.Attach(CreateReadback(command_processor.GetD3D12Provider().GetDevice(),
                                       (length + 0xFFFF) & ~0xFFFFu));
        if (!readback) return false;
        shared_memory.UseAsCopySource();
        command_processor.SubmitBarriers();
        command_processor.GetDeferredCommandList().D3DCopyBufferRegion(
            readback.Get(), 0, shared_memory.GetBuffer(), address, length);
        command_processor.Fh1AwaitAllQueueOperations();
        void* mapped = nullptr;
        D3D12_RANGE range = {0, length};
        if (FAILED(readback->Map(0, &range, &mapped))) return false;
        std::memcpy(out.data(), mapped, length);
        D3D12_RANGE written = {0, 0};
        readback->Unmap(0, &written);
        return true;
      });
}

}  // namespace rex::graphics::d3d12
