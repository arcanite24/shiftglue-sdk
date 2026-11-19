// Direct3D 11 texture cache: copies the linear output of a texture load
// shader into a texture subresource through a typed *_UINT UAV.
//
// Direct3D 11 has no GPU copy from a buffer into a texture, so the texture
// cache's load shaders write the same linear host layout as on Direct3D 12
// (rows of host elements at a row pitch, then depth slices or array layers at
// a slice pitch) into a scratch buffer, and this shader stores the elements
// into the texture. Each element is split into the components of the UAV's
// format (the bits of the host format laid out as in memory), so the store
// reproduces the bytes exactly. Block-compressed textures are written as
// R32G32_UINT or R32G32B32A32_UINT block textures and copied into the BC
// texture with CopySubresourceRegion by the texture cache.
//
// The 16-bit packed host formats of Direct3D 12 (B5G6R5, B5G5R5A1, B4G4R4A4)
// have no typed UAV store at feature level 11_0, so their textures are
// R8G8B8A8, and the copy expands each 16-bit element to 8 bits per component
// by bit replication, in R, G, B, A order (alpha 255 for B5G6R5). Replication
// maps 0 and the maximum exactly and 4-bit values exactly; 5- and 6-bit values
// in between differ from x / max * 255 by less than one 8-bit step (at most
// 0.72 of a step, measured over all 65536 inputs).
//
// t0: the scratch buffer (raw).
// u0: the destination, RWTexture2DArray (2D, stacked, cube faces) or
//     RWTexture3D (XE_TEXTURE_COPY_3D), of the mip level being written.
// b0: XeTextureCopyConstants.
//
// Defines: XE_TEXTURE_COPY_3D for the 3D variant.

cbuffer XeTextureCopyConstants : register(b0) {
  // Byte offset of the level's first slice in the scratch buffer.
  uint xe_texture_copy_source_offset;
  // Bytes between rows of elements.
  uint xe_texture_copy_source_row_pitch;
  // Bytes between depth slices (3D) or array layers.
  uint xe_texture_copy_source_slice_pitch;
  // XE_TEXTURE_COPY_MODE_*.
  uint xe_texture_copy_mode;
  // First element, row and slice to read (a level inside a packed mip tail).
  uint3 xe_texture_copy_source_origin;
  uint xe_texture_copy_padding_0;
  // Elements, rows and slices to write, from (0, 0, 0) of the destination.
  uint3 xe_texture_copy_size;
  uint xe_texture_copy_padding_1;
};

ByteAddressBuffer xe_texture_copy_source : register(t0);
#ifdef XE_TEXTURE_COPY_3D
RWTexture3D<uint4> xe_texture_copy_dest : register(u0);
#else
RWTexture2DArray<uint4> xe_texture_copy_dest : register(u0);
#endif

// Element layouts; the texture cache uses the same values.
#define XE_TEXTURE_COPY_MODE_R8 0u
#define XE_TEXTURE_COPY_MODE_R8G8 1u
#define XE_TEXTURE_COPY_MODE_R16 2u
#define XE_TEXTURE_COPY_MODE_R8G8B8A8 3u
#define XE_TEXTURE_COPY_MODE_R16G16 4u
#define XE_TEXTURE_COPY_MODE_R32 5u
#define XE_TEXTURE_COPY_MODE_R10G10B10A2 6u
#define XE_TEXTURE_COPY_MODE_R16G16B16A16 7u
#define XE_TEXTURE_COPY_MODE_R32G32 8u
#define XE_TEXTURE_COPY_MODE_R32G32B32A32 9u
#define XE_TEXTURE_COPY_MODE_B5G6R5_TO_RGBA8 10u
#define XE_TEXTURE_COPY_MODE_B5G5R5A1_TO_RGBA8 11u
#define XE_TEXTURE_COPY_MODE_B4G4R4A4_TO_RGBA8 12u

uint XeTextureCopyLoad8(uint address) {
  return (xe_texture_copy_source.Load(address & ~3u) >> ((address & 3u) << 3u)) & 0xFFu;
}

uint XeTextureCopyLoad16(uint address) {
  return (xe_texture_copy_source.Load(address & ~3u) >> ((address & 2u) << 3u)) & 0xFFFFu;
}

// Replicates the high bits of a 4 to 6 bit unorm value into an 8-bit one.
uint XeTextureCopyExpandTo8(uint value, uint bits) {
  return (value << (8u - bits)) | (value >> (bits * 2u - 8u));
}

[numthreads(8, 8, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  [branch] if (any(xe_thread_id >= xe_texture_copy_size)) {
    return;
  }
  uint3 source = xe_thread_id + xe_texture_copy_source_origin;
  uint row_address = xe_texture_copy_source_offset +
                     source.z * xe_texture_copy_source_slice_pitch +
                     source.y * xe_texture_copy_source_row_pitch;
  uint4 value = uint4(0u, 0u, 0u, 0u);
  uint packed;
  [branch] switch (xe_texture_copy_mode) {
    case XE_TEXTURE_COPY_MODE_R8:
      value.x = XeTextureCopyLoad8(row_address + source.x);
      break;
    case XE_TEXTURE_COPY_MODE_R8G8:
      packed = XeTextureCopyLoad16(row_address + (source.x << 1u));
      value.xy = (packed.xx >> uint2(0u, 8u)) & 0xFFu;
      break;
    case XE_TEXTURE_COPY_MODE_R16:
      value.x = XeTextureCopyLoad16(row_address + (source.x << 1u));
      break;
    case XE_TEXTURE_COPY_MODE_R8G8B8A8:
      value = (xe_texture_copy_source.Load(row_address + (source.x << 2u)).xxxx >>
               uint4(0u, 8u, 16u, 24u)) &
              0xFFu;
      break;
    case XE_TEXTURE_COPY_MODE_R16G16:
      value.xy = (xe_texture_copy_source.Load(row_address + (source.x << 2u)).xx >>
                  uint2(0u, 16u)) &
                 0xFFFFu;
      break;
    case XE_TEXTURE_COPY_MODE_R32:
      value.x = xe_texture_copy_source.Load(row_address + (source.x << 2u));
      break;
    case XE_TEXTURE_COPY_MODE_R10G10B10A2:
      value = (xe_texture_copy_source.Load(row_address + (source.x << 2u)).xxxx >>
               uint4(0u, 10u, 20u, 30u)) &
              uint4(0x3FFu, 0x3FFu, 0x3FFu, 0x3u);
      break;
    case XE_TEXTURE_COPY_MODE_R16G16B16A16:
      value = (xe_texture_copy_source.Load2(row_address + (source.x << 3u)).xxyy >>
               uint4(0u, 16u, 0u, 16u)) &
              0xFFFFu;
      break;
    case XE_TEXTURE_COPY_MODE_R32G32:
      value.xy = xe_texture_copy_source.Load2(row_address + (source.x << 3u));
      break;
    case XE_TEXTURE_COPY_MODE_R32G32B32A32:
      value = xe_texture_copy_source.Load4(row_address + (source.x << 4u));
      break;
    case XE_TEXTURE_COPY_MODE_B5G6R5_TO_RGBA8:
      // R in bits 11:15, G in 5:10, B in 0:4.
      packed = XeTextureCopyLoad16(row_address + (source.x << 1u));
      value.x = XeTextureCopyExpandTo8(packed >> 11u, 5u);
      value.y = XeTextureCopyExpandTo8((packed >> 5u) & 0x3Fu, 6u);
      value.z = XeTextureCopyExpandTo8(packed & 0x1Fu, 5u);
      value.w = 0xFFu;
      break;
    case XE_TEXTURE_COPY_MODE_B5G5R5A1_TO_RGBA8:
      // R in bits 10:14, G in 5:9, B in 0:4, A in 15.
      packed = XeTextureCopyLoad16(row_address + (source.x << 1u));
      value.x = XeTextureCopyExpandTo8((packed >> 10u) & 0x1Fu, 5u);
      value.y = XeTextureCopyExpandTo8((packed >> 5u) & 0x1Fu, 5u);
      value.z = XeTextureCopyExpandTo8(packed & 0x1Fu, 5u);
      value.w = (packed >> 15u) * 0xFFu;
      break;
    case XE_TEXTURE_COPY_MODE_B4G4R4A4_TO_RGBA8:
      // R in bits 8:11, G in 4:7, B in 0:3, A in 12:15.
      packed = XeTextureCopyLoad16(row_address + (source.x << 1u));
      value = ((packed.xxxx >> uint4(8u, 4u, 0u, 12u)) & 0xFu) * 0x11u;
      break;
  }
  xe_texture_copy_dest[xe_thread_id] = value;
}
