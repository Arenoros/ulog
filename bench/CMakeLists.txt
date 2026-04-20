find_package(benchmark CONFIG REQUIRED)

set(BENCH_SOURCES
    default_logger_bench.cpp
    sync_throughput_bench.cpp
    async_throughput_bench.cpp
    multi_producer_bench.cpp
    slow_sink_bench.cpp
    formatter_bench.cpp
    bench_main.cpp
)

if(ULOG_BENCH_SPDLOG)
    find_package(spdlog CONFIG REQUIRED)
    list(APPEND BENCH_SOURCES spdlog_compare_bench.cpp)
endif()

add_executable(ulog-bench ${BENCH_SOURCES})
target_link_libraries(ulog-bench
    PRIVATE
        ulog::ulog
        benchmark::benchmark
)

if(ULOG_BENCH_SPDLOG)
    target_link_libraries(ulog-bench PRIVATE spdlog::spdlog)
endif()
