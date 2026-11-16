#pragma once

// ShiftGlue: DXVK's string helpers reduced to str::format.

#include <sstream>
#include <string>

namespace dxvk::str {

  template<typename... Args>
  std::string format(const Args&... args) {
    std::stringstream stream;
    (stream << ... << args);
    return stream.str();
  }

}
