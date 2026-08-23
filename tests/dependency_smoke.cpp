#include <fmt/format.h>
#include <uv.h>

#include <string>

int RunDependencySmokeTest() {
  const std::string message = fmt::format("libuv {}", uv_version_string());
  return message.starts_with("libuv ") ? 0 : 1;
}

int main() noexcept {
  try {
    return RunDependencySmokeTest();
  } catch (...) {
    return 1;
  }
}
