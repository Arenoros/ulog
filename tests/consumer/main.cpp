#include <ulog/version.hpp>

int main() {
  const ulog::Version version = ulog::GetVersion();
  return version == ulog::kVersion ? 0 : 1;
}
