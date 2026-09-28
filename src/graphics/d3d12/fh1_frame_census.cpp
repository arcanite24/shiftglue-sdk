#include <rex/graphics/d3d12/fh1_frame_census.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>

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
  std::array<uint32_t, 4> blend;
  uint32_t used_texture_mask;
  bool memexport, occlusion_query;
  auto tie() const {
    return std::tie(vertex_shader, pixel_shader, vertex_modification, pixel_modification,
                    surface, guest_primitive, host_primitive, index_format, indexed,
                    depth_control, color_mask, stencil_ref_mask, color_control, mode_control,
                    blend, used_texture_mask, memexport, occlusion_query);
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
  std::map<uint32_t, uint64_t> surface_draws;
  std::unordered_map<DrawStateKey, DrawStateCount, DrawStateKeyHash> draw_states;
  std::unordered_map<TextureKey, TextureCount, TextureKeyHash> textures;
  // Consecutive draws usually share a surface, and a fetch constant is usually
  // unchanged between draws; these caches skip the table lookups then.
  // Element pointers stay valid until the window is flushed.
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

std::string Hex64(uint64_t value) {
  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(value));
  return buffer;
}

std::string Hex32(uint32_t value) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%08X", value);
  return buffer;
}

template <size_t N>
std::string HexArray(const std::array<uint32_t, N>& values) {
  std::string result = "[";
  for (size_t i = 0; i < N; ++i) {
    if (i) result += ",";
    result += "\"" + Hex32(values[i]) + "\"";
  }
  return result + "]";
}

void Flush(Census& census, uint64_t last_frame) {
  std::string line;
  line.reserve(1 << 16);
  line += "{\"schema\":\"pinyon-shift.fh1-frame-census.v1\",\"first_frame\":" +
          std::to_string(census.first_frame) + ",\"last_frame\":" +
          std::to_string(last_frame) + ",\"draws\":" + std::to_string(census.draws);
  line += ",\"surfaces\":[";
  bool first = true;
  for (const auto& [key, id] : census.surface_ids) {
    const auto& [surface_info, depth_info, color_info, window_offset, bound_bits,
                 host_formats] = key;
    line += std::string(first ? "" : ",") + "{\"id\":" + std::to_string(id) +
            ",\"surface_info\":\"" + Hex32(surface_info) + "\",\"depth_info\":\"" +
            Hex32(depth_info) + "\",\"color_info\":" + HexArray(color_info) +
            ",\"window_offset\":\"" + Hex32(window_offset) + "\",\"bound\":" +
            std::to_string(bound_bits) + ",\"host_formats\":" + HexArray(host_formats) +
            ",\"draws\":" + std::to_string(census.surface_draws[id]) + "}";
    first = false;
  }
  line += "],\"draw_states\":[";
  first = true;
  for (const auto& [key, count] : census.draw_states) {
    line += std::string(first ? "" : ",") + "{\"vs\":\"" + Hex64(key.vertex_shader) +
            "\",\"ps\":\"" + Hex64(key.pixel_shader) + "\",\"vs_mod\":\"" +
            Hex64(key.vertex_modification) + "\",\"ps_mod\":\"" +
            Hex64(key.pixel_modification) + "\",\"surface\":" + std::to_string(key.surface) +
            ",\"guest_primitive\":" + std::to_string(key.guest_primitive) +
            ",\"host_primitive\":" + std::to_string(key.host_primitive) +
            ",\"indexed\":" + (key.indexed ? "true" : "false") +
            ",\"index_format\":" + std::to_string(key.index_format) +
            ",\"depth_control\":\"" + Hex32(key.depth_control) + "\",\"color_mask\":\"" +
            Hex32(key.color_mask) + "\",\"stencil_ref_mask\":\"" +
            Hex32(key.stencil_ref_mask) + "\",\"color_control\":\"" +
            Hex32(key.color_control) + "\",\"mode_control\":\"" + Hex32(key.mode_control) +
            "\",\"blend\":" + HexArray(key.blend) + ",\"textures\":\"" +
            Hex32(key.used_texture_mask) + "\",\"memexport\":" +
            (key.memexport ? "true" : "false") + ",\"occlusion_query\":" +
            (key.occlusion_query ? "true" : "false") + ",\"pipeline\":\"" +
            Hex64(count.pipeline_hash) + "\",\"draws\":" + std::to_string(count.draws) +
            ",\"indices\":" + std::to_string(count.indices) + "}";
    first = false;
  }
  line += "],\"textures\":[";
  first = true;
  for (const auto& [key, count] : census.textures) {
    const auto& [format, dimension, width, height, depth, tiled, packed_mips, mip_min,
                 mip_max, signs, endian, swizzle] = key;
    line += std::string(first ? "" : ",") + "{\"format\":" + std::to_string(format) +
            ",\"dimension\":" + std::to_string(dimension) + ",\"width\":" +
            std::to_string(width) + ",\"height\":" + std::to_string(height) +
            ",\"depth\":" + std::to_string(depth) + ",\"tiled\":" + std::to_string(tiled) +
            ",\"packed_mips\":" + std::to_string(packed_mips) + ",\"mip_min\":" +
            std::to_string(mip_min) + ",\"mip_max\":" + std::to_string(mip_max) +
            ",\"signs\":" + std::to_string(signs) + ",\"endian\":" + std::to_string(endian) +
            ",\"swizzle\":\"" + Hex32(swizzle) + "\",\"fetches\":" +
            std::to_string(count.fetches) + ",\"from_resolve\":" +
            std::to_string(count.from_resolve) + "}";
    first = false;
  }
  line += "],\"copies\":[";
  first = true;
  for (const auto& [key, count] : census.copies) {
    const auto& [copy_control, dest_info, dest_pitch, surface_info, source_info,
                 depth_info, succeeded] = key;
    line += std::string(first ? "" : ",") + "{\"copy_control\":\"" + Hex32(copy_control) +
            "\",\"dest_info\":\"" + Hex32(dest_info) + "\",\"dest_pitch\":\"" +
            Hex32(dest_pitch) + "\",\"surface_info\":\"" + Hex32(surface_info) +
            "\",\"source_info\":\"" + Hex32(source_info) + "\",\"depth_info\":\"" +
            Hex32(depth_info) + "\",\"succeeded\":" + (succeeded ? "true" : "false") +
            ",\"copies\":" + std::to_string(count.copies) + ",\"bytes\":" +
            std::to_string(count.bytes) + "}";
    first = false;
  }
  line += "],\"optimized_clears\":{";
  first = true;
  for (const auto& [mode, count] : census.clears) {
    line += std::string(first ? "" : ",") + "\"" + std::to_string(mode) +
            "\":" + std::to_string(count);
    first = false;
  }
  line += "},\"swaps\":[";
  first = true;
  for (const auto& [key, count] : census.swaps) {
    const auto& [format, width, height] = key;
    line += std::string(first ? "" : ",") + "{\"format\":" + std::to_string(format) +
            ",\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height) +
            ",\"swaps\":" + std::to_string(count) + "}";
    first = false;
  }
  line += "],\"zpd_events\":" + std::to_string(census.zpd_events) +
          ",\"zpd_addresses\":" + std::to_string(census.zpd_addresses.size()) +
          ",\"cost_ns\":" + std::to_string(census.cost_ns);
  line += ",\"overflow\":{\"surfaces\":" + std::to_string(census.overflow_surfaces) +
          ",\"draw_states\":" + std::to_string(census.overflow_draw_states) +
          ",\"textures\":" + std::to_string(census.overflow_textures) +
          ",\"copies\":" + std::to_string(census.overflow_copies) + "}}\n";
  census.out << line;
  census.out.flush();

  census.window_open = false;
  census.draws = 0;
  census.surface_ids.clear();
  census.surface_draws.clear();
  census.draw_states.clear();
  census.textures.clear();
  census.last_surface_valid = false;
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
                         blend,
                         draw.used_texture_mask,
                         draw.memexport,
                         draw.occlusion_query};
  auto state = census.draw_states.find(key);
  if (state == census.draw_states.end() && census.draw_states.size() < kMaxDrawStates) {
    state = census.draw_states.emplace(key, DrawStateCount{draw.pipeline_hash}).first;
  }
  if (state != census.draw_states.end()) {
    ++state->second.draws;
    state->second.indices += draw.index_count;
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
