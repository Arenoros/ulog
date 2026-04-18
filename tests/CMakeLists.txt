find_package(GTest CONFIG REQUIRED)

add_executable(ulog-tests
    level_test.cpp
    format_test.cpp
    log_extra_test.cpp
    tskv_escape_test.cpp
    log_macros_test.cpp
    formatter_test.cpp
    dynamic_debug_test.cpp
    sink_test.cpp
    async_logger_test.cpp
    config_test.cpp
    tracing_hook_test.cpp
    rate_limit_test.cpp
    default_logger_test.cpp
    stacktrace_cache_test.cpp
    tcp_sink_test.cpp
)
if(NOT WIN32)
    target_sources(ulog-tests PRIVATE unix_sink_test.cpp)
endif()
if(WIN32)
    target_sources(ulog-tests PRIVATE named_event_reopen_test.cpp)
endif()
target_link_libraries(ulog-tests PRIVATE ulog::ulog GTest::gtest GTest::gtest_main)
add_test(NAME ulog-tests COMMAND ulog-tests)
