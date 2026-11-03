#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/shared_memory.h>

#include "test_memory.h"

namespace {

using namespace rex::graphics;

// Exercise the shared cache's real watches and binding memo without a GPU.
class TestSharedMemory final : public SharedMemory {
 public:
  TestSharedMemory() : SharedMemory(rex::testing::GetTestMemory()) { InitializeCommon(); }

 private:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& ranges) override {
    for (const auto& [first, count] : ranges) {
      MakeRangeValid(first << page_size_log2(), count << page_size_log2(), false);
    }
    return true;
  }
};

class TestTextureCache final : public TextureCache {
 public:
  TestTextureCache(const RegisterFile& regs, SharedMemory& memory)
      : TextureCache(regs, memory, 2, 2) {}

  const Texture* bound(uint32_t index = 0) const {
    const auto* binding = GetValidTextureBinding(index);
    return binding ? binding->texture : nullptr;
  }

  bool EnsureScaledResolveMemoryCommitted(uint32_t, uint32_t, uint32_t) override {
    return true;
  }

 private:
  class TestTexture final : public Texture {
   public:
    TestTexture(TextureCache& cache, TextureKey key) : Texture(cache, key, false) {}
  };

  uint32_t GetHostFormatSwizzle(TextureKey) const override {
    return xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  }
  uint32_t GetMaxHostTextureWidthHeight(xenos::DataDimension) const override { return 16384; }
  uint32_t GetMaxHostTextureDepthOrArraySize(xenos::DataDimension) const override { return 2048; }
  bool IsScaledResolveSupportedForFormat(TextureKey) const override { return true; }
  std::unique_ptr<Texture> CreateTexture(TextureKey key) override {
    return std::make_unique<TestTexture>(*this, key);
  }
  bool LoadTextureDataFromResidentMemoryImpl(Texture&, bool, bool) override { return true; }
};

void SetFetch(RegisterFile& regs, uint32_t index, uint32_t address) {
  xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.type = xenos::FetchConstantType::kTexture;
  fetch.dimension = xenos::DataDimension::k2DOrStacked;
  fetch.format = xenos::TextureFormat::k_8_8_8_8;
  fetch.tiled = 1;
  fetch.pitch = 2;
  fetch.size_2d.width = 63;
  fetch.size_2d.height = 63;
  fetch.base_address = address >> 12;
  std::memcpy(&regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + index * 6], &fetch,
              sizeof(fetch));
}

}  // namespace

TEST_CASE("shared memory watches grow past a pool and recycle cancelled ranges",
          "[graphics][shared_memory]") {
  constexpr uint32_t kBase = 0x100000;
  constexpr size_t kWatches = 16'385;
  std::vector<uint32_t> callbacks(kWatches, 0);
  TestSharedMemory memory;
  const auto callback = [](const std::unique_lock<std::recursive_mutex>&, void* context,
                           void*, uint64_t index, bool) {
    ++(*static_cast<std::vector<uint32_t>*>(context))[index];
  };
  std::vector<SharedMemory::WatchHandle> watches;
  for (size_t i = 0; i < kWatches; ++i) {
    auto watch = memory.WatchMemoryRange(kBase, 4096, callback, &callbacks, nullptr, i);
    REQUIRE(watch);
    watches.push_back(watch);
  }
  for (size_t i = 0; i < kWatches; i += 2) memory.UnwatchMemoryRange(watches[i]);
  for (size_t i = 0; i < kWatches; i += 2) {
    REQUIRE(memory.WatchMemoryRange(kBase, 4096, callback, &callbacks, nullptr, i));
  }
  memory.MemoryInvalidationCallback(kBase, 4096, true);
  CHECK(std::all_of(callbacks.begin(), callbacks.end(), [](uint32_t n) { return n == 1; }));
  memory.MemoryInvalidationCallback(kBase, 4096, true);
  CHECK(std::all_of(callbacks.begin(), callbacks.end(), [](uint32_t n) { return n == 1; }));
}

TEST_CASE("texture binding memo follows resolve ownership with unchanged fetch words",
          "[graphics][texture_cache]") {
  RegisterFile regs;
  TestSharedMemory memory;
  TestTextureCache cache(regs, memory);
  constexpr uint32_t kBase = 0x100000;
  constexpr uint32_t kOther = 0x200000;
  SetFetch(regs, 0, kBase);
  SetFetch(regs, 1, kOther);
  cache.RequestTextures(3);
  const auto* cpu_texture = cache.bound();
  const auto* unrelated = cache.bound(1);
  REQUIRE(cpu_texture);
  REQUIRE(unrelated);
  CHECK_FALSE(cpu_texture->key().scaled_resolve);

  cache.MarkRangeAsResolved(kBase, cpu_texture->GetGuestBaseSize());
  cache.RequestTextures(3);
  const auto* resolved = cache.bound();
  REQUIRE(resolved);
  CHECK(resolved->key().scaled_resolve);
  CHECK(resolved != cpu_texture);
  CHECK(cache.bound(1) == unrelated);
  CHECK(unrelated->payload_generation() == 1);

  // Reuse the livery allocation for a CPU texture, retaining the fetch words.
  memory.MemoryInvalidationCallback(kBase, resolved->GetGuestBaseSize(), true);
  cache.RequestTextures(2);  // Consume invalidation while only the other texture is used.
  cache.RequestTextures(3);
  REQUIRE(cache.bound());
  CHECK_FALSE(cache.bound()->key().scaled_resolve);
  CHECK(cache.bound() == cpu_texture);
  CHECK(cache.bound(1) == unrelated);
  CHECK(unrelated->payload_generation() == 1);
}

TEST_CASE("texture diagnostics report guest padding and separate mip storage",
          "[graphics][texture_cache]") {
  RegisterFile regs;
  TestSharedMemory memory;
  TestTextureCache cache(regs, memory);
  CHECK(cache.GetActiveTextureGuestRanges(0)[0].second == 0);
  constexpr uint32_t kBase = 0x400000, kMips = 0x800000;
  xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.type = xenos::FetchConstantType::kTexture;
  fetch.dimension = xenos::DataDimension::k2DOrStacked;
  fetch.tiled = 1;
  fetch.base_address = kBase >> 12;
  SECTION("32-bit guest scene with a 64-bit host view") {
    fetch.format = xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;
    fetch.pitch = 40;
    fetch.size_2d.width = 1279;
    fetch.size_2d.height = 719;
  }
  SECTION("packed mips have their own allocation") {
    fetch.format = xenos::TextureFormat::k_8_8_8_8;
    fetch.pitch = 2;
    fetch.size_2d.width = 63;
    fetch.size_2d.height = 63;
    fetch.packed_mips = 1;
    fetch.mip_address = kMips >> 12;
    fetch.mip_max_level = 3;
  }
  std::memcpy(&regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], &fetch, sizeof(fetch));
  cache.RequestTextures(1);
  REQUIRE(cache.bound());
  const auto ranges = cache.GetActiveTextureGuestRanges(0);
  CHECK(ranges[0].first == kBase);
  CHECK(ranges[0].second == (fetch.mip_address ? 64u * 64u * 4u : 1280u * 736u * 4u));
  CHECK(ranges[1].first == (fetch.mip_address ? kMips : 0));
  CHECK(ranges[1].second == (fetch.mip_address ? 8192 : 0));
}

TEST_CASE("compressed textures do not select a partly owned scaled buffer",
          "[graphics][texture_cache]") {
  RegisterFile regs;
  TestSharedMemory memory;
  TestTextureCache cache(regs, memory);
  constexpr uint32_t kBase = 0x400000;
  constexpr uint32_t kMips = 0x418000;
  xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.type = xenos::FetchConstantType::kTexture;
  fetch.dimension = xenos::DataDimension::k2DOrStacked;
  fetch.format = xenos::TextureFormat::k_DXT1;
  fetch.tiled = 1;
  fetch.packed_mips = 1;
  fetch.pitch = 24;
  fetch.size_2d.width = 767;
  fetch.size_2d.height = 255;
  fetch.base_address = kBase >> 12;
  fetch.mip_address = kMips >> 12;
  fetch.mip_max_level = 9;
  std::memcpy(&regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], &fetch, sizeof(fetch));
  SetFetch(regs, 1, 0x800000);
  cache.RequestTextures(3);
  const auto* cpu = cache.bound();
  const auto* unrelated = cache.bound(1);
  REQUIRE(cpu);
  REQUIRE(unrelated);
  CHECK_FALSE(cpu->key().scaled_resolve);
  const uint32_t length = kMips - kBase + cpu->GetGuestMipsSize();

  cache.MarkRangeAsResolved(kBase, length);
  cache.RequestTextures(3);
  REQUIRE(cache.bound());
  CHECK(cache.bound()->key().scaled_resolve);

  // Reuse the base for CPU asset data, leaving old GPU ownership in the mips.
  memory.MemoryInvalidationCallback(kBase, cpu->GetGuestBaseSize(), true);
  cache.RequestTextures(3);
  REQUIRE(cache.bound());
  CHECK_FALSE(cache.bound()->key().scaled_resolve);
  CHECK(cache.bound() == cpu);
  CHECK(cache.bound(1) == unrelated);
  CHECK(unrelated->payload_generation() == 1);

  // The reverse mixture must not select scaled memory either.
  memory.MemoryInvalidationCallback(kBase, length, true);
  cache.MarkRangeAsResolved(kBase, cpu->GetGuestBaseSize());
  cache.RequestTextures(3);
  REQUIRE(cache.bound());
  CHECK_FALSE(cache.bound()->key().scaled_resolve);
}
