# Dependency policy

Ulog uses Conan 2 for reproducible development and CI dependency resolution.
The bootstrap pins exact versions that are currently available from Conan
Center:

| Package | Version | Bootstrap role |
| --- | ---: | --- |
| fmt | 12.1.0 | Formatting dependency smoke test |
| libuv | 1.51.0 | Cross-platform asynchronous I/O dependency smoke test |
| GoogleTest | 1.17.0 | Unit tests |
| Google Benchmark | 1.9.5 | Benchmark harness |

fmt and libuv remain test requirements until production code actually uses
them. This keeps the initial installed library honest: it does not impose a
runtime dependency for a version query. Their first production use must promote
them to package requirements with the correct private/public traits and update
the installed CMake dependency graph.

libuv 1.51 exports `libuv::uv_a` for a static Conan package and `libuv::uv` for
a shared one. Ulog normalizes both names only inside the dependency smoke test;
future production code must keep libuv types out of the public API.

No Boost package is selected during bootstrap. Add only the specific Boost
modules justified by an implemented feature, never the aggregate package by
habit.

Conan Center uses `https://center2.conan.io` for current Conan 2 recipes. The
legacy Conan 1 remote is not supported.
