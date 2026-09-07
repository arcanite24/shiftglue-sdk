#include <rex/graphics/d3d12/fh1_shader_pack.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <tuple>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>

namespace rex::graphics::d3d12 {
namespace {

constexpr uint32_t kVersion = 2;
constexpr uint32_t kBytecodeFormatDxil = 1;
constexpr uint32_t kKnownFlags = 0xF;
constexpr size_t kMaximumEntries = 65'535;
constexpr size_t kMaximumBindings = 255;
constexpr size_t kMaximumBytecodeSize = 16 * 1024 * 1024;
constexpr size_t kMaximumPackSize = 512 * 1024 * 1024;

struct Header {
  char magic[8];
  uint32_t version;
  uint32_t header_size;
  uint32_t entry_size;
  uint32_t entry_count;
  uint64_t index_offset;
  uint64_t data_offset;
  uint64_t data_size;
  uint8_t content_sha256[32];
  uint32_t translator_version;
  uint32_t vendor_id;
  uint32_t flags;
  uint32_t draw_resolution_scale_x;
  uint32_t draw_resolution_scale_y;
  uint32_t reserved[3];
};
static_assert(sizeof(Header) == 112);

struct StoredEntry {
  uint32_t stage;
  uint32_t bytecode_format;
  uint64_t guest_hash;
  uint64_t modification;
  uint64_t data_offset;
  uint64_t bytecode_size;
  uint32_t texture_binding_count;
  uint32_t sampler_binding_count;
  uint32_t used_texture_mask;
  uint32_t reserved;
  uint8_t bytecode_sha256[32];
};
static_assert(sizeof(StoredEntry) == 88);

struct StoredTextureBinding {
  uint32_t bindless_descriptor_index;
  uint32_t fetch_constant;
  uint32_t dimension;
  uint32_t is_signed;
};
static_assert(sizeof(StoredTextureBinding) == 16);

struct StoredSamplerBinding {
  uint32_t bindless_descriptor_index;
  uint32_t fetch_constant;
  uint32_t mag_filter;
  uint32_t min_filter;
  uint32_t mip_filter;
  uint32_t aniso_filter;
};
static_assert(sizeof(StoredSamplerBinding) == 24);

bool RangeValid(uint64_t offset, uint64_t size, size_t total_size) {
  return offset <= total_size && size <= total_size - offset;
}

bool Sha256(std::span<const uint8_t> bytes, std::array<uint8_t, 32>& digest_out) {
  if (bytes.size() > std::numeric_limits<ULONG>::max()) {
    return false;
  }
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (!BCRYPT_SUCCESS(
          BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
    return false;
  }
  const NTSTATUS status = BCryptHash(
      algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()),
      digest_out.data(), static_cast<ULONG>(digest_out.size()));
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return BCRYPT_SUCCESS(status);
}

bool SameLayout(const Fh1ShaderPack::Entry& left, const Fh1ShaderPack::Entry& right) {
  return left.used_texture_mask == right.used_texture_mask &&
         left.texture_bindings.size() == right.texture_bindings.size() &&
         left.sampler_bindings.size() == right.sampler_bindings.size() &&
         (left.texture_bindings.empty() ||
          !std::memcmp(left.texture_bindings.data(), right.texture_bindings.data(),
                       left.texture_bindings.size() * sizeof(left.texture_bindings.front()))) &&
         (left.sampler_bindings.empty() ||
          !std::memcmp(left.sampler_bindings.data(), right.sampler_bindings.data(),
                       left.sampler_bindings.size() * sizeof(left.sampler_bindings.front())));
}

auto EntryKey(const Fh1ShaderPack::Entry& entry) {
  return std::tuple(static_cast<uint32_t>(entry.stage), entry.guest_hash, entry.modification);
}

}  // namespace

bool Fh1ShaderPack::Load(const std::filesystem::path& path, const Config& expected_config,
                         std::string* error_out) {
  Clear();
  auto fail = [&](const char* error) {
    if (error_out) {
      *error_out = error;
    }
    Clear();
    return false;
  };

  std::error_code size_error;
  const uintmax_t file_size = std::filesystem::file_size(path, size_error);
  if (size_error) {
    return fail("unavailable");
  }
  if (file_size < sizeof(Header) || file_size > kMaximumPackSize) {
    return fail("invalid_size");
  }
  auto mapping = rex::memory::MappedMemory::Open(path, rex::memory::MappedMemory::Mode::kRead);
  if (!mapping || mapping->size() != file_size) {
    return fail("read_failed");
  }
  const std::span<const uint8_t> data(mapping->data(), mapping->size());

  Header header;
  std::memcpy(&header, data.data(), sizeof(header));
  const Config actual_config{header.translator_version, header.vendor_id, header.flags,
                             header.draw_resolution_scale_x,
                             header.draw_resolution_scale_y};
  constexpr char kMagic[8] = {'P', 'N', 'Y', 'N', 'S', 'H', 'P', 'K'};
  if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) || header.version != kVersion ||
      header.header_size != sizeof(Header) || header.entry_size != sizeof(StoredEntry) ||
      !header.entry_count || header.entry_count > kMaximumEntries ||
      header.index_offset != sizeof(Header) || header.flags & ~kKnownFlags ||
      actual_config != expected_config || header.reserved[0] || header.reserved[1] ||
      header.reserved[2]) {
    return fail("incompatible_header");
  }
  const uint64_t index_size = uint64_t(header.entry_count) * sizeof(StoredEntry);
  if (!RangeValid(header.index_offset, index_size, data.size()) ||
      header.data_offset != header.index_offset + index_size ||
      !RangeValid(header.data_offset, header.data_size, data.size()) ||
      header.data_offset + header.data_size != data.size()) {
    return fail("invalid_ranges");
  }
  std::array<uint8_t, 32> content_digest;
  if (!Sha256(std::span(data).subspan(static_cast<size_t>(header.index_offset)), content_digest) ||
      std::memcmp(content_digest.data(), header.content_sha256, content_digest.size())) {
    return fail("content_hash_mismatch");
  }

  entries_.reserve(header.entry_count);
  uint64_t previous_payload_end = 0;
  for (uint32_t index = 0; index < header.entry_count; ++index) {
    StoredEntry stored;
    std::memcpy(&stored,
                data.data() + header.index_offset + uint64_t(index) * sizeof(StoredEntry),
                sizeof(stored));
    if ((stored.stage != 1 && stored.stage != 2) ||
        stored.bytecode_format != kBytecodeFormatDxil || stored.reserved ||
        !stored.bytecode_size || stored.bytecode_size > kMaximumBytecodeSize ||
        stored.texture_binding_count > kMaximumBindings ||
        stored.sampler_binding_count > kMaximumBindings || stored.data_offset % 16 ||
        stored.data_offset < previous_payload_end) {
      return fail("invalid_entry");
    }
    const uint64_t texture_bytes =
        uint64_t(stored.texture_binding_count) * sizeof(StoredTextureBinding);
    const uint64_t sampler_bytes =
        uint64_t(stored.sampler_binding_count) * sizeof(StoredSamplerBinding);
    const uint64_t entry_size = stored.bytecode_size + texture_bytes + sampler_bytes;
    if (!RangeValid(stored.data_offset, entry_size, static_cast<size_t>(header.data_size))) {
      return fail("entry_out_of_range");
    }
    for (uint64_t padding = previous_payload_end; padding < stored.data_offset; ++padding) {
      if (data[header.data_offset + padding]) {
        return fail("nonzero_padding");
      }
    }
    const uint8_t* entry_data = data.data() + header.data_offset + stored.data_offset;
    if (stored.bytecode_size < 4 || std::memcmp(entry_data, "DXBC", 4)) {
      return fail("invalid_bytecode");
    }
    std::array<uint8_t, 32> bytecode_digest;
    if (!Sha256(std::span(entry_data, static_cast<size_t>(stored.bytecode_size)),
                bytecode_digest) ||
        std::memcmp(bytecode_digest.data(), stored.bytecode_sha256, bytecode_digest.size())) {
      return fail("bytecode_hash_mismatch");
    }

    Entry entry;
    entry.stage = stored.stage == 1 ? xenos::ShaderType::kVertex : xenos::ShaderType::kPixel;
    entry.guest_hash = stored.guest_hash;
    entry.modification = stored.modification;
    entry.bytecode = {entry_data, static_cast<size_t>(stored.bytecode_size)};
    const uint8_t* bindings_data = entry_data + stored.bytecode_size;
    uint32_t actual_texture_mask = 0;
    entry.texture_bindings.reserve(stored.texture_binding_count);
    for (uint32_t binding_index = 0; binding_index < stored.texture_binding_count;
         ++binding_index) {
      StoredTextureBinding binding;
      std::memcpy(&binding, bindings_data + binding_index * sizeof(binding), sizeof(binding));
      if (binding.fetch_constant >= 32 || binding.dimension > 3 || binding.is_signed > 1) {
        return fail("invalid_texture_binding");
      }
      actual_texture_mask |= 1u << binding.fetch_constant;
      entry.texture_bindings.push_back(
          {binding.bindless_descriptor_index, binding.fetch_constant,
           static_cast<xenos::FetchOpDimension>(binding.dimension), binding.is_signed != 0});
    }
    if (actual_texture_mask != stored.used_texture_mask) {
      return fail("texture_mask_mismatch");
    }
    bindings_data += texture_bytes;
    entry.sampler_bindings.reserve(stored.sampler_binding_count);
    for (uint32_t binding_index = 0; binding_index < stored.sampler_binding_count;
         ++binding_index) {
      StoredSamplerBinding binding;
      std::memcpy(&binding, bindings_data + binding_index * sizeof(binding), sizeof(binding));
      if (binding.fetch_constant >= 32 || binding.mag_filter > 3 || binding.min_filter > 3 ||
          binding.mip_filter > 3 || binding.aniso_filter > 7) {
        return fail("invalid_sampler_binding");
      }
      entry.sampler_bindings.push_back(
          {binding.bindless_descriptor_index, binding.fetch_constant,
           static_cast<xenos::TextureFilter>(binding.mag_filter),
           static_cast<xenos::TextureFilter>(binding.min_filter),
           static_cast<xenos::TextureFilter>(binding.mip_filter),
           static_cast<xenos::AnisoFilter>(binding.aniso_filter)});
    }
    entry.used_texture_mask = stored.used_texture_mask;
    if (!entries_.empty() && EntryKey(entry) <= EntryKey(entries_.back())) {
      return fail("unsorted_or_duplicate_identity");
    }
    if (!entries_.empty() && entry.stage == entries_.back().stage &&
        entry.guest_hash == entries_.back().guest_hash && !SameLayout(entry, entries_.back())) {
      return fail("inconsistent_shader_layout");
    }
    entries_.push_back(std::move(entry));
    previous_payload_end = stored.data_offset + entry_size;
  }
  if (previous_payload_end != header.data_size) {
    return fail("trailing_payload");
  }
  mapping_ = std::move(mapping);
  return true;
}

const Fh1ShaderPack::Entry* Fh1ShaderPack::Find(xenos::ShaderType stage, uint64_t guest_hash,
                                                uint64_t modification) const {
  const auto key = std::tuple(static_cast<uint32_t>(stage), guest_hash, modification);
  const auto found = std::lower_bound(entries_.begin(), entries_.end(), key,
                                      [](const Entry& entry, const auto& wanted) {
                                        return EntryKey(entry) < wanted;
                                      });
  return found != entries_.end() && EntryKey(*found) == key ? &*found : nullptr;
}

}  // namespace rex::graphics::d3d12
