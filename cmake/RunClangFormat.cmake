if(NOT DEFINED ULOG_SOURCE_DIR)
  message(FATAL_ERROR "ULOG_SOURCE_DIR is required. Pass -DULOG_SOURCE_DIR=<repository root>.")
endif()

file(REAL_PATH "${ULOG_SOURCE_DIR}" ULOG_SOURCE_DIR BASE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
if(NOT EXISTS "${ULOG_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "ULOG_SOURCE_DIR does not point to the Ulog repository: ${ULOG_SOURCE_DIR}")
endif()

if(NOT ULOG_FORMAT_MODE MATCHES "^(check|fix)$")
  message(FATAL_ERROR "ULOG_FORMAT_MODE must be 'check' or 'fix'.")
endif()

find_program(_ulog_clang_format NAMES clang-format)
if(NOT _ulog_clang_format)
  message(FATAL_ERROR "clang-format was not found. Install LLVM clang-format and retry.")
endif()

file(
  GLOB_RECURSE _ulog_format_files
  LIST_DIRECTORIES FALSE
  "${ULOG_SOURCE_DIR}/include/*.h"
  "${ULOG_SOURCE_DIR}/include/*.hpp"
  "${ULOG_SOURCE_DIR}/include/*.hpp.in"
  "${ULOG_SOURCE_DIR}/src/*.cpp"
  "${ULOG_SOURCE_DIR}/src/*.hpp"
  "${ULOG_SOURCE_DIR}/tests/*.cpp"
  "${ULOG_SOURCE_DIR}/tests/*.hpp"
  "${ULOG_SOURCE_DIR}/benchmarks/*.cpp"
  "${ULOG_SOURCE_DIR}/benchmarks/*.hpp"
  "${ULOG_SOURCE_DIR}/test_package/*.cpp"
  "${ULOG_SOURCE_DIR}/test_package/*.hpp"
  "${ULOG_SOURCE_DIR}/tools/*.cpp"
  "${ULOG_SOURCE_DIR}/tools/*.hpp"
  "${ULOG_SOURCE_DIR}/tools/*.hpp.in"
)
list(FILTER _ulog_format_files EXCLUDE REGEX "/(build[^/]*|out)/")

if(NOT _ulog_format_files)
  message(FATAL_ERROR "No C++ files were found below ${ULOG_SOURCE_DIR}.")
endif()

if(ULOG_FORMAT_MODE STREQUAL "fix")
  set(_ulog_format_args -i)
else()
  set(_ulog_format_args --dry-run --Werror)
endif()

execute_process(
  COMMAND "${_ulog_clang_format}" ${_ulog_format_args} ${_ulog_format_files}
  RESULT_VARIABLE _ulog_format_result
)
if(NOT _ulog_format_result EQUAL 0)
  message(
    FATAL_ERROR
      "clang-format ${ULOG_FORMAT_MODE} failed. Run the ulog-format target to fix formatting."
  )
endif()
