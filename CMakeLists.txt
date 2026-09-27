cmake_minimum_required(VERSION 3.20)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" ULOG_PROJECT_VERSION LIMIT_COUNT 1)
if(NOT ULOG_PROJECT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(
    FATAL_ERROR
      "version.txt must contain one semantic version in MAJOR.MINOR.PATCH form; found '${ULOG_PROJECT_VERSION}'."
  )
endif()

project(
  ulog
  VERSION "${ULOG_PROJECT_VERSION}"
  DESCRIPTION "A standalone performance-oriented C++ logging library"
  LANGUAGES CXX
)

include(CMakePackageConfigHelpers)
include(CTest)
include(GNUInstallDirs)

include(cmake/UlogOptions.cmake)
include(cmake/UlogTooling.cmake)

ulog_define_options()

find_package(fmt 12 CONFIG REQUIRED)
# libuv's own CMake package has no version file; src/io checks 1.51+ at compile time.
find_package(libuv CONFIG REQUIRED)
find_package(Threads REQUIRED)

# libuv exports libuv::uv_a for a static build and libuv::uv for a shared one.
if(TARGET libuv::uv_a)
  set(ULOG_LIBUV_TARGET libuv::uv_a)
elseif(TARGET libuv::uv)
  set(ULOG_LIBUV_TARGET libuv::uv)
else()
  message(
    FATAL_ERROR
      "libuv was found but exported neither libuv::uv_a nor libuv::uv. Use the Conan profile documented in docs/dependencies.md or a libuv 1.51 CMake package."
  )
endif()

set(ULOG_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
configure_file(include/ulog/version.hpp.in "${ULOG_GENERATED_INCLUDE_DIR}/ulog/version.hpp" @ONLY)

set(ULOG_PRODUCER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/producer/credit_ledger.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/producer/producer_kernel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/producer/record_storage.cpp"
)
set(ULOG_ENCODING_PRIVATE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/encoding/raw_encoder.cpp")
set(ULOG_CONTROL_PRIVATE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/control/control_reserve.cpp"
                                 "${CMAKE_CURRENT_SOURCE_DIR}/src/control/thread_role.cpp"
)
set(ULOG_IO_PRIVATE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/io/io_error.cpp"
                            "${CMAKE_CURRENT_SOURCE_DIR}/src/io/raw_file_sink.cpp"
)
set(ULOG_LIBRARY_SOURCES
    ${ULOG_ENCODING_PRIVATE_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/logger.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/operation.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/testing/in_memory_encoded_destination.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/testing/in_memory_destination.cpp"
    ${ULOG_PRODUCER_SOURCES}
    ${ULOG_CONTROL_PRIVATE_SOURCES}
    ${ULOG_IO_PRIVATE_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/version.cpp"
)
add_library(ulog ${ULOG_LIBRARY_SOURCES})
add_library(ulog::ulog ALIAS ulog)

target_compile_features(ulog PUBLIC cxx_std_20)
target_include_directories(
  ulog
  PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
         "$<BUILD_INTERFACE:${ULOG_GENERATED_INCLUDE_DIR}>"
         "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(
  ulog
  PRIVATE Threads::Threads ${ULOG_LIBUV_TARGET}
  PUBLIC fmt::fmt
)

if(BUILD_SHARED_LIBS)
  target_compile_definitions(ulog PRIVATE ULOG_BUILDING_LIBRARY)
else()
  target_compile_definitions(ulog PUBLIC ULOG_STATIC_DEFINE)
endif()

set_target_properties(
  ulog
  PROPERTIES CXX_EXTENSIONS OFF
             CXX_VISIBILITY_PRESET hidden
             EXPORT_NAME ulog
             POSITION_INDEPENDENT_CODE ON
             SOVERSION "${PROJECT_VERSION_MAJOR}"
             VERSION "${PROJECT_VERSION}"
             VISIBILITY_INLINES_HIDDEN ON
)
ulog_configure_target(ulog)
ulog_add_tooling_targets()

if(BUILD_TESTING)
  add_subdirectory(tests)
  if(ULOG_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
  endif()
endif()

set(ULOG_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/ulog")
# A static Ulog carries its private libuv link requirement to consumers; a shared one does not.
if(BUILD_SHARED_LIBS)
  set(ULOG_PACKAGE_REQUIRES_LIBUV OFF)
else()
  set(ULOG_PACKAGE_REQUIRES_LIBUV ON)
endif()
configure_package_config_file(
  cmake/ulogConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/ulogConfig.cmake"
  INSTALL_DESTINATION "${ULOG_INSTALL_CMAKEDIR}"
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/ulogConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
)

install(
  TARGETS ulog
  EXPORT ulogTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(
  FILES include/ulog/export.hpp
        include/ulog/level.hpp
        include/ulog/log.hpp
        include/ulog/logger.hpp
        include/ulog/operation.hpp
        include/ulog/runtime.hpp
        include/ulog/source_location.hpp
        "${ULOG_GENERATED_INCLUDE_DIR}/ulog/version.hpp"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ulog"
)
install(FILES include/ulog/testing/in_memory_destination.hpp
              include/ulog/testing/in_memory_encoded_destination.hpp
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ulog/testing"
)
install(
  EXPORT ulogTargets
  FILE ulogTargets.cmake
  NAMESPACE ulog::
  DESTINATION "${ULOG_INSTALL_CMAKEDIR}"
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ulogConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/ulogConfigVersion.cmake"
        DESTINATION "${ULOG_INSTALL_CMAKEDIR}"
)
install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/ulog")
