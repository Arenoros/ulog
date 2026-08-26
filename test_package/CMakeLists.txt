cmake_minimum_required(VERSION 3.20)

project(ulog-conan-package-test LANGUAGES CXX)

enable_testing()
find_package(ulog CONFIG REQUIRED)

add_executable(ulog-conan-package-test src/main.cpp)
target_compile_features(ulog-conan-package-test PRIVATE cxx_std_20)
target_compile_definitions(ulog-conan-package-test PRIVATE ULOG_COMPILE_TIME_MIN_LEVEL=5)
target_link_libraries(ulog-conan-package-test PRIVATE ulog::ulog)
add_test(NAME ulog.conan-package COMMAND ulog-conan-package-test)
set_tests_properties(ulog.conan-package PROPERTIES TIMEOUT 20)

add_executable(ulog-conan-runtime-tracer src/runtime_main.cpp)
target_compile_features(ulog-conan-runtime-tracer PRIVATE cxx_std_20)
target_link_libraries(ulog-conan-runtime-tracer PRIVATE ulog::ulog)
add_test(NAME ulog.conan-runtime-tracer COMMAND ulog-conan-runtime-tracer)
set_tests_properties(ulog.conan-runtime-tracer PROPERTIES TIMEOUT 10)

add_executable(ulog-conan-raw-runtime-tracer src/raw_runtime_main.cpp)
target_compile_features(ulog-conan-raw-runtime-tracer PRIVATE cxx_std_20)
target_link_libraries(ulog-conan-raw-runtime-tracer PRIVATE ulog::ulog)
add_test(NAME ulog.conan-raw-runtime-tracer COMMAND ulog-conan-raw-runtime-tracer)
set_tests_properties(ulog.conan-raw-runtime-tracer PROPERTIES TIMEOUT 10)
