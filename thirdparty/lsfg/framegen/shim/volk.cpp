// The table itself, so the vk* aliases stay off here.
#define LSFG_VK_NO_ALIASES
#include "volk.h"

namespace LSFG {

VkTable g_vk;

bool LoadVkTable(PFN_vkGetInstanceProcAddr get_instance_proc_addr, VkInstance instance,
                 PFN_vkGetDeviceProcAddr get_device_proc_addr, VkDevice device) {
  VkTable table;
  bool ok = true;
#define LSFG_VK_LOAD_INSTANCE(name)                                                 \
  table.name = reinterpret_cast<PFN_##name>(get_instance_proc_addr(instance, #name)); \
  ok = ok && table.name;
#define LSFG_VK_LOAD_DEVICE(name)                                               \
  table.name = reinterpret_cast<PFN_##name>(get_device_proc_addr(device, #name)); \
  ok = ok && table.name;
  LSFG_VK_INSTANCE_FUNCTIONS(LSFG_VK_LOAD_INSTANCE)
  LSFG_VK_DEVICE_FUNCTIONS(LSFG_VK_LOAD_DEVICE)
#undef LSFG_VK_LOAD_DEVICE
#undef LSFG_VK_LOAD_INSTANCE
  if (ok) {
    g_vk = table;
  }
  return ok;
}

}  // namespace LSFG
