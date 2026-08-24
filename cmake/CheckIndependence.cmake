if(NOT DEFINED ULOG_SOURCE_DIR)
  message(FATAL_ERROR "ULOG_SOURCE_DIR is required. Pass -DULOG_SOURCE_DIR=<repository root>.")
endif()

file(REAL_PATH "${ULOG_SOURCE_DIR}" ULOG_SOURCE_DIR BASE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
if(NOT EXISTS "${ULOG_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "ULOG_SOURCE_DIR does not point to the Ulog repository: ${ULOG_SOURCE_DIR}")
endif()

set(_ulog_forbidden_regex "[Uu][Ss][Ee][Rr][Vv][Ee][Rr]")
set(_ulog_scan_roots
    "${ULOG_SOURCE_DIR}/include"
    "${ULOG_SOURCE_DIR}/src"
    "${ULOG_SOURCE_DIR}/tests"
    "${ULOG_SOURCE_DIR}/benchmarks"
    "${ULOG_SOURCE_DIR}/test_package"
    "${ULOG_SOURCE_DIR}/cmake"
    "${ULOG_SOURCE_DIR}/conan"
    "${ULOG_SOURCE_DIR}/scripts"
    "${ULOG_SOURCE_DIR}/tools"
    "${ULOG_SOURCE_DIR}/.github"
)
set(_ulog_scan_files "${ULOG_SOURCE_DIR}/CMakeLists.txt" "${ULOG_SOURCE_DIR}/CMakePresets.json"
                     "${ULOG_SOURCE_DIR}/conanfile.py" "${ULOG_SOURCE_DIR}/conan.lock"
)

foreach(_ulog_root IN LISTS _ulog_scan_roots)
  if(EXISTS "${_ulog_root}")
    file(
      GLOB_RECURSE _ulog_root_files
      LIST_DIRECTORIES FALSE
      "${_ulog_root}/*"
    )
    list(FILTER _ulog_root_files EXCLUDE REGEX "/(build[^/]*|out|__pycache__)/")
    # Baseline probes are manual evidence generators, outside Ulog's build graph.
    list(FILTER _ulog_root_files EXCLUDE REGEX "/tools/baseline_(json_|text_)?probe/")
    list(FILTER _ulog_root_files EXCLUDE REGEX "/CMakeUserPresets\\.json$")
    list(APPEND _ulog_scan_files ${_ulog_root_files})
  endif()
endforeach()

set(_ulog_violations)
foreach(_ulog_file IN LISTS _ulog_scan_files)
  if(EXISTS "${_ulog_file}")
    file(READ "${_ulog_file}" _ulog_contents)
    if(_ulog_contents MATCHES "${_ulog_forbidden_regex}")
      file(RELATIVE_PATH _ulog_relative "${ULOG_SOURCE_DIR}" "${_ulog_file}")
      list(APPEND _ulog_violations "${_ulog_relative}")
    endif()
  endif()
endforeach()

if(_ulog_violations)
  list(JOIN _ulog_violations "\n  - " _ulog_violation_list)
  message(
    FATAL_ERROR
      "Forbidden source dependency token found in:\n  - ${_ulog_violation_list}\nMove compatibility glue to the consuming project."
  )
endif()

message(STATUS "Ulog independence check passed")
