// UP-1.3: the camera and depth a temporal upscaler needs, taken from the
// title's own draws. FH1's scene vertex shaders read one world-view-projection
// matrix per object from float constants c128-c131 (a row each: clip x, y, z
// and w). The static world shares one matrix across hundreds of draws, so the
// largest group of draws with equal matrices is the world, and matching its
// objects with the previous frame's gives the camera's motion: for geometry
// that did not move, WVP_prev * WVP_cur^-1 = VP_prev * VP_cur^-1. The same
// matrix gives the lens: the y row's length is 1 / tan(fov_y / 2), and the z
// and w rows give the near and far planes.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/fh1_native_executor.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

namespace rex::graphics::vulkan {

namespace {

// The registers of the world-view-projection rows.
constexpr uint32_t kCameraFirstRegister = 128;
// UP-1.7: the first register of a car's own world-view-projection.
constexpr uint32_t kObjectFirstRegister = 0;
// Fewer draws in the largest group: no recognisable world (menus, loads).
constexpr size_t kCameraMinWorldDraws = 16;

const char* const kCameraDepthSource = R"(#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(push_constant) uniform CameraDepthConstants {
  uvec2 size;
  // Host depth to guest z: float24 depth is kept halved.
  float scale;
  // The band's first row in the destination.
  uint dest_row;
} c;
#if MULTISAMPLED
layout(set = 0, binding = 0) uniform texture2DMS depth;
#else
layout(set = 0, binding = 0) uniform texture2D depth;
#endif
layout(set = 0, binding = 1, r32f) writeonly uniform image2D dest;
void main() {
  uvec2 pixel = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(pixel, c.size))) {
    return;
  }
  // Sample 0 of a multisampled surface.
  float z = texelFetch(depth, ivec2(pixel), 0).r * c.scale;
  imageStore(dest, ivec2(pixel.x, pixel.y + c.dest_row), vec4(z, 0.0, 0.0, 0.0));
}
)";

using Matrix = std::array<double, 16>;

Matrix ToMatrix(const std::array<float, 16>& m) {
  Matrix out;
  for (size_t i = 0; i < 16; ++i) out[i] = m[i];
  return out;
}

Matrix Multiply(const Matrix& a, const Matrix& b) {
  Matrix out = {};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      double sum = 0.0;
      for (int k = 0; k < 4; ++k) sum += a[r * 4 + k] * b[k * 4 + c];
      out[r * 4 + c] = sum;
    }
  }
  return out;
}

// Gauss-Jordan with partial pivoting; false when singular.
bool Invert(const Matrix& m, Matrix& out) {
  double a[4][8];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      a[r][c] = m[r * 4 + c];
      a[r][c + 4] = r == c ? 1.0 : 0.0;
    }
  }
  for (int col = 0; col < 4; ++col) {
    int pivot = col;
    for (int r = col + 1; r < 4; ++r) {
      if (std::abs(a[r][col]) > std::abs(a[pivot][col])) pivot = r;
    }
    if (std::abs(a[pivot][col]) < 1e-12) return false;
    if (pivot != col) {
      for (int c = 0; c < 8; ++c) std::swap(a[pivot][c], a[col][c]);
    }
    const double inv = 1.0 / a[col][col];
    for (int c = 0; c < 8; ++c) a[col][c] *= inv;
    for (int r = 0; r < 4; ++r) {
      if (r == col) continue;
      const double f = a[r][col];
      if (f == 0.0) continue;
      for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
    }
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r][c + 4];
  }
  return true;
}

// The radical inverse of index in base, in [0, 1).
float Halton(uint32_t index, uint32_t base) {
  float result = 0.0f;
  float fraction = 1.0f;
  while (index) {
    fraction /= float(base);
    result += fraction * float(index % base);
    index /= base;
  }
  return result;
}

double RowLength(const std::array<float, 16>& m, int row) {
  return std::sqrt(double(m[row * 4]) * m[row * 4] + double(m[row * 4 + 1]) * m[row * 4 + 1] +
                   double(m[row * 4 + 2]) * m[row * 4 + 2]);
}

struct MatrixHash {
  size_t operator()(const std::array<float, 16>& m) const {
    uint64_t h = 1469598103934665603ull;
    for (float f : m) {
      // -0 equals 0.
      const uint32_t bits = f == 0.0f ? 0u : std::bit_cast<uint32_t>(f);
      h ^= bits;
      h *= 1099511628211ull;
    }
    return size_t(h);
  }
};

}  // namespace

void Fh1NativeExecutor::NoteCameraDraw(const Fh1DrawInfo& draw) {
  draw_uses_camera_matrix_ = false;
  draw_is_scene_car_ = false;
  current_object_slot_ = 0;
  if (!camera_tracking_enabled_ || !initialized_ || !draw.vertex_shader || !draw.rasterization_done ||
      draw.memexport) {
    return;
  }
  const Shader::ConstantRegisterMap& map = draw.vertex_shader->constant_register_map();
  if (((map.float_bitmap[kCameraFirstRegister / 64] >> (kCameraFirstRegister % 64)) & 0xF) !=
      0xF) {
    // UP-1.7: a car, with its own matrix in c0-c3. Jittered with the scene
    // (it draws to its own colour target, so GetSceneJitter's colour check
    // left cars unjittered and aliased before).
    if (SceneObjectMatrixRegister(*draw.vertex_shader) != kObjectFirstRegister) return;
    draw_is_scene_car_ = true;
    if (object_motion_enabled_) {
      std::array<float, 16> wvp;
      std::memcpy(wvp.data(),
                  &register_file_.values[XE_GPU_REG_SHADER_CONSTANT_000_X +
                                         kObjectFirstRegister * 4],
                  sizeof(wvp));
      current_object_slot_ = AssignObjectSlot(DrawObjectKey(draw), wvp);
    }
    return;
  }
  draw_uses_camera_matrix_ = true;
  if (!draw.normalized_depth_control.z_enable) return;
  CameraDraw& noted = camera_draws_.emplace_back();
  std::memcpy(noted.wvp.data(),
              &register_file_.values[XE_GPU_REG_SHADER_CONSTANT_000_X + kCameraFirstRegister * 4],
              sizeof(noted.wvp));
  noted.object = DrawObjectKey(draw);
  noted.depth_key = CurrentDepthKey();
  noted.color_key = CurrentColorKey();
  if (noted.depth_key == camera_scene_depth_) camera_depth_dirty_ = true;
  if (object_motion_enabled_ && IsSceneObjectDraw(*draw.vertex_shader)) {
    current_object_slot_ = AssignObjectSlot(noted.object, noted.wvp);
  }
}

uint64_t Fh1NativeExecutor::DrawObjectKey(const Fh1DrawInfo& draw) const {
  // The object: the shader and its first vertex buffer.
  uint64_t object = draw.vertex_shader->ucode_data_hash();
  const auto& bindings = draw.vertex_shader->vertex_bindings();
  if (!bindings.empty()) {
    const xenos::xe_gpu_vertex_fetch_t fetch =
        register_file_.GetVertexFetch(bindings.front().fetch_constant);
    object ^= (uint64_t(fetch.address) << 2) * 0x9E3779B97F4A7C15ull;
  }
  return object;
}

uint32_t Fh1NativeExecutor::AssignObjectSlot(uint64_t object, const std::array<float, 16>& wvp) {
  // One mesh drawn several times a frame (a car's four wheels, or each
  // predicated tile's band) is told apart by its order among the draws of
  // that mesh, which a frame repeats.
  const uint64_t key = object ^ (uint64_t(object_occurrences_[object]++) + 1) *
                                    0xC2B2AE3D27D4EB4Full;
  object_current_wvp_.emplace(key, wvp);
  uint32_t slot = 0;
  // Objects new this frame keep the camera's motion (slot 0).
  auto previous = object_previous_wvp_.find(key);
  Matrix current_inverse;
  if (previous != object_previous_wvp_.end() && Invert(ToMatrix(wvp), current_inverse)) {
    const Matrix reprojection = Multiply(ToMatrix(previous->second), current_inverse);
    std::array<float, 16> matrix;
    for (size_t i = 0; i < 16; ++i) matrix[i] = float(reprojection[i]);
    // Objects that move with the world (most props and the world's own
    // pieces) reproject as the camera does up to rounding: one slot for all,
    // at 1/4096 of clip space (under 0.1 pixel at 1280x720).
    uint64_t hash = 1469598103934665603ull;
    for (double element : reprojection) {
      hash ^= uint64_t(std::llround(element * 4096.0));
      hash *= 1099511628211ull;
    }
    auto same = object_slot_by_matrix_.find(hash);
    if (same != object_slot_by_matrix_.end()) {
      slot = same->second;
    } else {
      if (object_reprojections_.empty()) object_reprojections_.emplace_back();
      if (object_reprojections_.size() < kMaxObjectSlots) {
        slot = uint32_t(object_reprojections_.size());
        object_reprojections_.push_back(matrix);
        object_slot_by_matrix_.emplace(hash, slot);
      } else {
        Count("object_slots_full");
      }
    }
  }
  return slot;
}

bool Fh1NativeExecutor::IsSceneObjectDraw(const Shader& vertex_shader) const {
  return object_motion_enabled_ && SceneObjectMatrixRegister(vertex_shader) != UINT32_MAX;
}

uint32_t Fh1NativeExecutor::SceneObjectMatrixRegister(const Shader& vertex_shader) const {
  if (!camera_tracking_enabled_ || !camera_scene_depth_valid_) return UINT32_MAX;
  const uint32_t matrix_register = DrawObjectMatrixRegister(vertex_shader);
  if (matrix_register == UINT32_MAX) return UINT32_MAX;
  // Only draws that write the scene's depth: what the swap pass compares the
  // slot's depth with. Transparency without depth writes keeps what is
  // behind it.
  const auto depth_control = register_file_.Get<reg::RB_DEPTHCONTROL>();
  if (!depth_control.z_enable || !depth_control.z_write_enable ||
      CurrentDepthKey() != camera_scene_depth_) {
    return UINT32_MAX;
  }
  return matrix_register;
}

uint32_t Fh1NativeExecutor::DrawObjectMatrixRegister(const Shader& vertex_shader) const {
  // The world's draws read the camera's world-view-projection at c128-c131;
  // cars (body, wheels, interior: 60-odd vertex shaders a frame on the
  // Fujimi drive) read their own at c0-c3 instead.
  const Shader::ConstantRegisterMap& map = vertex_shader.constant_register_map();
  if (((map.float_bitmap[kCameraFirstRegister / 64] >> (kCameraFirstRegister % 64)) & 0xF) ==
      0xF) {
    return kCameraFirstRegister;
  }
  if ((map.float_bitmap[0] & 0xF) == 0xF) return kObjectFirstRegister;
  return UINT32_MAX;
}

uint32_t Fh1NativeExecutor::CurrentDepthKey() const {
  const auto depth_info = register_file_.Get<reg::RB_DEPTH_INFO>();
  const auto surface_info = register_file_.Get<reg::RB_SURFACE_INFO>();
  const uint32_t msaa = uint32_t(surface_info.msaa_samples);
  return StorageKey(MakeDepthKey(depth_info.depth_base,
                                 Fh1PitchTiles(surface_info.surface_pitch, msaa), msaa,
                                 depth_info.depth_format))
      .Pack();
}

int32_t Fh1NativeExecutor::ObjectRowOffset() const {
  if (untile_height_) return 0;
  return std::max(0, -int32_t(register_file_.Get<reg::PA_SC_WINDOW_OFFSET>().window_y_offset)) *
         int32_t(scale_y_);
}

uint64_t Fh1NativeExecutor::CurrentColorKey() const {
  const auto color_info = register_file_.Get<reg::RB_COLOR_INFO>();
  const auto surface_info = register_file_.Get<reg::RB_SURFACE_INFO>();
  return uint64_t(color_info.color_base) | uint64_t(surface_info.surface_pitch) << 16 |
         uint64_t(surface_info.msaa_samples) << 32 | uint64_t(color_info.color_format) << 40;
}

void Fh1NativeExecutor::AdvanceJitter() {
  // The scene is drawn turned 180 degrees and the post pass turns it back,
  // so a surface offset moves the picture the other way on the screen. In
  // the upscalers' convention the frame's jitter is (-x, -y): of the four
  // signs it is the one that came closest to a native 3x render of a still
  // scene (PSNR, FSR 3.1.4 at 3x).
  camera_ready_.jitter_x = -jitter_x_;
  camera_ready_.jitter_y = -jitter_y_;
  if (jitter_phases_) {
    // With SetStereoCamera, the sequence of the eye the next frame is for.
    uint32_t& index = next_eye_ >= 0 ? jitter_index_eye_[next_eye_] : jitter_index_;
    index = (index + 1) % jitter_phases_;
    jitter_x_ = Halton(index + 1, 2) - 0.5f;
    jitter_y_ = Halton(index + 1, 3) - 0.5f;
  } else {
    jitter_x_ = 0.0f;
    jitter_y_ = 0.0f;
  }
}

bool Fh1NativeExecutor::GetSceneJitter(float& x, float& y) const {
  // Only draws placed by the camera matrix: a screen-space pass in the
  // scene (a quad ending at the screen edge) would leave a strip of the
  // edge unwritten when moved, which showed as flickering stale colours.
  if (!jitter_phases_ || !camera_scene_depth_valid_) return false;
  if (draw_is_scene_car_) {
    x = jitter_x_;
    y = jitter_y_;
    return true;
  }
  if (!draw_uses_camera_matrix_) return false;
  if (CurrentDepthKey() != camera_scene_depth_ || CurrentColorKey() != camera_scene_color_) {
    return false;
  }
  x = jitter_x_;
  y = jitter_y_;
  return true;
}

void Fh1NativeExecutor::FinishCameraFrame() {
  camera_ready_valid_ = false;
  lens_valid_ = false;
  camera_depth_dirty_ = false;
  std::vector<std::array<float, 16>> object_reprojections = std::move(object_reprojections_);
  object_reprojections_.clear();
  object_slot_by_matrix_.clear();
  object_occurrences_.clear();
  // This frame's object matrices: the next frame's previous ones, or with
  // SetStereoCamera those of this frame's eye, and the other eye's become
  // the next frame's.
  std::unordered_map<uint64_t, std::array<float, 16>> finished_wvp =
      std::move(object_current_wvp_);
  object_current_wvp_.clear();
  auto finish_objects = [&](int eye) {
    if (lens_eye_ && eye >= 0) {
      object_wvp_eye_[eye] = std::move(finished_wvp);
      next_eye_ = eye ^ 1;
      object_previous_wvp_ = std::move(object_wvp_eye_[next_eye_]);
    } else {
      object_previous_wvp_ = std::move(finished_wvp);
      next_eye_ = -1;
    }
  };
  auto lose_eyes = [&] {
    for (auto& objects : camera_previous_objects_eye_) objects.clear();
    finish_objects(-1);
  };
  object_reprojections_ready_.clear();
  if (!camera_tracking_enabled_ || camera_draws_.empty()) {
    camera_scene_depth_valid_ = false;
    camera_draws_.clear();
    camera_previous_objects_.clear();
    lose_eyes();
    return;
  }
  // The scene's depth: the one most of the matrix draws test against.
  std::unordered_map<uint32_t, size_t> depth_counts;
  for (const CameraDraw& noted : camera_draws_) ++depth_counts[noted.depth_key];
  const uint32_t scene_depth =
      std::max_element(depth_counts.begin(), depth_counts.end(),
                       [](const auto& a, const auto& b) { return a.second < b.second; })
          ->first;
  // The next frame's bands copy it (shadow and reflection depths come
  // first in a frame, so a running count would pick them).
  camera_scene_depth_ = scene_depth;
  camera_scene_depth_valid_ = true;
  std::unordered_map<uint64_t, size_t> color_counts;
  for (const CameraDraw& noted : camera_draws_) {
    if (noted.depth_key == scene_depth) ++color_counts[noted.color_key];
  }
  camera_scene_color_ =
      std::max_element(color_counts.begin(), color_counts.end(),
                       [](const auto& a, const auto& b) { return a.second < b.second; })
          ->first;
  // The world: the largest group of scene draws sharing one matrix.
  std::unordered_map<std::array<float, 16>, size_t, MatrixHash> groups;
  std::unordered_map<uint64_t, std::array<float, 16>> objects;
  std::unordered_map<uint64_t, bool> ambiguous;
  for (const CameraDraw& noted : camera_draws_) {
    if (noted.depth_key != scene_depth) continue;
    ++groups[noted.wvp];
    auto [it, inserted] = objects.emplace(noted.object, noted.wvp);
    // An object drawn with two matrices (instances) cannot be matched.
    if (!inserted && it->second != noted.wvp) ambiguous[noted.object] = true;
  }
  camera_draws_.clear();
  for (const auto& [object, unused] : ambiguous) objects.erase(object);
  const auto world =
      std::max_element(groups.begin(), groups.end(),
                       [](const auto& a, const auto& b) { return a.second < b.second; });
  if (world == groups.end() || world->second < kCameraMinWorldDraws) {
    camera_previous_objects_.clear();
    lose_eyes();
    return;
  }
  const std::array<float, 16>& current = world->first;

  // The lens, from the world's matrix (its world transform has unit scale).
  Camera camera = {};
  const double y_scale = RowLength(current, 1);
  const double z_length = RowLength(current, 2);
  const double w_length = RowLength(current, 3);
  if (y_scale <= 0.0 || w_length <= 0.0) {
    camera_previous_objects_ = std::move(objects);
    lose_eyes();
    return;
  }
  camera.fov_y = float(2.0 * std::atan(1.0 / y_scale));
  const int eye = lens_eye_ ? lens_eye_(camera.fov_y) : -1;
  camera.eye = eye;
  std::unordered_map<uint64_t, std::array<float, 16>>& previous_objects =
      eye >= 0 ? camera_previous_objects_eye_[eye] : camera_previous_objects_;
  // The OpenXR output reads the tag its host put in the field of view.
  const double x_scale = RowLength(current, 0);
  if (x_scale > 0.0) {
    lens_fov_x_ = float(2.0 * std::atan(1.0 / x_scale));
    lens_fov_y_ = camera.fov_y;
    lens_valid_ = true;
    // The camera: the point the x, y and w rows send to 0.
    const double a[3][3] = {{current[0], current[1], current[2]},
                            {current[4], current[5], current[6]},
                            {current[12], current[13], current[14]}};
    const double b[3] = {-current[3], -current[7], -current[15]};
    auto det3 = [](const double m[3][3]) {
      return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
             m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
             m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    };
    const double d = det3(a);
    lens_has_position_ = std::abs(d) > 1e-12;
    for (int c = 0; c < 3 && lens_has_position_; ++c) {
      double m[3][3];
      for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) m[r][k] = k == c ? b[r] : a[r][k];
      }
      lens_position_[c] = float(det3(m) / d);
      // The scene is drawn turned 180 degrees (clip x is the screen's
      // negated), so the x row points to the viewer's left.
      lens_right_[c] = float(-current[c] / x_scale);
    }
  }
  const double k = z_length / w_length;
  const double near_plane = (k * current[15] / w_length - current[11] / w_length) / k;
  camera.near_plane = float(near_plane);
  camera.far_plane = k > 1.0 ? float(k * near_plane / (k - 1.0)) : 0.0f;

  // The previous frame's matrix for the same objects.
  std::unordered_map<std::array<float, 16>, size_t, MatrixHash> previous_matches;
  size_t matched = 0;
  for (const auto& [object, wvp] : objects) {
    if (wvp != current) continue;
    auto previous = previous_objects.find(object);
    if (previous == previous_objects.end()) continue;
    ++previous_matches[previous->second];
    ++matched;
  }
  camera.cut = true;
  Matrix reprojection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  if (!previous_matches.empty()) {
    const auto previous =
        std::max_element(previous_matches.begin(), previous_matches.end(),
                         [](const auto& a, const auto& b) { return a.second < b.second; });
    Matrix current_inverse;
    if (previous->second >= kCameraMinWorldDraws / 2 && previous->second * 2 >= matched &&
        Invert(ToMatrix(current), current_inverse)) {
      const Matrix candidate = Multiply(ToMatrix(previous->first), current_inverse);
      // A jump of the view between frames is a cut even when the world
      // carries over: points 2, 10 and 100 m away moving a quarter of the
      // screen or more, or to behind the camera. (The matrix's own entries
      // are no measure: with a 0.1 m near plane, driving moves its depth
      // column by more than any turn, and in VR an eye's previous frame is
      // two frames back; at speed nearly every frame counted as a cut.)
      bool jump = false;
      for (const double distance : {2.0, 10.0, 100.0}) {
        // The matrix's clip z is the usual D3D depth.
        const double z = camera.far_plane > camera.near_plane
                             ? camera.far_plane * (distance - camera.near_plane) /
                                   (distance * (camera.far_plane - camera.near_plane))
                             : 1.0 - camera.near_plane / distance;
        for (const double x : {-0.6, 0.0, 0.6}) {
          for (const double y : {-0.6, 0.6}) {
            const double v[4] = {x, y, z, 1.0};
            double out[4] = {};
            for (int r = 0; r < 4; ++r) {
              for (int c = 0; c < 4; ++c) out[r] += candidate[r * 4 + c] * v[c];
            }
            if (!(out[3] > 1.0e-6) || std::abs(out[0] / out[3] - x) >= 0.5 ||
                std::abs(out[1] / out[3] - y) >= 0.5) {
              jump = true;
            }
          }
        }
      }
      if (!jump) {
        reprojection = candidate;
        camera.cut = false;
      } else {
        Count("camera_cut_jump");
      }
    } else {
      Count("camera_cut_unmatched");
    }
  } else {
    Count("camera_cut_unmatched");
  }
  for (size_t i = 0; i < 16; ++i) camera.reprojection[i] = float(reprojection[i]);
  if (camera.cut) Count("camera_cut");
  Count("camera_frame");
  if ((frame_ % 600) == 0) {
    REXGPU_INFO(
        "FH1 camera: world group {} draws, {} matched, cut {}, fov_y {:.2f} deg, near {:.3f}, "
        "far {:.0f}",
        world->second, matched, camera.cut, camera.fov_y * 57.29578f, camera.near_plane,
        camera.far_plane);
  }
  camera_ready_ = camera;
  camera_ready_valid_ = true;
  previous_objects = std::move(objects);
  finish_objects(eye);
  if (!camera.cut) {
    if (object_reprojections.size() > 1) Count("camera_object_frame");
    if ((frame_ % 600) == 0) {
      REXGPU_INFO("FH1 camera: {} object reprojections",
                  object_reprojections.empty() ? 0 : object_reprojections.size() - 1);
    }
    object_reprojections_ready_ = std::move(object_reprojections);
  }
}

bool Fh1NativeExecutor::EnsureCameraDepthPipelines() {
  if (camera_depth_pipelines_[0] != VK_NULL_HANDLE) return true;
  if (camera_depth_failed_) return false;
  camera_depth_failed_ = true;
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkDescriptorSetLayoutBinding bindings[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType =
        i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo set_layout_info = {};
  set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout_info.bindingCount = 2;
  set_layout_info.pBindings = bindings;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                      &camera_depth_set_layout_) != VK_SUCCESS) {
    return false;
  }
  VkPushConstantRange push_constant_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                             sizeof(uint32_t) * 3};
  VkPipelineLayoutCreateInfo pipeline_layout_info = {};
  pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = &camera_depth_set_layout_;
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr,
                                 &camera_depth_pipeline_layout_) != VK_SUCCESS) {
    return false;
  }
  const uint32_t set_count = uint32_t(camera_depth_sets_.size());
  VkDescriptorPoolSize pool_sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, set_count},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, set_count}};
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = set_count;
  pool_info.poolSizeCount = 2;
  pool_info.pPoolSizes = pool_sizes;
  if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &camera_depth_descriptor_pool_) !=
      VK_SUCCESS) {
    return false;
  }
  std::array<VkDescriptorSetLayout, 4> layouts;
  layouts.fill(camera_depth_set_layout_);
  VkDescriptorSetAllocateInfo set_info = {};
  set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  set_info.descriptorPool = camera_depth_descriptor_pool_;
  set_info.descriptorSetCount = set_count;
  set_info.pSetLayouts = layouts.data();
  if (dfn.vkAllocateDescriptorSets(device, &set_info, camera_depth_sets_.data()) != VK_SUCCESS) {
    return false;
  }
  for (uint32_t multisampled = 0; multisampled < 2; ++multisampled) {
    std::string source = kCameraDepthSource;
    source.insert(source.find('\n') + 1,
                  std::string("#define MULTISAMPLED ") + (multisampled ? "1\n" : "0\n"));
    std::vector<uint32_t> spirv;
    std::string error;
    if (!command_processor_.CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, source, spirv,
                                               error)) {
      REXGPU_WARN("Failed to compile the camera depth shader: {}", error);
      return false;
    }
    camera_depth_pipelines_[multisampled] = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, camera_depth_pipeline_layout_, spirv.data(),
        spirv.size() * sizeof(uint32_t));
    if (camera_depth_pipelines_[multisampled] == VK_NULL_HANDLE) return false;
  }
  camera_depth_failed_ = false;
  return true;
}

void Fh1NativeExecutor::SnapshotCameraDepth(uint32_t band_row) {
  if (!camera_tracking_enabled_ || !camera_depth_dirty_ || !camera_scene_depth_valid_) return;
  Surface* surface = FindSurface(camera_scene_depth_);
  if (!surface || !surface->key.is_depth || surface->sampled_view == VK_NULL_HANDLE ||
      !EnsureCameraDepthPipelines()) {
    return;
  }
  camera_depth_dirty_ = false;
  // A band holds the surface's rows. The copy is resized only before a
  // frame's first band, to the bottom of the lowest band seen, so frames
  // with a taller band than it has are dropped until the next.
  const uint32_t width = surface->width * scale_x_;
  const uint32_t height = std::min(surface->height, surface->image_height) * scale_y_;
  const uint32_t dest_row = band_row * scale_y_;
  camera_depth_wanted_height_ = std::max(camera_depth_wanted_height_, dest_row + height);
  if (camera_depth_rows_ &&
      (camera_depth_.width != width || camera_depth_.height < dest_row + height)) {
    Count("camera_depth_band_unfit");
    return;
  }
  const uint32_t full_height = camera_depth_wanted_height_;
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  command_processor_.EndRenderPass();
  if (camera_depth_.image != VK_NULL_HANDLE &&
      (camera_depth_.width != width || camera_depth_.height != full_height)) {
    camera_depth_rows_ = 0;
    command_processor_.Fh1AwaitAllQueueOperations();
    if (camera_depth_.view) dfn.vkDestroyImageView(device, camera_depth_.view, nullptr);
    if (camera_depth_.image) dfn.vkDestroyImage(device, camera_depth_.image, nullptr);
    if (camera_depth_.memory) dfn.vkFreeMemory(device, camera_depth_.memory, nullptr);
    camera_depth_ = Surface();
  }
  if (camera_depth_.image == VK_NULL_HANDLE) {
    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R32_SFLOAT;
    image_info.extent = {width, full_height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!ui::vulkan::util::CreateDedicatedAllocationImage(
            vulkan_device, image_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal,
            camera_depth_.image, camera_depth_.memory)) {
      Skip("camera_depth_create");
      camera_depth_ = Surface();
      return;
    }
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = camera_depth_.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R32_SFLOAT;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (dfn.vkCreateImageView(device, &view_info, nullptr, &camera_depth_.view) != VK_SUCCESS) {
      dfn.vkDestroyImage(device, camera_depth_.image, nullptr);
      dfn.vkFreeMemory(device, camera_depth_.memory, nullptr);
      camera_depth_ = Surface();
      Skip("camera_depth_create");
      return;
    }
    camera_depth_.format = VK_FORMAT_R32_SFLOAT;
    camera_depth_.width = width;
    camera_depth_.height = full_height;
  }
  TransitionForSampling(*surface, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  Transition(camera_depth_, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
             VK_ACCESS_SHADER_WRITE_BIT);
  command_processor_.SubmitBarriers(true);
  const VkDescriptorSet set = camera_depth_sets_[frame_ % camera_depth_sets_.size()];
  VkDescriptorImageInfo images[2] = {
      {VK_NULL_HANDLE, surface->sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {VK_NULL_HANDLE, camera_depth_.view, VK_IMAGE_LAYOUT_GENERAL},
  };
  VkWriteDescriptorSet writes[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType =
        i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[i].pImageInfo = &images[i];
  }
  dfn.vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();
  command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                         camera_depth_pipeline_layout_, 0, 1, &set, 0, nullptr);
  struct {
    uint32_t size[2];
    float scale;
    uint32_t dest_row;
  } constants = {{width, height},
                 xenos::DepthRenderTargetFormat(surface->key.format) ==
                         xenos::DepthRenderTargetFormat::kD24FS8
                     ? 2.0f
                     : 1.0f,
                 dest_row};
  camera_depth_scale_ = constants.scale;
  command_buffer.CmdVkPushConstants(camera_depth_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                    sizeof(constants), &constants);
  command_processor_.BindExternalComputePipeline(camera_depth_pipelines_[surface->samples > 1]);
  command_buffer.CmdVkDispatch((width + 7) / 8, (height + 7) / 8, 1);
  camera_depth_rows_ = std::max(camera_depth_rows_, dest_row + height);
  Count("camera_depth_snapshot");
}

VkImageView Fh1NativeExecutor::TakeCameraFrame(uint32_t width, uint32_t height,
                                               Camera& camera_out) {
  const bool depth_ready = camera_depth_rows_ready_ >= height;
  const bool ready = depth_ready && camera_ready_valid_;
  camera_depth_rows_ready_ = 0;
  if (!ready || camera_depth_.view == VK_NULL_HANDLE || camera_depth_.width < width ||
      camera_depth_.height < height) {
    Count(!depth_ready && !camera_ready_valid_ ? "camera_take_none"
          : !camera_ready_valid_                       ? "camera_take_no_matrix"
          : !ready                                     ? "camera_take_no_depth"
                                                       : "camera_take_small");
    return VK_NULL_HANDLE;
  }
  Count("camera_take");
  camera_out = camera_ready_;
  TransitionForSampling(camera_depth_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  return camera_depth_.view;
}

void Fh1NativeExecutor::DestroyCamera() {
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (camera_depth_.view) dfn.vkDestroyImageView(device, camera_depth_.view, nullptr);
  if (camera_depth_.image) dfn.vkDestroyImage(device, camera_depth_.image, nullptr);
  if (camera_depth_.memory) dfn.vkFreeMemory(device, camera_depth_.memory, nullptr);
  camera_depth_ = Surface();
  camera_depth_rows_ = 0;
  camera_depth_rows_ready_ = 0;
  for (VkPipeline& pipeline : camera_depth_pipelines_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, pipeline);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         camera_depth_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         camera_depth_descriptor_pool_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         camera_depth_set_layout_);
}

}  // namespace rex::graphics::vulkan
