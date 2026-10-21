#pragma once

// D3D source census (Pinyon Shift docs/D3D_RENDERER_RESEARCH.md DL-0.1): the
// tables a title's D3D observers (game side, guest hooks) fill and the
// command processor reads, so every decoded draw can be traced back to the
// API path that wrote its bytes, and every write into a watched display list
// to the code that made it. Diagnostics only: nothing here writes guest
// memory or changes rendering, and the command processor calls none of the
// lookups while Enabled() is false.
//
// Addresses are guest physical (the command processor's indirect-buffer
// pointers, & 0x1FFFFFFF). Lives in rexruntime because the GPU plugin and
// the game module both link it.

#include <cstdint>
#include <utility>
#include <vector>

namespace rex::system::d3d_census {

// Who wrote the packets from a marked address on (until the next mark).
enum Source : uint8_t {
  kSourceNone = 0,
  // kSourceDrawEntry + n: the game's n-th hooked draw entry point (0-14).
  kSourceDrawEntry = 1,
  kSourceDrawEntryLast = 15,
  // A D3D-internal draw (built-in shaders: clears, blits, resolves).
  kSourceInternal = 16,
  // A command-buffer run (the indirect-buffer packets it writes).
  kSourceListRun = 17,
  // An asynchronous command-buffer call.
  kSourceAsyncRun = 18,
  // D3D's own indirect buffer (a second INDIRECT_BUFFER producer).
  kSourceD3dIb = 19,
  kSourceCount = 32,
};

// How a segment the device closed reached the GPU.
enum SegmentMode : uint8_t {
  kSegmentUnknown = 0,
  kSegmentRing,       // kicked to the ring
  kSegmentTiling,     // appended to the tiling stream (replayed per tile)
  kSegmentRecording,  // appended to the command buffer being recorded
  kSegmentDiscard,    // the device dropped it
  kSegmentModeCount,
};

// Where a command buffer came from (bits).
enum ListOrigin : uint8_t {
  kOriginLoaded = 1,     // static fixups applied at load (asset path)
  kOriginRecorded = 2,   // was the device's recording command buffer
  kOriginCloned = 4,     // destination of the clone copy
  kOriginStream = 8,     // its bytes overlap a buffered file read
  kOriginD3dBuffer = 16, // D3D's own indirect buffer, not a command buffer
};

// What changed a list between two of its runs (set by the game side at each
// run, read by the decoder for the draws of that list).
enum ListState : uint8_t {
  kListFirstRun = 0,
  kListUnchanged,         // same bytes as at the previous run
  kListFixupOnly,         // bytes changed, fixups applied, no other writer seen
  kListChangedNoFixup,    // bytes changed with no fixup applied
  kListOtherWrite,        // the write watch saw a non-fixup writer
  kListStateCount,
};

// The writer of a watched buffer's page, by the guest code its lr is in.
enum WriterClass : uint8_t {
  kWriterOther = 0,
  kWriterFixup,
  kWriterStream,
  kWriterClone,
  kWriterClassCount,
};

// Innermost buffer of a decoded draw.
enum Context : uint8_t {
  kContextRing = 0,         // the primary ring itself
  kContextSegmentRing,      // a segment kicked to the ring
  kContextSegmentTiling,    // a segment replayed from the tiling stream
  kContextSegmentRecording, // a segment of a recording, not seen as a list
  kContextSegmentUnknown,   // an outermost buffer no segment mark names
  kContextList,             // a command buffer's indirect buffer
  kContextD3dBuffer,        // D3D's own indirect buffer
  kContextNestedUnknown,    // a nested buffer no list names
  kContextCount,
};

// Draw classes of the census.
enum DrawClass : uint8_t {
  kDrawImmediate = 0,  // an entry-point call into a ring segment
  kDrawTiledImmediate, // an entry-point call into a tiled segment
  kDrawInternal,       // a D3D built-in-shader draw
  kDrawList,           // inside a command buffer
  kDrawD3dBuffer,      // inside D3D's own indirect buffer
  kDrawUnattributed,   // in a segment, with no fresh mark before it
  kDrawUnknownBuffer,  // in a buffer the census does not know
  kDrawClassCount,
};

bool Enabled();
void SetEnabled(bool enabled);
// DL-1.1: classify only. The decoder keeps the draw classes and drops what
// only the census report needs (per-packet mark lookups inside display
// lists, the class timing); the game side drops the DL-0.2 device snapshots.
bool Lean();
void SetLean(bool lean);
// The guest's presented frames (set by the game side at each frame); marks
// older than a few frames are ignored, as their buffers may have been reused.
uint32_t Frame();
void SetFrame(uint32_t frame);

// --- Game side (guest threads) ---
void MarkPacket(uint32_t physical, uint8_t source);
void MarkSegment(uint32_t physical, uint32_t bytes, uint8_t mode);
void MarkListBuffer(uint32_t physical, uint32_t bytes, uint32_t object, uint8_t origin,
                    uint8_t state);
// Guest code [begin, end) whose writes into a watched list are writer_class.
void AddWriterRange(uint32_t begin, uint32_t end, uint8_t writer_class);
// The watch's writes into `object`'s buffers since the last call, by class.
void TakeListWrites(uint32_t object, uint32_t (&counts)[kWriterClassCount]);

// --- Command processor side ---
// The source of the mark at `physical` if it is fresh (any_age: a mark in a
// command buffer, which keeps its bytes across frames), else kSourceNone.
uint8_t FindPacket(uint32_t physical, bool any_age = false);
// The segment mode of a buffer starting at `physical` (kSegmentUnknown if
// none).
uint8_t FindSegment(uint32_t physical);
bool FindListBuffer(uint32_t physical, uint32_t* object, uint8_t* origin, uint8_t* state);
uint8_t ClassifyWriter(uint32_t lr);
// A write the IB write watch saw (gpu_trace_ib_writers) into a buffer
// starting at buffer_physical; list: the buffer was called from another.
void NoteBufferWrite(uint32_t buffer_physical, bool list, uint8_t writer_class, uint32_t lr);

// Decoder results, accumulated by the command processor and taken (summed
// and reset) by the game side once per window.
struct DecoderWindow {
  uint64_t swaps = 0;
  uint64_t ibs[kContextCount] = {};
  uint64_t drawn[kDrawClassCount] = {};
  // Drawn draws by the source of their mark and their buffer.
  uint64_t drawn_by_source[kSourceCount][kContextCount] = {};
  // Decoder thread time, exclusive, by the class of the work.
  uint64_t decode_ns[kDrawClassCount] = {};
  uint64_t list_drawn_by_state[kListStateCount] = {};
  uint64_t list_drawn_by_origin[32] = {};
  // Mark spans (from one draw-entry mark to the next) with 0, 1 and more
  // drawn draws: more than one means a draw from code without a mark.
  uint64_t spans[3] = {};
  uint64_t marks_found = 0;
  uint64_t gap_marks_found = 0;
};
void AddDecoderWindow(const DecoderWindow& window);
void TakeDecoderWindow(DecoderWindow* out);

// What the decoder saw inside one command buffer's runs (the pass census,
// NA-2.2): its drawn draws (first tile, as `drawn`), EDRAM copies, and the
// packets that have an effect outside the draw state.
struct ListDecoded {
  uint64_t draws = 0;
  uint64_t copies = 0;
  uint64_t events = 0;         // EVENT_WRITE, _SHD, _EXT, _ZPD
  uint64_t memory_writes = 0;  // MEM_WRITE, REG_TO_MEM, COND_WRITE
  uint64_t waits = 0;          // WAIT_REG_MEM
  uint64_t constants = 0;      // SET_CONSTANT(2), LOAD_ALU_CONSTANT
  uint64_t shader_loads = 0;   // IM_LOAD, IM_LOAD_IMMEDIATE
};
// Called by the decoder once a frame for each command buffer it saw.
void AddListDecoded(uint32_t object, const ListDecoded& counts);
// The sums since the last call, by command buffer object.
void TakeListDecoded(std::vector<std::pair<uint32_t, ListDecoded>>* out);

struct WriterWindow {
  // [list or outer][class]
  uint64_t writes[2][kWriterClassCount] = {};
  // Writes to buffers that are known command-buffer buffers.
  uint64_t list_object_writes[kWriterClassCount] = {};
  // The most frequent lrs of "other" writes (lr, count), zero-terminated.
  uint32_t other_lr[16] = {};
  uint64_t other_lr_count[16] = {};
};
void TakeWriterWindow(WriterWindow* out);

}  // namespace rex::system::d3d_census
