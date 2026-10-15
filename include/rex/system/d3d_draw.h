#pragma once

// D3D-level draws (Pinyon Shift docs/D3D_RENDERER_RESEARCH.md DL-1.2): a
// draw described from the guest D3D device's state at the draw call, handed
// from the title's hooks (game side) to the command processor, which draws
// it in place of the packets the device wrote for it.
//
// Ordering: at the draw call the game side writes a marker (a PM4 NOP,
// kMarkerMagic, the description id and the mode) into the device's command
// buffer before the call's own packets, and publishes the description here
// when the call returns. The decoder meets the marker where the call's
// packets start, so the draw lands in the same place in the host command
// stream; it then reads the description by id. A description it cannot read
// (overwritten, declined, too large) leaves the packets to the decoder.
//
// Description format: a header and a word stream of entries, each a kind
// word ((kind << 16) | count) and a first-register word, then:
// - kEntryRegisters: count register values, big-endian (as the device
//   shadow holds them);
// - kEntryRegistersFromMemory: one guest physical address of count
//   big-endian values (a shader's literal constants);
// - kEntryShader: the shader type in the first-register word; one guest
//   physical address and, if count > 1, count - 1 microcode words copied
//   from the command buffer (a load the device wrote inline).
// Entries apply in order. Lives in rexruntime: the game module and the GPU
// plugin both link it.

#include <atomic>
#include <cstdint>

namespace rex::system::d3d_draw {

// "D3DL", the first word of a marker NOP's payload.
constexpr uint32_t kMarkerMagic = 0x4433444C;
// Type-3 NOP, three payload words: magic, id, mode.
constexpr uint32_t kMarkerHeader = 0xC0021000;

enum Mode : uint32_t {
  kModeOff = 0,
  // The decoder runs the packets and compares them with the description.
  kModeVerify = 1,
  // The decoder skips the packets the description covers and draws it.
  kModeDraw = 2,
};

enum EntryKind : uint32_t {
  kEntryRegisters = 1,
  kEntryRegistersFromMemory = 2,
  kEntryShader = 3,
};

enum DescriptionFlags : uint32_t {
  // The draw call did not qualify after its prologue ran (target, scope):
  // the decoder leaves the packets alone.
  kFlagDeclined = 1,
  // The description did not fit: the same.
  kFlagOverflow = 2,
  // DL-2.1 replacement: the draw packet is an auto-index DRAW_INDX of two
  // words (viz query, VGT_DRAW_INITIATOR) instead of an indexed one of four.
  kFlagAutoIndex = 4,
};

constexpr uint32_t kMaxWords = 3072;

struct Description {
  uint32_t id = 0;
  uint32_t flags = 0;
  // The draw packet the call writes: PM4_DRAW_INDX's viz query word,
  // VGT_DRAW_INITIATOR, VGT_DMA_BASE and VGT_DMA_SIZE.
  uint32_t draw[4] = {};
  // DL-2.1 replacement: the draw packet's header (its predicate bit decides
  // whether a tiling pass draws it).
  uint32_t packet = 0;
  // DL-2.1 arena: the guest frame that published it (set by PublishArena).
  uint32_t frame = 0;
  uint32_t word_count = 0;
  uint32_t words[kMaxWords];
};

// Publishes a finished description (copies description.word_count words).
void Publish(const Description& description);

// Copies the description with this id into *out if it is still held and
// complete; false if it was overwritten or never published.
bool Fetch(uint32_t id, Description* out);

// A new id (never 0).
uint32_t NextId();

// --- DL-2.1, the first native slice (Pinyon Shift D3D_RENDERER_RESEARCH) ---
//
// Replaced draw calls: the title's indexed and auto-index draw calls are
// replaced at their entry (the guest writes none of their packets). The
// game side describes the call from the device state, clears the dirty
// masks as the call would, and writes one marker, a type-3 NOP of three
// words: kReplaceMagic, the description's arena offset, and (id << 2) |
// mode. The decoder applies the description where the marker is and, in
// mode kSliceOn, draws it; in kSliceVerify the call ran as usual after the
// marker and the decoder compares.
//
// Descriptions live in an arena of chunks that are reused only once the
// decoder has fetched a description two guest frames newer than the chunk's
// last writer (a marker in the tiling stream is decoded once per tile, so a
// description is read more than once, but never after the next frame's
// markers). Publishing fails rather than overwriting; the caller then runs
// the guest call as usual.

// "D3DR", the first word of a replacement marker's payload.
constexpr uint32_t kReplaceMagic = 0x44334452;

enum SliceMode : uint32_t {
  kSliceOff = 0,
  // Calls run as usual after a marker; the decoder compares them with the
  // description. Display lists are compiled and checked against the packets.
  kSliceVerify = 1,
  // Calls are replaced; display lists run from their compiled form.
  kSliceOn = 2,
};

// The game's slice mode (pinyon_shift_d3d_slice), set once a frame.
uint32_t GetSliceMode();
void SetSliceMode(uint32_t mode);

// Publishes a description into the arena; returns its offset + 1, or 0 if
// the arena has no chunk the decoder is done with.
uint32_t PublishArena(const Description& description);
// The description at offset (offset + 1 as returned by PublishArena, minus
// one) if it is still the one with this id, else nullptr. Only
// word_count words of words[] are valid.
const Description* FetchArena(uint32_t offset, uint32_t id);
// The guest's frame boundary (once a frame, before its draws).
void GuestFrame();

}  // namespace rex::system::d3d_draw
