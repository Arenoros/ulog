cmake_minimum_required(VERSION 3.20)

project(ulog-installed-consumer LANGUAGES CXX)

enable_testing()
find_package(ulog CONFIG REQUIRED)

add_executable(ulog-installed-consumer default_logger_probe.cpp main.cpp)
target_compile_features(ulog-installed-consumer PRIVATE cxx_std_20)
target_compile_definitions(ulog-installed-consumer PRIVATE ULOG_COMPILE_TIME_MIN_LEVEL=5)
target_link_libraries(ulog-installed-consumer PRIVATE ulog::ulog)

add_executable(ulog-installed-runtime-consumer runtime_main.cpp)
target_compile_features(ulog-installed-runtime-consumer PRIVATE cxx_std_20)
target_link_libraries(ulog-installed-runtime-consumer PRIVATE ulog::ulog)

add_executable(ulog-installed-raw-runtime-consumer raw_runtime_main.cpp)
target_compile_features(ulog-installed-raw-runtime-consumer PRIVATE cxx_std_20)
target_link_libraries(ulog-installed-raw-runtime-consumer PRIVATE ulog::ulog)

if(WIN32)
  get_target_property(_ulog_imported_type ulog::ulog TYPE)
  if(_ulog_imported_type STREQUAL "SHARED_LIBRARY")
    foreach(_ulog_consumer IN ITEMS ulog-installed-consumer ulog-installed-runtime-consumer
                                    ulog-installed-raw-runtime-consumer
    )
      add_custom_command(
        TARGET ${_ulog_consumer}
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:ulog::ulog>"
                "$<TARGET_FILE_DIR:${_ulog_consumer}>"
        VERBATIM
      )
    endforeach()
  endif()
endif()

add_test(NAME ulog.installed-consumer COMMAND ulog-installed-consumer)
add_test(NAME ulog.installed-runtime-consumer COMMAND ulog-installed-runtime-consumer)
add_test(NAME ulog.installed-raw-runtime-consumer COMMAND ulog-installed-raw-runtime-consumer)
set_tests_properties(
  ulog.installed-runtime-consumer ulog.installed-raw-runtime-consumer PROPERTIES TIMEOUT 10
)
