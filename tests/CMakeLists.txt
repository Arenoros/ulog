find_package(GTest CONFIG REQUIRED)

add_executable(ulog-tests
    level_test.cpp
    format_test.cpp
    log_extra_test.cpp
    tskv_escape_test.cpp
    log_macros_test.cpp
)
target_link_libraries(ulog-tests PRIVATE ulog::ulog GTest::gtest GTest::gtest_main)
add_test(NAME ulog-tests COMMAND ulog-tests)
