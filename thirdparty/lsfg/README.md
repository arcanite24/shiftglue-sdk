# LSFG frame generation

Runs Lossless Scaling's LSFG 3.1 frame generation inside ShiftGlue's Vulkan
presenter (`src/ui/vulkan/vulkan_lsfg_frame_generator.cpp`). The shaders are
not here and are never fetched or shipped. They are read at run time from the
player's own `Lossless.dll` and translated in memory.

| Folder | What it is | Source | Licence |
|---|---|---|---|
| `framegen/` | The LSFG 3.1 and 3.1 performance-mode compute passes | [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk) 1.0.0 (`7113d7d`), `framegen/` | MIT, `framegen/LICENSE.md` |
| `dxbc/` | DXBC to SPIR-V compiler | [DXVK](https://github.com/doitsujin/dxvk) 2.6.2, `src/dxbc`, `src/spirv` and part of `src/util` | zlib, `dxbc/LICENSE` |
| `loader/` | Reads the DLL's RCDATA resources and translates one shader | Ours. The shader name table and binding renumbering follow lsfg-vk 1.0.0 `src/extract` | MIT (lsfg-vk) |

## Changes from upstream

lsfg-vk ran the passes on a Vulkan device of its own, in a layer, and shared
images and semaphores with the game through file descriptors. Here:

- `framegen/shim/volk.h` replaces volk. The `vk*` calls go through one
  function table filled from the presenter's device.
- `Core::Device` wraps the presenter's device. lsfg-vk's own instance, device
  creation, semaphores, fences, queue submits and DDS upload are removed.
- `Core::CommandBuffer` can wrap the presenter's command buffer, which is
  already recording.
- Barriers use `vkCmdPipelineBarrier` (Vulkan 1.0), not synchronization2.
- Generated frames go to plain images that are copied out. They are not
  exported memory.
- `framegen/embed/` takes the place of `context.cpp` and `lsfg.cpp`. It
  copies the real frame into the history, records both of lsfg-vk's command
  buffers into one, and copies the generated frame out. It makes one frame per
  real frame.

DXVK's compiler is cut down to stand alone. It has no D3D11 device options,
it uses a small logger, `compile()` returns the SPIR-V buffer, and only the
`util` headers it needs are kept. The upstream DXBC fork that lsfg-vk used is
no longer published.

The translated shaders need Vulkan 1.3 with `vulkanMemoryModel`,
`shaderStorageImageReadWithoutFormat`, `shaderStorageImageWriteWithoutFormat`
and `nullDescriptor`. The provider reports what is missing.
