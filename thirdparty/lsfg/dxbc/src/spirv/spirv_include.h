#pragma once

// ShiftGlue: standard headers DXVK's removed headers used to bring in.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>


#include <spirv/unified1/spirv.hpp>
#include <spirv/unified1/GLSL.std.450.h>

#include "../util/log/log.h"
#include "../util/log/log_debug.h"

#include "../util/util_error.h"
#include "../util/util_flags.h"
#include "../util/util_likely.h"
#include "../util/util_string.h"

#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"
