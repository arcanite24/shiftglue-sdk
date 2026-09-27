#pragma once

#include <string_view>

namespace rex::kernel::xboxkrnl {

// Receives the translated guest path of every NtCreateFile/NtOpenFile call,
// including calls the title makes through function pointers. The observer
// runs on the calling guest thread before the path is resolved; it must be
// cheap and must not call back into the file system. Pass nullptr to remove.
using GuestFileOpenObserver = void (*)(std::string_view guest_path);
void SetGuestFileOpenObserver(GuestFileOpenObserver observer);

}  // namespace rex::kernel::xboxkrnl
