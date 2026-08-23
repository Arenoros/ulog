include_guard(GLOBAL)

function(ulog_configure_target target)
  set_target_properties("${target}" PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

  if(MSVC)
    target_compile_options("${target}" PRIVATE /W4 /EHsc /permissive- /Zc:__cplusplus /utf-8)
    if(ULOG_WARNINGS_AS_ERRORS)
      target_compile_options("${target}" PRIVATE /WX)
    endif()
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
      "${target}"
      PRIVATE -Wall
              -Wextra
              -Wpedantic
              -Wconversion
              -Wsign-conversion
              -Wshadow
              -Wformat=2
    )
    if(ULOG_WARNINGS_AS_ERRORS)
      target_compile_options("${target}" PRIVATE -Werror)
    endif()
  endif()

  if(ULOG_ENABLE_CLANG_TIDY)
    find_program(ULOG_CLANG_TIDY_EXECUTABLE NAMES clang-tidy)
    if(NOT ULOG_CLANG_TIDY_EXECUTABLE)
      message(
        FATAL_ERROR
          "ULOG_ENABLE_CLANG_TIDY=ON, but clang-tidy was not found. Install LLVM clang-tidy or set ULOG_CLANG_TIDY_EXECUTABLE."
      )
    endif()
    set(_ulog_clang_tidy_command "${ULOG_CLANG_TIDY_EXECUTABLE}" --warnings-as-errors=*)
    if(MSVC)
      list(APPEND _ulog_clang_tidy_command --extra-arg-before=/EHsc)
    endif()
    set_property(TARGET "${target}" PROPERTY CXX_CLANG_TIDY "${_ulog_clang_tidy_command}")
  endif()

  set(_ulog_sanitizer_compile_flags)
  set(_ulog_sanitizer_link_flags)
  if(ULOG_ENABLE_ASAN)
    list(APPEND _ulog_sanitizer_compile_flags -fsanitize=address -fno-omit-frame-pointer)
    list(APPEND _ulog_sanitizer_link_flags -fsanitize=address)
  endif()
  if(ULOG_ENABLE_UBSAN)
    list(APPEND _ulog_sanitizer_compile_flags -fsanitize=undefined -fno-sanitize-recover=all)
    list(APPEND _ulog_sanitizer_link_flags -fsanitize=undefined)
  endif()
  if(ULOG_ENABLE_TSAN)
    list(APPEND _ulog_sanitizer_compile_flags -fsanitize=thread -fno-omit-frame-pointer)
    list(APPEND _ulog_sanitizer_link_flags -fsanitize=thread)
  endif()

  if(_ulog_sanitizer_compile_flags)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      message(
        FATAL_ERROR
          "The selected Ulog sanitizer configuration requires GCC or Clang. Disable ULOG_ENABLE_*SAN or select a supported compiler."
      )
    endif()
    target_compile_options("${target}" PRIVATE ${_ulog_sanitizer_compile_flags})
    get_target_property(_ulog_target_type "${target}" TYPE)
    if(_ulog_target_type STREQUAL "EXECUTABLE")
      target_link_options("${target}" PRIVATE ${_ulog_sanitizer_link_flags})
    else()
      target_link_options("${target}" PUBLIC ${_ulog_sanitizer_link_flags})
    endif()
  endif()
endfunction()

function(ulog_add_tooling_targets)
  add_custom_target(
    ulog-format
    COMMAND "${CMAKE_COMMAND}" "-DULOG_SOURCE_DIR=${PROJECT_SOURCE_DIR}" -DULOG_FORMAT_MODE=fix -P
            "${PROJECT_SOURCE_DIR}/cmake/RunClangFormat.cmake"
    VERBATIM
  )
  add_custom_target(
    ulog-format-check
    COMMAND "${CMAKE_COMMAND}" "-DULOG_SOURCE_DIR=${PROJECT_SOURCE_DIR}" -DULOG_FORMAT_MODE=check -P
            "${PROJECT_SOURCE_DIR}/cmake/RunClangFormat.cmake"
    VERBATIM
  )
  add_custom_target(
    ulog-check-independence
    COMMAND "${CMAKE_COMMAND}" "-DULOG_SOURCE_DIR=${PROJECT_SOURCE_DIR}" -P
            "${PROJECT_SOURCE_DIR}/cmake/CheckIndependence.cmake"
    VERBATIM
  )
endfunction()
