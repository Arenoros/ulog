#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>

namespace ulog::test_support {

/// Creates a unique directory below the system temporary directory and removes it on exit.
class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> next_directory{0};
    const auto nonce =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    for (int attempt = 0; attempt < 16; ++attempt) {
      const std::uint64_t index = next_directory.fetch_add(1, std::memory_order_relaxed);
      auto candidate = std::filesystem::temp_directory_path() /
                       ("ulog-test-" + std::to_string(nonce) + "-" + std::to_string(index));
      std::error_code error;
      if (std::filesystem::create_directory(candidate, error)) {
        path_ = std::move(candidate);
        return;
      }
    }
    throw std::runtime_error{"unable to create a unique temporary test directory"};
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

[[nodiscard]] inline std::string ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary};
  return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

}  // namespace ulog::test_support
