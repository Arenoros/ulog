find_package(benchmark 1.9 CONFIG REQUIRED)

add_executable(ulog-benchmarks version_benchmark.cpp)
target_compile_features(ulog-benchmarks PRIVATE cxx_std_20)
target_link_libraries(ulog-benchmarks PRIVATE ulog::ulog benchmark::benchmark)
ulog_configure_target(ulog-benchmarks)

add_test(
  NAME ulog.benchmark-smoke
  COMMAND
    ulog-benchmarks --benchmark_min_time=0.001s --benchmark_color=false
    "--benchmark_out=${PROJECT_BINARY_DIR}/benchmark-results.json" --benchmark_out_format=json
)
set_tests_properties(ulog.benchmark-smoke PROPERTIES LABELS "benchmark")
