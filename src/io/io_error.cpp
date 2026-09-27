#include <cstdint>
#include <string_view>
#include <ulog/runtime.hpp>

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <uv.h>

static_assert(UV_VERSION_MAJOR == 1 && UV_VERSION_MINOR >= 51,
              "Ulog requires libuv 1.51 or a later 1.x release; see docs/dependencies.md.");

namespace ulog {

std::string_view IoErrorName(std::int32_t io_error) noexcept {
  // Built from libuv's static table; uv_err_name() allocates for unknown values.
  switch (io_error) {
#define ULOG_DETAIL_IO_ERROR_NAME(name, message) \
  case UV_##name:                                \
    return #name;
    UV_ERRNO_MAP(ULOG_DETAIL_IO_ERROR_NAME)
#undef ULOG_DETAIL_IO_ERROR_NAME
    default:
      return "UNKNOWN";
  }
}

}  // namespace ulog
