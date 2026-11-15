#include <rex/platform/huge_page_code.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

REXCVAR_DEFINE_BOOL(code_huge_pages, true, "Memory",
                    "Back the executable code of the game and its libraries with 2 MiB pages "
                    "(Linux): fewer instruction TLB misses in tens of MiB of code")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

#if REX_PLATFORM_LINUX && !REX_PLATFORM_ANDROID

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <link.h>
#include <unistd.h>
#include <sys/mman.h>

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

namespace rex::platform {

namespace {

struct CodeRange {
  std::string name;
  uintptr_t base;
  uintptr_t begin;
  uintptr_t end;
};

constexpr uintptr_t kHugePage = uintptr_t(2) << 20;

struct Collect {
  std::string own_dir;
  std::vector<CodeRange> ranges;
};

int CollectCodeRanges(dl_phdr_info* info, size_t, void* data) {
  auto& collect = *static_cast<Collect*>(data);
  auto& ranges = collect.ranges;
  const bool is_main = !info->dlpi_name || !info->dlpi_name[0];
  const char* name = is_main ? "<main>" : info->dlpi_name;
  // Only the game and the libraries beside it: system and Steam libraries are
  // not linked for this and are not ours to change.
  if (!is_main && (collect.own_dir.empty() || std::strncmp(name, collect.own_dir.c_str(),
                                                            collect.own_dir.size()) != 0)) {
    return 0;
  }
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr)& ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_X)) {
      continue;
    }
    // The page cache can map a file page as part of a 2 MiB page only where
    // the address and the file offset agree modulo 2 MiB.
    const uintptr_t start = info->dlpi_addr + ph.p_vaddr;
    if ((start - ph.p_offset) & (kHugePage - 1)) {
      ranges.push_back({name, info->dlpi_addr, 0, 0});
      continue;
    }
    const uintptr_t begin = (start + kHugePage - 1) & ~(kHugePage - 1);
    const uintptr_t end = (start + ph.p_memsz) & ~(kHugePage - 1);
    ranges.push_back({name, info->dlpi_addr, begin, end > begin ? end : begin});
  }
  return 0;
}

}  // namespace

void BackCodeWithHugePages() {
  if (!REXCVAR_GET(code_huge_pages)) {
    return;
  }
  static std::mutex mutex;
  static std::set<std::pair<uintptr_t, uintptr_t>> done;
  std::lock_guard<std::mutex> lock(mutex);
  Collect collect;
  {
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
      std::string path(exe, size_t(n));
      collect.own_dir = path.substr(0, path.rfind('/') + 1);
    }
  }
  dl_iterate_phdr(CollectCodeRanges, &collect);
  for (const CodeRange& range : collect.ranges) {
    if (!done.insert({range.base, range.begin}).second) {
      continue;
    }
    if (range.end == range.begin) {
      if (!range.begin) {
        REXLOG_INFO("Huge-page code: {} is not linked 2 MiB-aligned; left on 4 KiB pages",
                    range.name);
      }
      continue;
    }
    const size_t length = range.end - range.begin;
    void* address = reinterpret_cast<void*>(range.begin);
    if (madvise(address, length, MADV_HUGEPAGE) != 0) {
      const int error = errno;
      REXLOG_INFO("Huge-page code: MADV_HUGEPAGE on {} failed ({})", range.name,
                  std::strerror(error));
      continue;
    }
    // Collapse now, one 2 MiB page at a time, rather than waiting for
    // khugepaged's scan (which still collapses what is refused here). Needs
    // a kernel with read-only file THP (6.1+).
    size_t collapsed = 0;
    int error = 0;
    for (uintptr_t page = range.begin; page < range.end; page += kHugePage) {
      if (madvise(reinterpret_cast<void*>(page), kHugePage, MADV_COLLAPSE) == 0) {
        ++collapsed;
      } else if (!error) {
        error = errno;
      }
    }
    REXLOG_INFO("Huge-page code: {} of {} 2 MiB pages of {} collapsed{}{}", collapsed,
                length / kHugePage, range.name, error ? ", first refusal: " : "",
                error ? std::strerror(error) : "");
  }
}

}  // namespace rex::platform

#else

namespace rex::platform {
void BackCodeWithHugePages() {}
}  // namespace rex::platform

#endif
