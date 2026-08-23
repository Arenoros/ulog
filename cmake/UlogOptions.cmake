include_guard(GLOBAL)

function(ulog_define_options)
  if(PROJECT_IS_TOP_LEVEL)
    set(_ulog_top_level_default ON)
  else()
    set(_ulog_top_level_default OFF)
  endif()

  option(ULOG_BUILD_UNIT_TESTS "Build Ulog unit tests" "${_ulog_top_level_default}")
  option(ULOG_BUILD_PACKAGE_TESTS "Test the installed CMake package" "${_ulog_top_level_default}")
  option(ULOG_BUILD_STRESS_TESTS "Build deterministic stress tests" OFF)
  option(ULOG_BUILD_BENCHMARKS "Build Google Benchmark executables" OFF)
  option(ULOG_BUILD_DEPENDENCY_SMOKE_TESTS "Validate planned fmt and libuv dependencies" OFF)
  option(ULOG_WARNINGS_AS_ERRORS "Treat warnings in Ulog-owned targets as errors" OFF)
  option(ULOG_ENABLE_CLANG_TIDY "Run clang-tidy while compiling Ulog-owned targets" OFF)
  option(ULOG_ENABLE_ASAN "Enable AddressSanitizer for Ulog-owned targets" OFF)
  option(ULOG_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer for Ulog-owned targets" OFF)
  option(ULOG_ENABLE_TSAN "Enable ThreadSanitizer for Ulog-owned targets" OFF)

  if(ULOG_ENABLE_TSAN AND (ULOG_ENABLE_ASAN OR ULOG_ENABLE_UBSAN))
    message(
      FATAL_ERROR
        "ULOG_ENABLE_TSAN cannot be combined with ASan or UBSan. Configure a separate TSan build directory."
    )
  endif()

  if(NOT BUILD_TESTING
     AND (ULOG_BUILD_UNIT_TESTS
          OR ULOG_BUILD_PACKAGE_TESTS
          OR ULOG_BUILD_STRESS_TESTS
          OR ULOG_BUILD_BENCHMARKS
          OR ULOG_BUILD_DEPENDENCY_SMOKE_TESTS
         )
  )
    message(
      FATAL_ERROR
        "A Ulog test or benchmark option is enabled while BUILD_TESTING=OFF. Set BUILD_TESTING=ON or disable all ULOG_BUILD_* test options."
    )
  endif()
endfunction()
