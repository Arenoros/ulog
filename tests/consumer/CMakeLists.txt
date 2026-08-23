cmake_minimum_required(VERSION 3.20)

project(ulog-installed-consumer LANGUAGES CXX)

enable_testing()
find_package(ulog CONFIG REQUIRED)

add_executable(ulog-installed-consumer main.cpp)
target_compile_features(ulog-installed-consumer PRIVATE cxx_std_20)
target_link_libraries(ulog-installed-consumer PRIVATE ulog::ulog)

if(WIN32)
  get_target_property(_ulog_imported_type ulog::ulog TYPE)
  if(_ulog_imported_type STREQUAL "SHARED_LIBRARY")
    add_custom_command(
      TARGET ulog-installed-consumer
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:ulog::ulog>"
              "$<TARGET_FILE_DIR:ulog-installed-consumer>"
      VERBATIM
    )
  endif()
endif()

add_test(NAME ulog.installed-consumer COMMAND ulog-installed-consumer)
