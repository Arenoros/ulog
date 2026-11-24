#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <typeinfo>
#include <ulog/log_writer.hpp>

#if defined(__GNUG__)
#include <cxxabi.h>
#endif

namespace ulog::detail {

#if defined(__GNUG__)
namespace {

/// A per-thread buffer that __cxa_demangle grows with realloc; after the longest name a thread
/// logs, demangling reuses it without allocating.
struct DemangleBuffer final {
  DemangleBuffer() = default;
  DemangleBuffer(const DemangleBuffer&) = delete;
  DemangleBuffer& operator=(const DemangleBuffer&) = delete;
  ~DemangleBuffer() { std::free(data); }

  char* data{nullptr};
  std::size_t size{0};
};

}  // namespace
#endif

std::string_view TypeName(const std::type_info& type) noexcept {
  const char* const name = type.name();
#if defined(__GNUG__)
  thread_local DemangleBuffer buffer;
  int status = 0;
  std::size_t size = buffer.size;
  char* const demangled = abi::__cxa_demangle(name, buffer.data, &size, &status);
  if (status == 0 && demangled != nullptr) {
    buffer.data = demangled;
    buffer.size = size;
    return demangled;
  }
  return name;
#else
  // MSVC returns cached readable names such as "class std::runtime_error".
  std::string_view readable{name};
  for (const std::string_view prefix : {std::string_view{"class "}, std::string_view{"struct "}}) {
    if (readable.starts_with(prefix)) {
      readable.remove_prefix(prefix.size());
      break;
    }
  }
  return readable;
#endif
}

}  // namespace ulog::detail
