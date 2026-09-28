#include <rex/graphics/d3d12/fh1_frame_census.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

REXCVAR_DEFINE_BOOL(fh1_frame_census, false, "GPU/D3D12",
                    "Aggregate a metadata-only census of consumed draws, "
                    "resolves and swaps for the FH1 native frame contract")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fh1_frame_census_path, "", "GPU/D3D12",
                      "JSON Lines file that receives one census record per window")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_frame_census_window, 60, "GPU/D3D12",
                     "Frames aggregated into one census record")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::d3d12 {
namespace {

constexpr size_t kMaxSurfaces = 256;
constexpr size_t kMaxDrawStates = 16384;
constexpr size_t kMaxTextures = 8192;
constexpr size_t kMaxCopies = 1024;
constexpr size_t kMaxResolveRanges = 4096;

// Render surface as the guest configured it, plus the host formats the
// backend bound for it. Band passes share one key with different offsets, so
// the window offset is part of the key.
using SurfaceKey = std::tuple<uint32_t, uint32_t, std::array<uint32_t, 4>, uint32_t,
                              uint32_t, std::array<uint32_t, 5>>;

struct DrawStateKey {
  uint64_t vertex_shader, pixel_shader, vertex_modification, pixel_modification;
  uint32_t surface;
  uint32_t guest_primitive, host_primitive, index_format;
  bool indexed;
  uint32_t depth_control, color_mask, stencil_ref_mask, color_control, mode_control;
  uint32_t stencil_ref_mask_bf;
  std::array<uint32_t, 4> blend;
  uint32_t used_texture_mask;
  bool memexport, occlusion_query;
  auto tie() const {
    return std::tie(vertex_shader, pixel_shader, vertex_modification, pixel_modification,
                    surface, guest_primitive, host_primitive, index_format, indexed,
                    depth_control, color_mask, stencil_ref_mask, color_control, mode_control,
                    stencil_ref_mask_bf, blend, used_texture_mask, memexport, occlusion_query);
  }
  bool operator==(const DrawStateKey& other) const { return tie() == other.tie(); }
};

// FNV-1a over 32-bit words; the census keys are plain integers.
struct WordHasher {
  uint64_t value = 14695981039346656037ull;
  void Add(uint64_t word) {
    value ^= word;
    value *= 1099511628211ull;
  }
};

struct DrawStateKeyHash {
  size_t operator()(const DrawStateKey& key) const {
    WordHasher hash;
    hash.Add(key.vertex_shader);
    hash.Add(key.pixel_shader);
    hash.Add(key.vertex_modification);
    hash.Add(key.pixel_modification);
    hash.Add(key.surface | (uint64_t(key.guest_primitive) << 32));
    hash.Add(key.host_primitive | (uint64_t(key.index_format) << 32));
    hash.Add(key.depth_control | (uint64_t(key.color_mask) << 32));
    hash.Add(key.stencil_ref_mask | (uint64_t(key.color_control) << 32));
    hash.Add(key.mode_control | (uint64_t(key.used_texture_mask) << 32));
    hash.Add(key.stencil_ref_mask_bf);
    for (uint32_t blend : key.blend) hash.Add(blend);
    hash.Add(uint64_t(key.indexed) | (uint64_t(key.memexport) << 1) |
             (uint64_t(key.occlusion_query) << 2));
    return size_t(hash.value);
  }
};

struct DrawStateCount {
  uint64_t pipeline_hash = 0;
  uint64_t draws = 0;
  uint64_t indices = 0;
};

// Texture layout as fetched; the base address is excluded so streamed
// instances of one layout aggregate, but resolve-destination hits are counted.
using TextureKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                              uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
struct TextureKeyHash {
  size_t operator()(const TextureKey& key) const {
    WordHasher hash;
    std::apply([&hash](auto... values) { (hash.Add(values), ...); }, key);
    return size_t(hash.value);
  }
};
struct TextureCount {
  uint64_t fetches = 0;
  uint64_t from_resolve = 0;
};

using CopyKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, bool>;
struct CopyCount {
  uint64_t copies = 0;
  uint64_t bytes = 0;
};

struct Census {
  std::mutex mutex;
  bool initialized = false;
  bool enabled = false;
  std::ofstream out;
  uint64_t window = 60;
  uint64_t first_frame = 0;
  bool window_open = false;
  uint64_t draws = 0;
  std::map<SurfaceKey, uint32_t> surface_ids;
  std::array<uint64_t, kMaxSurfaces> surface_draws{};
  std::unordered_map<DrawStateKey, DrawStateCount, DrawStateKeyHash> draw_states;
  std::unordered_map<TextureKey, TextureCount, TextureKeyHash> textures;
  // Consecutive draws usually share a surface, and a fetch constant is usually
  // unchanged between draws; these caches skip the table lookups then.
  // Element pointers stay valid until the window is flushed.
  DrawStateCount* last_draw_state = nullptr;
  DrawStateKey last_draw_state_key{};
  bool last_surface_valid = false;
  SurfaceKey last_surface_key;
  uint32_t last_surface_id = UINT32_MAX;
  struct FetchCache {
    bool valid = false;
    std::array<uint32_t, 6> words{};
    uint64_t resolve_generation = 0;
    TextureCount* texture = nullptr;
    bool from_resolve = false;
  };
  std::array<FetchCache, 32> fetch_cache{};
  uint64_t resolve_generation = 0;
  std::map<CopyKey, CopyCount> copies;
  std::map<uint32_t, uint64_t> clears;
  std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint64_t> swaps;
  uint64_t zpd_events = 0;
  std::map<uint32_t, uint64_t> zpd_addresses;
  std::map<uint32_t, uint32_t> resolve_ranges;  // base -> end, kept across windows
  uint64_t overflow_surfaces = 0, overflow_draw_states = 0, overflow_textures = 0,
           overflow_copies = 0;
  // Time spent inside the census itself this window, including the flush that
  // closed the previous one.
  uint64_t cost_ns = 0;
};

// Adds the scope's duration to the census cost; declared after the lock so
// contention with other threads is not counted.
class CostScope {
 public:
  explicit CostScope(Census& census)
      : census_(census), start_(std::chrono::steady_clock::now()) {}
  ~CostScope() {
    census_.cost_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - start_)
                                    .count());
  }

 private:
  Census& census_;
  std::chrono::steady_clock::time_point start_;
};

Census& State() {
  static Census census;
  return census;
}

bool EnsureInitialized(Census& census) {
  if (census.initialized) return census.enabled;
  census.initialized = true;
  if (!REXCVAR_GET(fh1_frame_census)) return false;
  const std::string path = REXCVAR_GET(fh1_frame_census_path);
  if (path.empty()) return false;
  census.out.open(path, std::ios::app);
  census.window = std::max<int32_t>(1, REXCVAR_GET(fh1_frame_census_window));
  census.enabled = census.out.good();
  // Race windows reach a few thousand states; avoid rehashing mid-frame.
  census.draw_states.reserve(8192);
  census.textures.reserve(2048);
  return census.enabled;
}

void OpenWindow(Census& census, uint64_t frame) {
  if (census.window_open) return;
  census.window_open = true;
  census.first_frame = frame;
}

bool InResolveRange(const Census& census, uint32_t address) {
  auto it = census.resolve_ranges.upper_bound(address);
  if (it == census.resolve_ranges.begin()) return false;
  --it;
  return address < it->second;
}

template <size_t N>
void AppendHexArray(fmt::memory_buffer& out, const std::array<uint32_t, N>& values) {
  out.push_back('[');
  for (size_t i = 0; i < N; ++i) {
    fmt::format_to(std::back_inserter(out), "{}\"{:08X}\"", i ? "," : "", values[i]);
  }
  out.push_back(']');
}

void Flush(Census& census, uint64_t last_frame) {
  fmt::memory_buffer line;
  auto out = std::back_inserter(line);
  fmt::format_to(out,
                 "{{\"schema\":\"pinyon-shift.fh1-frame-census.v1\",\"first_frame\":{},"
                 "\"last_frame\":{},\"draws\":{},\"surfaces\":[",
                 census.first_frame, last_frame, census.draws);
  bool first = true;
  for (const auto& [key, id] : census.surface_ids) {
    const auto& [surface_info, depth_info, color_info, window_offset, bound_bits,
                 host_formats] = key;
    fmt::format_to(out,
                   "{}{{\"id\":{},\"surface_info\":\"{:08X}\",\"depth_info\":\"{:08X}\","
                   "\"color_info\":",
                   first ? "" : ",", id, surface_info, depth_info);
    AppendHexArray(line, color_info);
    fmt::format_to(out, ",\"window_offset\":\"{:08X}\",\"bound\":{},\"host_formats\":",
                   window_offset, bound_bits);
    AppendHexArray(line, host_formats);
    fmt::format_to(out, ",\"draws\":{}}}", census.surface_draws[id]);
    first = false;
  }
  fmt::format_to(out, "],\"draw_states\":[");
  first = true;
  for (const auto& [key, count] : census.draw_states) {
    fmt::format_to(out,
                   "{}{{\"vs\":\"{:016X}\",\"ps\":\"{:016X}\",\"vs_mod\":\"{:016X}\","
                   "\"ps_mod\":\"{:016X}\",\"surface\":{},\"guest_primitive\":{},"
                   "\"host_primitive\":{},\"indexed\":{},\"index_format\":{},"
                   "\"depth_control\":\"{:08X}\",\"color_mask\":\"{:08X}\","
                   "\"stencil_ref_mask\":\"{:08X}\",\"stencil_ref_mask_bf\":\"{:08X}\","
                   "\"color_control\":\"{:08X}\",\"mode_control\":\"{:08X}\",\"blend\":",
                   first ? "" : ",", key.vertex_shader, key.pixel_shader,
                   key.vertex_modification, key.pixel_modification, key.surface,
                   key.guest_primitive, key.host_primitive, key.indexed, key.index_format,
                   key.depth_control, key.color_mask, key.stencil_ref_mask,
                   key.stencil_ref_mask_bf, key.color_control, key.mode_control);
    AppendHexArray(line, key.blend);
    fmt::format_to(out,
                   ",\"textures\":\"{:08X}\",\"memexport\":{},\"occlusion_query\":{},"
                   "\"pipeline\":\"{:016X}\",\"draws\":{},\"indices\":{}}}",
                   key.used_texture_mask, key.memexport, key.occlusion_query,
                   count.pipeline_hash, count.draws, count.indices);
    first = false;
  }
  fmt::format_to(out, "],\"textures\":[");
  first = true;
  for (const auto& [key, count] : census.textures) {
    const auto& [format, dimension, width, height, depth, tiled, packed_mips, mip_min,
                 mip_max, signs, endian, swizzle] = key;
    fmt::format_to(out,
                   "{}{{\"format\":{},\"dimension\":{},\"width\":{},\"height\":{},"
                   "\"depth\":{},\"tiled\":{},\"packed_mips\":{},\"mip_min\":{},"
                   "\"mip_max\":{},\"signs\":{},\"endian\":{},\"swizzle\":\"{:08X}\","
                   "\"fetches\":{},\"from_resolve\":{}}}",
                   first ? "" : ",", format, dimension, width, height, depth, tiled,
                   packed_mips, mip_min, mip_max, signs, endian, swizzle, count.fetches,
                   count.from_resolve);
    first = false;
  }
  fmt::format_to(out, "],\"copies\":[");
  first = true;
  for (const auto& [key, count] : census.copies) {
    const auto& [copy_control, dest_info, dest_pitch, surface_info, source_info,
                 depth_info, succeeded] = key;
    fmt::format_to(out,
                   "{}{{\"copy_control\":\"{:08X}\",\"dest_info\":\"{:08X}\","
                   "\"dest_pitch\":\"{:08X}\",\"surface_info\":\"{:08X}\","
                   "\"source_info\":\"{:08X}\",\"depth_info\":\"{:08X}\",\"succeeded\":{},"
                   "\"copies\":{},\"bytes\":{}}}",
                   first ? "" : ",", copy_control, dest_info, dest_pitch, surface_info,
                   source_info, depth_info, succeeded, count.copies, count.bytes);
    first = false;
  }
  fmt::format_to(out, "],\"optimized_clears\":{{");
  first = true;
  for (const auto& [mode, count] : census.clears) {
    fmt::format_to(out, "{}\"{}\":{}", first ? "" : ",", mode, count);
    first = false;
  }
  fmt::format_to(out, "}},\"swaps\":[");
  first = true;
  for (const auto& [key, count] : census.swaps) {
    const auto& [format, width, height] = key;
    fmt::format_to(out, "{}{{\"format\":{},\"width\":{},\"height\":{},\"swaps\":{}}}",
                   first ? "" : ",", format, width, height, count);
    first = false;
  }
  fmt::format_to(out,
                 "],\"zpd_events\":{},\"zpd_addresses\":{},\"cost_ns\":{},"
                 "\"overflow\":{{\"surfaces\":{},\"draw_states\":{},\"textures\":{},"
                 "\"copies\":{}}}}}\n",
                 census.zpd_events, census.zpd_addresses.size(), census.cost_ns,
                 census.overflow_surfaces, census.overflow_draw_states,
                 census.overflow_textures, census.overflow_copies);
  census.out.write(line.data(), std::streamsize(line.size()));
  census.out.flush();

  census.window_open = false;
  census.draws = 0;
  census.surface_ids.clear();
  census.surface_draws.fill(0);
  census.draw_states.clear();
  census.textures.clear();
  census.last_surface_valid = false;
  census.last_draw_state = nullptr;
  census.fetch_cache = {};
  census.copies.clear();
  census.clears.clear();
  census.swaps.clear();
  census.zpd_events = 0;
  census.zpd_addresses.clear();
  census.cost_ns = 0;
  census.overflow_surfaces = census.overflow_draw_states = census.overflow_textures =
      census.overflow_copies = 0;
}

}  // namespace

bool Fh1FrameCensus::Enabled() {
  static const bool enabled = REXCVAR_GET(fh1_frame_census);
  return enabled;
}

void Fh1FrameCensus::ObserveDraw(const RegisterFile& regs, const Fh1CensusDraw& draw) {
  auto& census = State();
  std::lock_guard lock(census.mutex);
  if (!EnsureInitialized(census)) return;
  CostScope cost(census);
  OpenWindow(census, draw.frame);
  ++census.draws;

  std::array<uint32_t, 4> color_info{};
  std::array<uint32_t, 4> blend{};
  for (uint32_t i = 0; i < 4; ++i) {
    if (draw.bound_render_target_bits & (1u << (1 + i))) {
      color_info[i] = regs[reg::RB_COLOR_INFO::rt_register_indices[i]];
      blend[i] = regs[reg::RB_BLENDCONTROL::rt_register_indices[i]];
    }
  }
  std::array<uint32_t, 5> host_formats{};
  std::copy(std::begin(draw.host_render_target_formats),
            std::end(draw.host_render_target_formats), host_formats.begin());
  const SurfaceKey surface_key{regs.Get<reg::RB_SURFACE_INFO>().value,
                               (draw.bound_render_target_bits & 1u)
                                   ? regs.Get<reg::RB_DEPTH_INFO>().value
                                   : 0u,
                               color_info, regs.Get<reg::PA_SC_WINDOW_OFFSET>().value,
                               draw.bound_render_target_bits, host_formats};
  uint32_t surface_id = UINT32_MAX;
  if (census.last_surface_valid && census.last_surface_key == surface_key) {
    surface_id = census.last_surface_id;
  } else if (auto it = census.surface_ids.find(surface_key); it != census.surface_ids.end()) {
    surface_id = it->second;
  } else if (census.surface_ids.size() < kMaxSurfaces) {
    surface_id = uint32_t(census.surface_ids.size());
    census.surface_ids.emplace(surface_key, surface_id);
  } else {
    ++census.overflow_surfaces;
  }
  if (surface_id != UINT32_MAX) {
    census.last_surface_valid = true;
    census.last_surface_key = surface_key;
    census.last_surface_id = surface_id;
  }
  if (surface_id != UINT32_MAX) ++census.surface_draws[surface_id];

  const DrawStateKey key{draw.vertex_shader,
                         draw.pixel_shader,
                         draw.vertex_modification,
                         draw.pixel_modification,
                         surface_id,
                         draw.guest_primitive,
                         draw.host_primitive,
                         draw.index_format,
                         draw.indexed,
                         draw.normalized_depth_control,
                         draw.normalized_color_mask,
                         regs.Get<reg::RB_STENCILREFMASK>().value,
                         regs.Get<reg::RB_COLORCONTROL>().value,
                         regs.Get<reg::PA_SU_SC_MODE_CNTL>().value,
                         regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF).value,
                         blend,
                         draw.used_texture_mask,
                         draw.memexport,
                         draw.occlusion_query};
  DrawStateCount* state = nullptr;
  if (census.last_draw_state && census.last_draw_state_key == key) {
    state = census.last_draw_state;
  } else {
    auto it = census.draw_states.find(key);
    if (it == census.draw_states.end() && census.draw_states.size() < kMaxDrawStates) {
      it = census.draw_states.emplace(key, DrawStateCount{draw.pipeline_hash}).first;
    }
    if (it != census.draw_states.end()) {
      state = &it->second;
      census.last_draw_state = state;
      census.last_draw_state_key = key;
    }
  }
  if (state) {
    ++state->draws;
    state->indices += draw.index_count;
  } else {
    ++census.overflow_draw_states;
  }

  for (uint32_t mask = draw.used_texture_mask; mask; mask &= mask - 1) {
    const uint32_t index = uint32_t(std::countr_zero(mask));
    const auto fetch = regs.GetTextureFetch(index);
    auto& cached = census.fetch_cache[index];
    std::array<uint32_t, 6> words;
    std::memcpy(words.data(), &fetch, sizeof(words));
    if (cached.valid && cached.words == words &&
        cached.resolve_generation == census.resolve_generation) {
      ++cached.texture->fetches;
      if (cached.from_resolve) ++cached.texture->from_resolve;
      continue;
    }
    uint32_t width = 0, height = 0, depth = 0;
    switch (fetch.dimension) {
      case xenos::DataDimension::k1D:
        width = fetch.size_1d.width + 1;
        break;
      case xenos::DataDimension::k3D:
        width = fetch.size_3d.width + 1;
        height = fetch.size_3d.height + 1;
        depth = fetch.size_3d.depth + 1;
        break;
      default:
        width = fetch.size_2d.width + 1;
        height = fetch.size_2d.height + 1;
        depth = fetch.size_2d.stack_depth + 1;
        break;
    }
    const uint32_t signs = uint32_t(fetch.sign_x) | (uint32_t(fetch.sign_y) << 2) |
                           (uint32_t(fetch.sign_z) << 4) | (uint32_t(fetch.sign_w) << 6);
    const TextureKey texture_key{uint32_t(fetch.format), uint32_t(fetch.dimension), width,
                                 height, depth, fetch.tiled, fetch.packed_mips,
                                 fetch.mip_min_level, fetch.mip_max_level, signs,
                                 uint32_t(fetch.endianness), fetch.swizzle};
    auto texture = census.textures.find(texture_key);
    if (texture == census.textures.end() && census.textures.size() < kMaxTextures) {
      texture = census.textures.emplace(texture_key, TextureCount{}).first;
    }
    if (texture == census.textures.end()) {
      ++census.overflow_textures;
      continue;
    }
    ++texture->second.fetches;
    const bool from_resolve = InResolveRange(census, fetch.base_address << 12);
    if (from_resolve) ++texture->second.from_resolve;
    cached.valid = true;
    cached.words = words;
    cached.resolve_generation = census.resolve_generation;
    cached.texture = &texture->second;
    cached.from_resolve = from_resolve;
  }
}

void Fh1FrameCensus::ObserveOptimizedClear(uint64_t frame, uint32_t mode) {
  auto& census = State();
  std::lock_guard lock(census.mutex);
  if (!EnsureInitialized(census)) return;
  CostScope cost(census);
  OpenWindow(census, frame);
  ++census.clears[mode];
}

void Fh1FrameCensus::ObserveCopy(const RegisterFile& regs, uint64_t frame,
                                 uint32_t written_address, uint32_t written_length,
                                 bool succeeded) {
  auto& census = State();
  std::lock_guard lock(census.mutex);
  if (!EnsureInitialized(census)) return;
  CostScope cost(census);
  OpenWindow(census, frame);
  const auto copy_control = regs.Get<reg::RB_COPY_CONTROL>();
  const uint32_t source = copy_control.copy_src_select;
  const CopyKey key{copy_control.value,
                    regs.Get<reg::RB_COPY_DEST_INFO>().value,
                    regs.Get<reg::RB_COPY_DEST_PITCH>().value,
                    regs.Get<reg::RB_SURFACE_INFO>().value,
                    source < 4 ? regs[reg::RB_COLOR_INFO::rt_register_indices[source]] : 0u,
                    regs.Get<reg::RB_DEPTH_INFO>().value,
                    succeeded};
  auto copy = census.copies.find(key);
  if (copy == census.copies.end() && census.copies.size() < kMaxCopies) {
    copy = census.copies.emplace(key, CopyCount{}).first;
  }
  if (copy != census.copies.end()) {
    ++copy->second.copies;
    copy->second.bytes += written_length;
  } else {
    ++census.overflow_copies;
  }
  if (succeeded && written_length) {
    if (census.resolve_ranges.size() >= kMaxResolveRanges) census.resolve_ranges.clear();
    census.resolve_ranges[written_address] = written_address + written_length;
    ++census.resolve_generation;
  }
}

void Fh1FrameCensus::ObserveZpd(uint64_t frame, uint32_t sample_count_address) {
  auto& census = State();
  std::lock_guard lock(census.mutex);
  if (!EnsureInitialized(census)) return;
  CostScope cost(census);
  OpenWindow(census, frame);
  ++census.zpd_events;
  if (census.zpd_addresses.size() < 4096) ++census.zpd_addresses[sample_count_address];
}

void Fh1FrameCensus::ObserveSwap(uint64_t frame, uint32_t frontbuffer_address,
                                 uint32_t width, uint32_t height, uint32_t format) {
  auto& census = State();
  std::lock_guard lock(census.mutex);
  if (!EnsureInitialized(census)) return;
  CostScope cost(census);
  OpenWindow(census, frame);
  ++census.swaps[{format, width, height}];
  (void)frontbuffer_address;
  if (frame + 1 >= census.first_frame + census.window) Flush(census, frame);
}

}  // namespace rex::graphics::d3d12
