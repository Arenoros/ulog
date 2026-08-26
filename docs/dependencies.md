# Dependency policy

Ulog uses Conan 2 for reproducible development and CI dependency resolution.
The bootstrap pins exact versions that are currently available from Conan
Center:

| Package | Version | Bootstrap role |
| --- | ---: | --- |
| fmt | 12.1.0 | Public compile-time-checked formatting backend |
| libuv | 1.51.0 | Cross-platform asynchronous I/O dependency smoke test |
| GoogleTest | 1.17.0 | Unit tests |
| Google Benchmark | 1.9.5 | Benchmark harness |

fmt is a public production requirement because `<ulog/log.hpp>` exposes
compile-time-checked formatting calls. The installed CMake and Conan metadata
therefore carry fmt transitively. libuv remains a test requirement until the
production Runtime delivery path uses it.

libuv 1.51 exports `libuv::uv_a` for a static Conan package and `libuv::uv` for
a shared one. Ulog normalizes both names only inside the dependency smoke test;
future production code must keep libuv types out of the public API.

No Boost package is selected during bootstrap. Add only the specific Boost
modules justified by an implemented feature, never the aggregate package by
habit.

Conan Center uses `https://center2.conan.io` for current Conan 2 recipes. The
legacy Conan 1 remote is not supported.
