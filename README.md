# Ulog

Ulog is a standalone, performance-oriented C++20 logging library under active
development. The repository currently contains the verified project and agent
infrastructure; production logging APIs are intentionally not implemented yet.

The design preserves the capabilities of the pinned reference implementation
without source or API compatibility and without depending on that project.
Windows, Linux, and macOS are first-class targets. Native asynchronous file,
network, and IPC implementations will use libuv.

## Build the bootstrap

CMake 3.20 or newer is required. Presets require CMake 3.25 or newer. On
Windows, run CMake from a Visual Studio developer shell. Agents and automation
may use `scripts\\with-msvc.cmd` to locate and activate the newest installed MSVC
toolchain without hard-coding a Visual Studio version.

```shell
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Static is the default. Explicit release checks are available as
`release-static` and `release-shared` presets. Both install and exercise an
external consumer through the canonical target:

```cmake
find_package(ulog CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE ulog::ulog)
```

For the full Conan, unit, stress, dependency, and benchmark workflow, see
[`docs/testing.md`](docs/testing.md). The shared performance workload and
controlled-run procedure are in [`docs/benchmarking.md`](docs/benchmarking.md).
Dependency versions and promotion rules are in
[`docs/dependencies.md`](docs/dependencies.md).

## Architecture

Start with [`CONTEXT.md`](CONTEXT.md), then read the applicable records under
[`docs/adr/`](docs/adr/) and the
[`Performance Contract`](docs/performance-contract.md). Production work must
not infer numerical defaults that those documents intentionally leave to
benchmark evidence.

## Contributing and support

See [`CONTRIBUTING.md`](CONTRIBUTING.md), [`SUPPORT.md`](SUPPORT.md), and
[`SECURITY.md`](SECURITY.md). Ulog is licensed under Apache-2.0; see
[`LICENSE`](LICENSE).
