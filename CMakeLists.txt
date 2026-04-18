cmake_minimum_required(VERSION 3.16)
project(ulog
    VERSION 0.1.0
    DESCRIPTION "Standalone logging library extracted from userver"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(ULOG_BUILD_TESTS      "Build unit tests"                 ON)
option(ULOG_BUILD_EXAMPLES   "Build examples"                   ON)
option(ULOG_BUILD_BENCH      "Build benchmarks"                 OFF)
option(ULOG_WITH_NLOHMANN    "Use nlohmann::json for JsonString" OFF)
option(ULOG_WITH_YAML        "Enable yaml-cpp config loader"    OFF)
option(ULOG_NO_SHORT_MACROS  "Disable short LOG_* macros (expose ULOG_LOG_* only)" OFF)
option(ULOG_INSTALL          "Generate install rules"           ON)
set(ULOG_ERASE_LOG_WITH_LEVEL "0" CACHE STRING "Compile-erase logs strictly below this level (0..5). 0 disables.")

# ---------------------------------------------------------------------------
# Compile settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /EHsc)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Dependencies (resolved via Conan toolchain or system find_package)
# ---------------------------------------------------------------------------
find_package(fmt CONFIG REQUIRED)
find_package(Boost REQUIRED)

if(ULOG_WITH_NLOHMANN)
    find_package(nlohmann_json CONFIG REQUIRED)
endif()

if(ULOG_WITH_YAML)
    find_package(yaml-cpp CONFIG REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Main target
# ---------------------------------------------------------------------------
add_library(ulog)
add_library(ulog::ulog ALIAS ulog)

target_compile_features(ulog PUBLIC cxx_std_17)

target_include_directories(ulog
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/concurrentqueue
)

target_link_libraries(ulog
    PUBLIC
        fmt::fmt
        Boost::container
        Boost::stacktrace
)

if(ULOG_WITH_NLOHMANN)
    target_link_libraries(ulog PUBLIC nlohmann_json::nlohmann_json)
    target_compile_definitions(ulog PUBLIC ULOG_WITH_NLOHMANN=1)
endif()

if(ULOG_NO_SHORT_MACROS)
    target_compile_definitions(ulog PUBLIC ULOG_NO_SHORT_MACROS=1)
endif()

if(NOT ULOG_ERASE_LOG_WITH_LEVEL STREQUAL "0")
    target_compile_definitions(ulog PUBLIC ULOG_ERASE_LOG_WITH_LEVEL=${ULOG_ERASE_LOG_WITH_LEVEL})
endif()

if(WIN32)
    target_link_libraries(ulog PUBLIC ws2_32)
endif()
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(ulog PUBLIC Threads::Threads)
    if(NOT APPLE)
        target_link_libraries(ulog PUBLIC ${CMAKE_DL_LIBS})
    endif()
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
add_subdirectory(src)

# ---------------------------------------------------------------------------
# Tests / examples / bench
# ---------------------------------------------------------------------------
if(ULOG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ULOG_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(ULOG_BUILD_BENCH)
    add_subdirectory(bench)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
if(NOT ULOG_INSTALL)
    return()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS ulog
    EXPORT ulog-targets
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT ulog-targets
    FILE ulog-targets.cmake
    NAMESPACE ulog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ulog
)

configure_package_config_file(
    cmake/ulog-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ulog-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ulog
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ulog-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/ulog-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ulog-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ulog
)
