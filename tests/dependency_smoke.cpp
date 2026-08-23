#include <fmt/format.h>
#include <uv.h>

#include <string>

int main() {
  const std::string message = fmt::format("libuv {}", uv_version_string());
  return message.starts_with("libuv ") ? 0 : 1;
}
