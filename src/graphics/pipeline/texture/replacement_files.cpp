/**
 * @file        graphics/pipeline/texture/replacement_files.cpp
 *
 * @brief       The files of the texture replacement folders, with aliases.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/graphics/pipeline/texture/replacement.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/filesystem.h>

namespace rex::graphics::texture_replacement {

namespace {

bool ParseHash(std::string_view text, uint64_t& hash) {
  if (text.size() != 16) return false;
  hash = 0;
  for (const char c : text) {
    const int digit = c >= '0' && c <= '9'   ? c - '0'
                      : c >= 'a' && c <= 'f' ? c - 'a' + 10
                      : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                             : -1;
    if (digit < 0) return false;
    hash = (hash << 4) | uint64_t(digit);
  }
  return true;
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

}  // namespace

ReplacementFiles ScanReplacementFolders(std::string_view dirs) {
  ReplacementFiles result;
  // Each hash's file and the rank of the folder that named it; the lowest
  // rank (the earliest folder) wins.
  std::unordered_map<uint64_t, std::pair<size_t, std::filesystem::path>> found;
  struct HashAlias {
    uint64_t hash, target;
    size_t rank;
    std::string where;
  };
  std::vector<HashAlias> hash_aliases;
  const auto offer = [&](uint64_t hash, size_t rank, const std::filesystem::path& path) {
    auto [it, inserted] = found.try_emplace(hash, rank, path);
    if (!inserted && rank < it->second.first) it->second = {rank, path};
  };

  size_t start = 0, rank = 0;
  while (start <= dirs.size()) {
    const size_t end = std::min(dirs.find(';', start), dirs.size());
    const std::filesystem::path dir = rex::to_path(dirs.substr(start, end - start));
    start = end + 1;
    std::error_code error;
    if (dir.empty() || !std::filesystem::is_directory(dir, error)) continue;
    ++rank;
    for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
      uint64_t hash = 0;
      if (entry.path().extension() == ".dds" &&
          ParseHash(rex::path_to_utf8(entry.path().stem()), hash)) {
        offer(hash, rank, entry.path());
      }
    }
    // aliases.txt: "<hash> <hash or file name>" per line, so textures that
    // differ only in their guest bytes (a streamed level, a recompressed
    // copy) share one image. '#' starts a comment.
    std::ifstream aliases(dir / "aliases.txt", std::ios::binary);
    std::string line;
    for (int number = 1; std::getline(aliases, line); ++number) {
      std::string_view text = line;
      if (const size_t hash_mark = text.find('#'); hash_mark != std::string_view::npos) {
        text = text.substr(0, hash_mark);
      }
      text = Trim(text);
      if (text.empty()) continue;
      const std::string where = rex::path_to_utf8(dir / "aliases.txt") + ":" +
                                std::to_string(number);
      const size_t space = text.find_first_of(" \t");
      uint64_t hash = 0, target = 0;
      if (space == std::string_view::npos || !ParseHash(text.substr(0, space), hash)) {
        result.problems.push_back(where + ": expected <hash> <hash or file>");
        continue;
      }
      const std::string_view target_text = Trim(text.substr(space));
      if (ParseHash(target_text, target)) {
        hash_aliases.push_back({hash, target, rank, where});
        continue;
      }
      const auto path = dir / rex::to_path(target_text);
      if (target_text.find("..") != std::string_view::npos ||
          !std::filesystem::is_regular_file(path, error)) {
        result.problems.push_back(where + ": no file " + std::string(target_text));
        continue;
      }
      offer(hash, rank, path);
      ++result.aliases;
    }
  }
  // Hash targets may be in any folder; the alias keeps its own folder's rank.
  for (const auto& alias : hash_aliases) {
    const auto target = found.find(alias.target);
    if (target == found.end()) {
      result.problems.push_back(alias.where + ": no replacement for its target");
      continue;
    }
    const std::filesystem::path path = target->second.second;
    offer(alias.hash, alias.rank, path);
    ++result.aliases;
  }
  for (auto& [hash, ranked] : found) result.files.emplace(hash, std::move(ranked.second));
  return result;
}

}  // namespace rex::graphics::texture_replacement
