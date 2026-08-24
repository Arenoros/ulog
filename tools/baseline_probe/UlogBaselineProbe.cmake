include_guard(GLOBAL)

function(ulog_add_baseline_probe)
  set(_ulog_one_value_args TARGET SOURCE_DIR DESCRIPTION)
  cmake_parse_arguments(ULOG_PROBE "" "${_ulog_one_value_args}" "" ${ARGN})
  foreach(_ulog_required_arg IN LISTS _ulog_one_value_args)
    if(NOT ULOG_PROBE_${_ulog_required_arg})
      message(FATAL_ERROR "ulog_add_baseline_probe requires ${_ulog_required_arg}.")
    endif()
  endforeach()

  set(ULOG_BASELINE_SOURCE
      ""
      CACHE PATH "Clean checkout of the pinned userver baseline"
  )
  if(NOT IS_DIRECTORY "${ULOG_BASELINE_SOURCE}")
    message(
      FATAL_ERROR
        "ULOG_BASELINE_SOURCE must name a clean userver checkout. Configure with -DULOG_BASELINE_SOURCE=/absolute/path/to/userver and retry."
    )
  endif()

  find_package(Git REQUIRED)

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${ULOG_BASELINE_SOURCE}" rev-parse --verify HEAD^{commit}
    RESULT_VARIABLE ULOG_REVISION_RESULT
    OUTPUT_VARIABLE ULOG_CHECKOUT_REVISION
    ERROR_VARIABLE ULOG_REVISION_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT ULOG_REVISION_RESULT EQUAL 0)
    message(
      FATAL_ERROR
        "Unable to read ULOG_BASELINE_SOURCE revision: ${ULOG_REVISION_ERROR}. Pass a valid Git checkout and retry."
    )
  endif()

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${ULOG_BASELINE_SOURCE}" status --short
            --untracked-files=all
    RESULT_VARIABLE ULOG_STATUS_RESULT
    OUTPUT_VARIABLE ULOG_WORKTREE_CHANGES
    ERROR_VARIABLE ULOG_STATUS_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT ULOG_STATUS_RESULT EQUAL 0)
    message(
      FATAL_ERROR
        "Unable to verify ULOG_BASELINE_SOURCE worktree: ${ULOG_STATUS_ERROR}. Fix the checkout and retry."
    )
  endif()
  if(NOT ULOG_WORKTREE_CHANGES STREQUAL "")
    message(
      FATAL_ERROR
        "ULOG_BASELINE_SOURCE must be clean before capture; found:\n${ULOG_WORKTREE_CHANGES}\nCommit, stash, or revert tracked changes and remove untracked files, then retry."
    )
  endif()

  set(ULOG_BASELINE_DOCUMENT "${ULOG_PROBE_SOURCE_DIR}/../../docs/migration/baseline.md")
  file(READ "${ULOG_BASELINE_DOCUMENT}" ULOG_BASELINE_METADATA)
  string(REGEX MATCH "repository:[ \t]*([^\r\n ]+)" ULOG_REPOSITORY_MATCH
               "${ULOG_BASELINE_METADATA}"
  )
  set(ULOG_BASELINE_REPOSITORY "${CMAKE_MATCH_1}")
  string(REGEX MATCH "commit:[ \t]*([0-9a-f]+)" ULOG_REVISION_MATCH
               "${ULOG_BASELINE_METADATA}"
  )
  set(ULOG_EXPECTED_REVISION "${CMAKE_MATCH_1}")
  string(LENGTH "${ULOG_EXPECTED_REVISION}" ULOG_EXPECTED_REVISION_LENGTH)
  if(ULOG_BASELINE_REPOSITORY STREQUAL "" OR NOT ULOG_EXPECTED_REVISION_LENGTH EQUAL 40)
    message(
      FATAL_ERROR
        "${ULOG_BASELINE_DOCUMENT} must contain one repository and one lowercase 40-character commit. Restore it and retry."
    )
  endif()
  if(NOT ULOG_CHECKOUT_REVISION STREQUAL ULOG_EXPECTED_REVISION)
    message(
      FATAL_ERROR
        "ULOG_BASELINE_SOURCE is at ${ULOG_CHECKOUT_REVISION}, but ${ULOG_EXPECTED_REVISION} is required. Check out the documented revision and retry."
    )
  endif()

  set(ULOG_ARCHIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/userver-source.tar")
  set(ULOG_SNAPSHOT_DIR "${CMAKE_CURRENT_BINARY_DIR}/userver-source")
  file(REMOVE "${ULOG_ARCHIVE_PATH}")
  file(REMOVE_RECURSE "${ULOG_SNAPSHOT_DIR}")
  file(MAKE_DIRECTORY "${ULOG_SNAPSHOT_DIR}")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${ULOG_BASELINE_SOURCE}" archive --format=tar
            "--output=${ULOG_ARCHIVE_PATH}" "${ULOG_CHECKOUT_REVISION}"
    RESULT_VARIABLE ULOG_ARCHIVE_RESULT
    ERROR_VARIABLE ULOG_ARCHIVE_ERROR
  )
  if(NOT ULOG_ARCHIVE_RESULT EQUAL 0)
    message(
      FATAL_ERROR
        "Unable to archive the verified baseline: ${ULOG_ARCHIVE_ERROR}. Check Git permissions and retry."
    )
  endif()

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" get-tar-commit-id
    INPUT_FILE "${ULOG_ARCHIVE_PATH}"
    RESULT_VARIABLE ULOG_ARCHIVE_REVISION_RESULT
    OUTPUT_VARIABLE ULOG_ARCHIVE_REVISION
    ERROR_VARIABLE ULOG_ARCHIVE_REVISION_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT ULOG_ARCHIVE_REVISION_RESULT EQUAL 0 OR NOT ULOG_ARCHIVE_REVISION STREQUAL
                                                   ULOG_CHECKOUT_REVISION
  )
    message(
      FATAL_ERROR
        "The baseline archive attests '${ULOG_ARCHIVE_REVISION}', expected '${ULOG_CHECKOUT_REVISION}': ${ULOG_ARCHIVE_REVISION_ERROR}. Remove this probe build directory and retry."
    )
  endif()

  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xf "${ULOG_ARCHIVE_PATH}"
    WORKING_DIRECTORY "${ULOG_SNAPSHOT_DIR}"
    RESULT_VARIABLE ULOG_EXTRACT_RESULT
    ERROR_VARIABLE ULOG_EXTRACT_ERROR
  )
  if(NOT ULOG_EXTRACT_RESULT EQUAL 0)
    message(
      FATAL_ERROR
        "Unable to extract the verified baseline archive: ${ULOG_EXTRACT_ERROR}. Check the build directory and retry."
    )
  endif()

  configure_file(
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/baseline_attestation.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/baseline_attestation.hpp" @ONLY
  )

  set(ULOG_DISABLED_USERVER_OPTIONS
      USERVER_BUILD_SAMPLES
      USERVER_BUILD_TESTS
      USERVER_FEATURE_CHAOTIC
      USERVER_FEATURE_CHAOTIC_OPENAPI
      USERVER_FEATURE_CORE
      USERVER_FEATURE_JEMALLOC
      USERVER_FEATURE_TESTSUITE
      USERVER_FEATURE_UTEST
  )
  foreach(ULOG_OPTION IN LISTS ULOG_DISABLED_USERVER_OPTIONS)
    set(${ULOG_OPTION}
        OFF
        CACHE BOOL "Disabled for the Ulog baseline ${ULOG_PROBE_DESCRIPTION} probe" FORCE
    )
  endforeach()
  add_subdirectory("${ULOG_SNAPSHOT_DIR}" userver-build EXCLUDE_FROM_ALL)

  add_executable(${ULOG_PROBE_TARGET} "${ULOG_PROBE_SOURCE_DIR}/main.cpp")
  target_compile_features(${ULOG_PROBE_TARGET} PRIVATE cxx_std_20)
  target_include_directories(
    ${ULOG_PROBE_TARGET}
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}"
  )
  target_link_libraries(${ULOG_PROBE_TARGET} PRIVATE userver::universal)
  set_target_properties(${ULOG_PROBE_TARGET} PROPERTIES CXX_EXTENSIONS OFF)
endfunction()
