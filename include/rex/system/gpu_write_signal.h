#pragma once

#include <chrono>
#include <cstdint>

namespace rex::system {

// Counts the command processor's writes of CPU-visible results to guest
// memory (fence words from EVENT_WRITE_SHD, MEM_WRITE, COND_WRITE, interrupts
// and swaps), so a guest thread polling such a word can block until the next
// write instead of spinning.
uint32_t GpuWriteSequence();

// Called by the command processor after each such packet.
void SignalGpuWrite();

// Blocks until GpuWriteSequence() differs from `seen` or `timeout` passes.
void WaitForGpuWrite(uint32_t seen, std::chrono::microseconds timeout);

}  // namespace rex::system
