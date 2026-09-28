#pragma once

#include <cstdint>

namespace rex::graphics {

// Layout of the title's 256x256 eight-level reflection cube: the six base faces
// precede the mip chain in guest memory.
struct Fh1MipChain {
  static constexpr uint32_t kBaseBytes = 6 * 256 * 256 * 4;
};

}  // namespace rex::graphics
