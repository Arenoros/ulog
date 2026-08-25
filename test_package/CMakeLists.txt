cmake_minimum_required(VERSION 3.20)

project(ulog-conan-package-test LANGUAGES CXX)

enable_testing()
find_package(ulog CONFIG REQUIRED)

add_executable(ulog-conan-package-test src/main.cpp)
target_compile_features(ulog-conan-package-test PRIVATE cxx_std_20)
target_link_libraries(ulog-conan-package-test PRIVATE ulog::ulog)
add_test(NAME ulog.conan-package COMMAND ulog-conan-package-test)
set_tests_properties(ulog.conan-package PROPERTIES TIMEOUT 20)
