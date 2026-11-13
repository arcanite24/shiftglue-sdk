#pragma once
// Live shader and pipeline compilation counters, written by the pipeline
// cache and command processor from their own threads. Plugin-internal: the
// singleton is not visible across the plugin library boundary, so the host
// reads it as a plain snapshot through IGraphicsSystem::shader_compile_status
// (system/interfaces/graphics.h). Reporting only; nothing here changes what
// the renderer does.

#include <atomic>
#include <cstdint>

namespace rex::graphics {

struct ShaderCompileStatus {
  // Pipelines read back from the on-disk storage at start, and how many of
  // them are built so far. Both zero when the storage was empty.
  std::atomic<uint32_t> startup_pipelines_total{0};
  std::atomic<uint32_t> startup_pipelines_done{0};
  // Pipelines queued for the asynchronous creation workers (first-seen
  // state during play) that are not built yet, and shaders still being
  // translated. Frames that would draw with one of them are not presented.
  std::atomic<uint32_t> pending_pipelines{0};
  std::atomic<uint32_t> pending_translations{0};
  // Frames held back from presentation because a draw used a placeholder
  // pipeline, and the last frame the renderer did present. Monotonic.
  std::atomic<uint64_t> frames_held{0};
  std::atomic<uint64_t> frames_presented{0};

  // Anything the player would otherwise see as a stall.
  bool busy() const {
    const uint32_t total = startup_pipelines_total.load(std::memory_order_relaxed);
    return (total && startup_pipelines_done.load(std::memory_order_relaxed) < total) ||
           pending_pipelines.load(std::memory_order_relaxed) ||
           pending_translations.load(std::memory_order_relaxed);
  }

  static ShaderCompileStatus& Get() {
    static ShaderCompileStatus status;
    return status;
  }
};

}  // namespace rex::graphics
