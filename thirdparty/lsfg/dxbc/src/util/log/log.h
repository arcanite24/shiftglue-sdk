#pragma once

// ShiftGlue: DXVK's logger reduced to warnings and errors on stderr.

#include <cstdio>
#include <string>

namespace dxvk {

  class Logger {
  public:
    static void trace(const std::string&) { }
    static void debug(const std::string&) { }
    static void info (const std::string&) { }
    static void warn (const std::string& message) { std::fprintf(stderr, "dxbc: warn: %s\n", message.c_str()); }
    static void err  (const std::string& message) { std::fprintf(stderr, "dxbc: err: %s\n", message.c_str()); }
  };

}
