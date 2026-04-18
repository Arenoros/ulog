find_package(benchmark CONFIG REQUIRED)

add_executable(ulog-bench
    default_logger_bench.cpp
    sync_throughput_bench.cpp
    async_throughput_bench.cpp
    bench_main.cpp
)
target_link_libraries(ulog-bench
    PRIVATE
        ulog::ulog
        benchmark::benchmark
)
