/**
 * Unit tests for the log file sink: non-ASCII paths and rotation.
 */

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <spdlog/sinks/sink.h>

#include <rex/filesystem.h>
#include <rex/logging.h>

namespace {

std::string ReadAll(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

spdlog::details::log_msg Message(const std::string& text) {
  return spdlog::details::log_msg("test", spdlog::level::info, text);
}

}  // namespace

TEST_CASE("log file sink writes to a non-ASCII path", "[logging]") {
  const auto root = std::filesystem::temp_directory_path() /
                    rex::to_path("rexglue_log_J\xc3\xbanior_\xe6\x97\xa5\xe6\x9c\xac");
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  const auto path = root / "logs" / "runtime.log";
  {
    auto sink = rex::CreateRotatingFileSink(path, 1 << 20, 3);
    sink->set_pattern("%v");
    sink->log(Message("first line"));
    sink->flush();
  }
  REQUIRE(std::filesystem::exists(path));
  CHECK(ReadAll(path).find("first line") != std::string::npos);
  std::filesystem::remove_all(root, ec);
}

TEST_CASE("log file sink appends and rotates", "[logging]") {
  const auto root = std::filesystem::temp_directory_path() / "rexglue_log_rotate";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  const auto path = root / "app.log";
  {
    auto sink = rex::CreateRotatingFileSink(path, 64, 2);
    sink->set_pattern("%v");
    sink->log(Message("kept from the first open"));
  }
  {
    auto sink = rex::CreateRotatingFileSink(path, 64, 2);
    sink->set_pattern("%v");
    for (int i = 0; i < 10; ++i)
      sink->log(Message("line " + std::to_string(i) + " 0123456789"));
    sink->flush();
  }
  CHECK(std::filesystem::exists(root / "app.1.log"));
  CHECK(std::filesystem::exists(root / "app.2.log"));
  CHECK_FALSE(std::filesystem::exists(root / "app.3.log"));
  CHECK(ReadAll(path).find("line 9") != std::string::npos);
  CHECK(std::filesystem::file_size(path) <= 64);
  std::filesystem::remove_all(root, ec);
}
