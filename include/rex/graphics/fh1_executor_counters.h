#pragma once
// Counters of the FH1 native executor (NP-9.0): why work was skipped (a
// draw, resolve or transfer the native path could not take) and how often
// each path ran. API-agnostic; logged periodically as name=count lists.

#include <cstdint>
#include <map>
#include <string>

namespace rex::graphics {

class Fh1ExecutorCounters {
 public:
  void Skip(const char* reason, uint64_t count = 1) { skips_[reason] += count; }
  void Count(const char* stat, uint64_t count = 1) { stats_[stat] += count; }

  std::string FormatSkips() const { return Format(skips_); }
  std::string FormatStats() const { return Format(stats_); }

 private:
  static std::string Format(const std::map<std::string, uint64_t>& counts) {
    std::string text;
    for (const auto& [name, count] : counts) {
      text += (text.empty() ? "" : ",") + name + "=" + std::to_string(count);
    }
    return text;
  }

  std::map<std::string, uint64_t> skips_;
  std::map<std::string, uint64_t> stats_;
};

}  // namespace rex::graphics
