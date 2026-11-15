#pragma once

namespace rex::platform {

// Asks the kernel to back the executable segments of every loaded object with
// 2 MiB pages (desktop Linux; a no-op elsewhere). The executable and the plugins are
// tens of MiB of code, so instruction fetches miss the TLB often with 4 KiB
// pages. Only the 2 MiB-aligned interior of each segment can be collapsed,
// and only when the file offset and the address agree modulo 2 MiB (link with
// -z max-page-size=0x200000). Objects already handled are skipped, so call it
// again after loading a plugin. Controlled by the code_huge_pages cvar.
void BackCodeWithHugePages();

}  // namespace rex::platform
