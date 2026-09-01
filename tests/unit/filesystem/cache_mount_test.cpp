/**
 * @file        tests/unit/filesystem/cache_mount_test.cpp
 * @brief       Regression coverage for writable guest cache mounts
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/vfs.h>

namespace {

class ScopedTempDirectory {
 public:
  ScopedTempDirectory() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("rexglue-cache-mount-" + std::to_string(nonce));
  }

  ~ScopedTempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace

TEST_CASE("Writable cache mount creates transient guest files", "[filesystem][cache]") {
  ScopedTempDirectory cache_root;
  rex::filesystem::VirtualFileSystem vfs;

  constexpr auto kCacheMount = "\\Device\\Harddisk0\\Cache";
  auto cache_device = std::make_unique<rex::filesystem::HostPathDevice>(
      kCacheMount, cache_root.path(), false);
  REQUIRE(cache_device->Initialize());
  REQUIRE(vfs.RegisterDevice(std::move(cache_device)));
  REQUIRE(vfs.RegisterSymbolicLink("cache:", kCacheMount));

  auto* root = vfs.ResolvePath("cache:\\");
  REQUIRE(root != nullptr);
  CHECK_FALSE(root->is_read_only());

  auto* replay_stream =
      vfs.CreatePath("cache:\\replay_stream", rex::filesystem::kFileAttributeNormal);
  REQUIRE(replay_stream != nullptr);
  CHECK(std::filesystem::is_regular_file(cache_root.path() / "replay_stream"));
}
