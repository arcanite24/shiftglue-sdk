#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <rex/graphics/register_file.h>
#include <rex/memory.h>

namespace rex::graphics {

class CommandProcessor;
class SharedMemory;

// Offline replay of an FH1 frame dump (Fh1FrameDump records them) on
// any backend: with no title running, the dump's guest ranges are written to
// guest memory, the registers and active shaders restored and the packets
// executed; the front buffer `read_front_buffer` returns (the GPU's copy of
// that guest range after the replayed swap) is compared with the recorded one
// and written next to the dump with a JSON summary.
//
// Runs fh1_frame_replay when set, on the command processor thread. Returns
// the process exit code, or -1 when no replay was requested.
using Fh1FrameReplayReadback =
    std::function<bool(uint32_t address, uint32_t length, std::vector<uint8_t>& out)>;
int RunFh1FrameReplay(CommandProcessor& command_processor, RegisterFile& register_file,
                      memory::Memory& memory, SharedMemory& shared_memory,
                      const Fh1FrameReplayReadback& read_front_buffer);

}  // namespace rex::graphics
