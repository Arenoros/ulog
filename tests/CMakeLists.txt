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
    instrumented_sink_test.cpp
    async_logger_test.cpp
    otlp_batch_sink_test.cpp
    config_test.cpp
    tracing_hook_test.cpp
    rate_limit_test.cpp
    default_logger_test.cpp
    stacktrace_cache_test.cpp
    tcp_sink_test.cpp
    per_sink_format_test.cpp
    record_enricher_test.cpp
)
if(NOT WIN32)
    target_sources(ulog-tests PRIVATE unix_sink_test.cpp)
endif()
if(WIN32)
    target_sources(ulog-tests PRIVATE named_event_reopen_test.cpp)
    # Compiled on Windows only when the AF_UNIX sink was enabled via
    # ULOG_WITH_AFUNIX — the test file itself is guarded on
    # ULOG_HAVE_AFUNIX so a stray inclusion without the PUBLIC define
    # would still expand to an empty translation unit.
    if(ULOG_WITH_AFUNIX)
        target_sources(ulog-tests PRIVATE unix_sink_test.cpp)
    endif()
endif()
target_link_libraries(ulog-tests PRIVATE ulog::ulog GTest::gtest GTest::gtest_main)
add_test(NAME ulog-tests COMMAND ulog-tests)
# Kill the test binary after 5 minutes rather than letting CI burn
# credit on a hang. The suite normally runs under 10 s; the budget is
# deliberately wide to absorb sanitizer overhead (TSan can be 10x).
# If a future test deadlocks or livelocks, ctest's timeout report
# names the test that was running when the process was killed.
set_tests_properties(ulog-tests PROPERTIES TIMEOUT 300)
