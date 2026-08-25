#include <cstddef>
#include <cstdlib>
#include <new>
#include <string_view>
#include <type_traits>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/source_location.hpp>

std::string_view MissingMessageDefinition();

namespace {

std::size_t g_allocations = 0;
std::size_t g_deallocations = 0;

void* Allocate(std::size_t size) {
  ++g_allocations;
  if (void* const pointer = std::malloc(size == 0 ? 1 : size)) {
    return pointer;
  }
  throw std::bad_alloc{};
}

void Deallocate(void* pointer) noexcept {
  ++g_deallocations;
  std::free(pointer);
}

}  // namespace

void* operator new(std::size_t size) { return Allocate(size); }

void* operator new[](std::size_t size) { return Allocate(size); }

void operator delete(void* pointer) noexcept { Deallocate(pointer); }

void operator delete[](void* pointer) noexcept { Deallocate(pointer); }

void operator delete(void* pointer, std::size_t) noexcept { Deallocate(pointer); }

void operator delete[](void* pointer, std::size_t) noexcept { Deallocate(pointer); }

int main() {
  static_assert(std::is_trivially_copyable_v<ulog::Logger>);
  static_assert(std::is_trivially_destructible_v<ulog::Logger>);
  static_assert(sizeof(ulog::Logger) == sizeof(void*));

  const ulog::SourceLocation location = ulog::SourceLocation::Current();
  const ulog::Logger initial_logger = ulog::GetDefaultLogger();
  if (initial_logger != ulog::GetNullLogger()) {
    return 1;
  }

  const std::size_t allocations_before = g_allocations;
  const std::size_t deallocations_before = g_deallocations;
  std::size_t message_evaluations = 0;

  for (std::size_t iteration = 0; iteration < 10'000; ++iteration) {
    const ulog::Logger logger = ulog::GetDefaultLogger();
    logger.Log<ulog::Level::kInfo>(location, [] { return MissingMessageDefinition(); });
    logger.Log<ulog::Level::kCritical>(location, [&]() -> std::string_view {
      ++message_evaluations;
      return "the Null Logger must reject this";
    });
  }

  if (message_evaluations != 0) {
    return 2;
  }
  if (g_allocations != allocations_before || g_deallocations != deallocations_before) {
    return 3;
  }
  return 0;
}
