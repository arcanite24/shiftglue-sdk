#pragma once

// Owned draw programs (Pinyon Shift NA-2.4, issue #753): the command buffers
// a title's pilot owner runs, marked by the game side as they are run and
// looked up by the command processor when it enters an indirect buffer. The
// backend then runs the buffer's draws from a program it recorded (the
// derived draw state, by draw) instead of deriving them again.
//
// Addresses are guest physical (the command processor's indirect-buffer
// pointers, & 0x1FFFFFFF). Lives in rexruntime because the GPU plugin and
// the game module both link it.

#include <cstdint>

namespace rex::system::d3d_program {

// gpu_d3d_program, set by the command processor at each swap: 0 off,
// 1 shadow (derive as usual and compare with the program), 2 performance
// (draw from the program), 3 baseline (record and count only), 4 timed only
// (the lists bracketed, no program).
uint32_t Mode();
void SetMode(uint32_t mode);
// Advanced by the command processor at each swap; marks older than a few
// swaps are ignored, as the guest may reuse the buffer for another list.
void Swap();

// --- Game side (guest threads) ---
void MarkList(uint32_t physical);

// --- Command processor side ---
bool IsList(uint32_t physical);

}  // namespace rex::system::d3d_program
