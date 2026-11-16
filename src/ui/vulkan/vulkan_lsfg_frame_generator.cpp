/**
 * Lossless Scaling frame generation (UP-5.2), the provider slot.
 *
 * LSFG runs from the player's own Lossless.dll (docs/LEGAL.md in Pinyon
 * Shift: it is never fetched, shipped or cached). This finds the DLL (a path
 * the player sets, or the Lossless Scaling folder in their Steam libraries)
 * and counts the DXBC shader containers in its resources without copying
 * them. Running them needs a DXBC to SPIR-V translation that is not written
 * yet, so the provider always reports itself unavailable, with the reason.
 */

#include <rex/ui/vulkan/frame_generator.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>

REXCVAR_DEFINE_STRING(present_lsfg_dll, "", "UI/Presenter",
                      "Path to the player's own Lossless.dll for LSFG frame generation; empty "
                      "looks in the Steam library folders")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex {
namespace ui {
namespace vulkan {

namespace {

namespace fs = std::filesystem;

std::string GetEnvironment(const char* name) {
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
}

// The Steam library folders: each install's own folder, then the paths its
// libraryfolders.vdf lists.
std::vector<fs::path> GetSteamLibraries() {
  std::vector<fs::path> roots;
#if defined(_WIN32)
  for (const char* variable : {"ProgramFiles(x86)", "ProgramFiles"}) {
    const std::string base = GetEnvironment(variable);
    if (!base.empty()) {
      roots.push_back(fs::path(base) / "Steam");
    }
  }
#else
  const std::string home = GetEnvironment("HOME");
  if (!home.empty()) {
    roots.push_back(fs::path(home) / ".local/share/Steam");
    roots.push_back(fs::path(home) / ".steam/steam");
    roots.push_back(fs::path(home) / ".var/app/com.valvesoftware.Steam/.local/share/Steam");
  }
#endif
  std::vector<fs::path> libraries;
  auto add = [&libraries](const fs::path& path) {
    std::error_code error;
    if (!fs::is_directory(path / "steamapps", error)) {
      return;
    }
    fs::path key = fs::weakly_canonical(path, error);
    if (error) {
      key = path;
    }
    if (std::find(libraries.begin(), libraries.end(), key) == libraries.end()) {
      libraries.push_back(key);
    }
  };
  for (const fs::path& root : roots) {
    add(root);
    // Lines like: "path"  "D:\\SteamLibrary" (backslashes escaped).
    std::ifstream vdf(root / "steamapps" / "libraryfolders.vdf");
    std::string line;
    while (std::getline(vdf, line)) {
      const size_t key = line.find("\"path\"");
      if (key == std::string::npos) {
        continue;
      }
      const size_t open = line.find('"', key + 6);
      const size_t close = open == std::string::npos ? open : line.find('"', open + 1);
      if (close == std::string::npos) {
        continue;
      }
      std::string value;
      for (size_t i = open + 1; i < close; ++i) {
        if (line[i] == '\\' && i + 1 < close) {
          ++i;
        }
        value.push_back(line[i]);
      }
      add(fs::path(value));
    }
  }
  return libraries;
}

fs::path FindLosslessDll() {
  std::error_code error;
  const std::string configured = REXCVAR_GET(present_lsfg_dll);
  if (!configured.empty()) {
    return fs::is_regular_file(configured, error) ? fs::path(configured) : fs::path();
  }
  for (const fs::path& library : GetSteamLibraries()) {
    const fs::path dll = library / "steamapps" / "common" / "Lossless Scaling" / "Lossless.dll";
    if (fs::is_regular_file(dll, error)) {
      return dll;
    }
  }
  return {};
}

template <typename T>
bool ReadAt(const std::vector<uint8_t>& data, size_t offset, T* value) {
  if (offset > data.size() || data.size() - offset < sizeof(T)) {
    return false;
  }
  std::memcpy(value, data.data() + offset, sizeof(T));
  return true;
}

// The number of resources in a PE file's resource tree that start with a
// DXBC container header, or -1 when the file is not a PE file with
// resources. Nothing is copied out.
int CountDxbcResources(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
  uint16_t mz = 0;
  uint32_t pe_offset = 0;
  uint32_t signature = 0;
  if (!ReadAt(data, 0, &mz) || mz != 0x5A4D || !ReadAt(data, 0x3C, &pe_offset) ||
      !ReadAt(data, pe_offset, &signature) || signature != 0x00004550) {
    return -1;
  }
  const size_t file_header = size_t(pe_offset) + 4;
  const size_t optional = file_header + 20;
  uint16_t section_count = 0;
  uint16_t optional_size = 0;
  uint16_t magic = 0;
  if (!ReadAt(data, file_header + 2, &section_count) ||
      !ReadAt(data, file_header + 16, &optional_size) || !ReadAt(data, optional, &magic)) {
    return -1;
  }
  // The data directories follow the PE32 (0x10B) or PE32+ (0x20B) fields;
  // the resource directory is the third.
  const size_t directories = optional + (magic == 0x20B ? 112 : 96);
  uint32_t resource_rva = 0;
  if (!ReadAt(data, directories + 2 * 8, &resource_rva) || !resource_rva) {
    return -1;
  }
  struct Section {
    uint32_t virtual_address = 0;
    uint32_t virtual_size = 0;
    uint32_t raw_size = 0;
    uint32_t raw_offset = 0;
  };
  std::vector<Section> sections;
  const size_t section_table = optional + optional_size;
  for (uint16_t i = 0; i < section_count; ++i) {
    const size_t entry = section_table + size_t(i) * 40;
    Section section;
    if (!ReadAt(data, entry + 8, &section.virtual_size) ||
        !ReadAt(data, entry + 12, &section.virtual_address) ||
        !ReadAt(data, entry + 16, &section.raw_size) ||
        !ReadAt(data, entry + 20, &section.raw_offset)) {
      return -1;
    }
    sections.push_back(section);
  }
  auto to_offset = [&sections](uint32_t rva, size_t* offset) {
    for (const Section& section : sections) {
      const uint32_t size = std::max(section.virtual_size, section.raw_size);
      if (rva >= section.virtual_address && rva - section.virtual_address < size) {
        *offset = size_t(section.raw_offset) + (rva - section.virtual_address);
        return true;
      }
    }
    return false;
  };
  size_t resource_base = 0;
  if (!to_offset(resource_rva, &resource_base)) {
    return -1;
  }
  int count = 0;
  // Type, name and language directories: three levels.
  std::function<void(size_t, int)> walk = [&](size_t directory, int depth) {
    uint16_t named = 0;
    uint16_t numbered = 0;
    if (depth > 3 || !ReadAt(data, directory + 12, &named) ||
        !ReadAt(data, directory + 14, &numbered)) {
      return;
    }
    for (uint32_t i = 0; i < uint32_t(named) + numbered; ++i) {
      uint32_t target = 0;
      if (!ReadAt(data, directory + 16 + size_t(i) * 8 + 4, &target)) {
        return;
      }
      if (target & 0x80000000u) {
        walk(resource_base + (target & 0x7FFFFFFFu), depth + 1);
        continue;
      }
      uint32_t data_rva = 0;
      uint32_t data_size = 0;
      size_t data_offset = 0;
      char header[4] = {};
      if (ReadAt(data, resource_base + target, &data_rva) &&
          ReadAt(data, resource_base + target + 4, &data_size) && data_size >= 4 &&
          to_offset(data_rva, &data_offset) && ReadAt(data, data_offset, &header) &&
          !std::memcmp(header, "DXBC", 4)) {
        ++count;
      }
    }
  };
  walk(resource_base, 1);
  return count;
}

class VulkanLsfgFrameGenerator final : public VulkanFrameGenerator {
 public:
  VulkanLsfgFrameGenerator() {
    info_.id = "lsfg";
    info_.name = "Lossless Scaling frame generation";
    const fs::path dll = FindLosslessDll();
    if (dll.empty()) {
      info_.unavailable_reason = "no Lossless.dll found";
      return;
    }
    const int shaders = CountDxbcResources(dll);
    REXLOG_INFO("LSFG: {} has {} DXBC shaders", dll.string(), shaders);
    info_.unavailable_reason = shaders > 0
                                   ? "Lossless.dll found; its shaders are not translated yet"
                                   : "Lossless.dll has no shaders this version can read";
  }

  const FrameGeneratorInfo& info() const override { return info_; }
  bool Prepare(uint32_t, uint32_t, uint32_t, uint32_t) override { return false; }
  bool Dispatch(VkCommandBuffer, const VulkanFrameGeneratorInputs&) override { return false; }
  void Release() override {}

 private:
  FrameGeneratorInfo info_;
};

}  // namespace

std::unique_ptr<VulkanFrameGenerator> CreateVulkanLsfgFrameGenerator(const VulkanDevice*) {
  return std::make_unique<VulkanLsfgFrameGenerator>();
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
