# Dependency policy

Ulog uses Conan 2 for reproducible development and CI dependency resolution.
The bootstrap pins exact versions that are currently available from Conan
Center:

| Package | Version | Bootstrap role |
| --- | ---: | --- |
| fmt | 12.1.0 | Public compile-time-checked formatting backend |
| libuv | 1.51.0 | Private asynchronous file I/O backend |
| GoogleTest | 1.17.0 | Unit tests |
| Google Benchmark | 1.9.5 | Benchmark harness |

fmt is a public production requirement because `<ulog/log.hpp>` exposes
compile-time-checked formatting calls. The installed CMake and Conan metadata
therefore carry fmt transitively.

libuv is a private production requirement of the Raw file route. No libuv header,
type, or handle appears in `include/ulog`; public results carry libuv error codes
only as `std::int32_t` values with `ulog::IoErrorName()`. Ulog links libuv
privately:

- a static Ulog records a link-only libuv requirement, so its installed
  `ulogConfig.cmake` finds libuv and the Conan package lists `libuv::libuv`;
- a shared Ulog embeds the static libuv package and consumers do not link it.

libuv 1.51 exports `libuv::uv_a` for a static package and `libuv::uv` for a shared
one; Ulog prefers the static target. libuv's own CMake package has no version
file, so CMake finds it without a version and the sources reject anything older
than 1.51 or outside the 1.x series with a `static_assert`. Ulog never reads or
changes `UV_THREADPOOL_SIZE`.

No Boost package is selected during bootstrap. Add only the specific Boost
modules justified by an implemented feature, never the aggregate package by
habit.

Conan Center uses `https://center2.conan.io` for current Conan 2 recipes. The
legacy Conan 1 remote is not supported.
