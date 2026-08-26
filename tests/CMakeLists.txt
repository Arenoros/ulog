add_test(NAME ulog.independence
         COMMAND "${CMAKE_COMMAND}" "-DULOG_SOURCE_DIR=${PROJECT_SOURCE_DIR}" -P
                 "${PROJECT_SOURCE_DIR}/cmake/CheckIndependence.cmake"
)
set_tests_properties(ulog.independence PROPERTIES LABELS "architecture" TIMEOUT 30)

if(PROJECT_IS_TOP_LEVEL)
  find_package(
    Python3
    COMPONENTS Interpreter
    REQUIRED
  )
  add_test(NAME ulog.script-tools COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s
                                          "${CMAKE_CURRENT_SOURCE_DIR}/scripts" -p "*_test.py"
  )
  add_test(
    NAME ulog.corpus
    COMMAND
      "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/baseline_corpus.py" validate
      --corpus-root "${PROJECT_SOURCE_DIR}/docs/migration/corpus" --manifest
      "${PROJECT_SOURCE_DIR}/docs/migration/capability-manifest.md"
  )
  add_test(
    NAME ulog.parity-scenarios
    COMMAND
      "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/non_format_scenarios.py" validate
      --catalog "${PROJECT_SOURCE_DIR}/docs/migration/non-format-parity-scenarios.md" --manifest
      "${PROJECT_SOURCE_DIR}/docs/migration/capability-manifest.md" --baseline
      "${PROJECT_SOURCE_DIR}/docs/migration/baseline.md"
  )
  add_test(
    NAME ulog.frontend-hot-path-contract
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/frontend_hot_path_contract.py"
            --source-dir "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(
    ulog.script-tools PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" LABELS "tooling;unit"
                                 TIMEOUT 90
  )
  set_tests_properties(
    ulog.corpus PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" LABELS "migration;tooling"
                           TIMEOUT 30
  )
  set_tests_properties(
    ulog.parity-scenarios PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" LABELS
                                     "migration;tooling" TIMEOUT 30
  )
  set_tests_properties(
    ulog.frontend-hot-path-contract PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" LABELS
                                               "architecture;frontend;performance" TIMEOUT 10
  )
endif()

if(ULOG_BUILD_UNIT_TESTS OR ULOG_BUILD_STRESS_TESTS)
  find_package(Threads REQUIRED)
endif()

set(_ulog_producer_test_library ulog::ulog)
if(BUILD_SHARED_LIBS AND (ULOG_BUILD_UNIT_TESTS OR ULOG_BUILD_STRESS_TESTS))
  # Private producer symbols stay out of the shared ABI. Tests link the same sources privately.
  add_library(ulog-producer-test-support STATIC ${ULOG_PRODUCER_SOURCES})
  target_compile_features(ulog-producer-test-support PRIVATE cxx_std_20)
  target_include_directories(
    ulog-producer-test-support PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include"
                                       "${ULOG_GENERATED_INCLUDE_DIR}"
  )
  target_link_libraries(ulog-producer-test-support PUBLIC ulog::ulog)
  ulog_configure_target(ulog-producer-test-support)
  set(_ulog_producer_test_library ulog-producer-test-support)
endif()

if(ULOG_BUILD_UNIT_TESTS)
  find_package(GTest CONFIG REQUIRED)
  add_executable(
    ulog-unit-tests
    default_logger_probe.cpp
    default_logger_test.cpp
    frontend_test.cpp
    log_macro_test.cpp
    memory_resource_test.cpp
    producer_kernel_test.cpp
    version_test.cpp
  )
  target_compile_features(ulog-unit-tests PRIVATE cxx_std_20)
  target_include_directories(
    ulog-unit-tests PRIVATE "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}"
  )
  target_link_libraries(
    ulog-unit-tests PRIVATE ${_ulog_producer_test_library} GTest::gtest_main Threads::Threads
  )
  ulog_configure_target(ulog-unit-tests)
  add_test(NAME ulog.unit COMMAND ulog-unit-tests)
  set_tests_properties(ulog.unit PROPERTIES LABELS "unit" TIMEOUT 20)

  foreach(_ulog_cutoff RANGE 0 5)
    add_executable(ulog-log-cutoff-${_ulog_cutoff} log_cutoff_probe.cpp)
    target_compile_features(ulog-log-cutoff-${_ulog_cutoff} PRIVATE cxx_std_20)
    target_compile_definitions(
      ulog-log-cutoff-${_ulog_cutoff} PRIVATE ULOG_COMPILE_TIME_MIN_LEVEL=${_ulog_cutoff}
                                              ULOG_EXPECTED_MIN_LEVEL=${_ulog_cutoff}
    )
    target_link_libraries(ulog-log-cutoff-${_ulog_cutoff} PRIVATE ulog::ulog)
    ulog_configure_target(ulog-log-cutoff-${_ulog_cutoff})
    add_test(NAME ulog.log-cutoff-${_ulog_cutoff} COMMAND ulog-log-cutoff-${_ulog_cutoff})
    set_tests_properties(
      ulog.log-cutoff-${_ulog_cutoff} PROPERTIES LABELS "unit;frontend;compile" TIMEOUT 10
    )
  endforeach()

  if(NOT ULOG_ENABLE_TSAN)
    # This test interposes global allocation functions, whose symbols are also owned by TSan.
    add_executable(ulog-producer-allocation-test producer_kernel_allocation_test.cpp)
    target_compile_features(ulog-producer-allocation-test PRIVATE cxx_std_20)
    target_include_directories(
      ulog-producer-allocation-test PRIVATE "${PROJECT_SOURCE_DIR}/src"
                                            "${PROJECT_SOURCE_DIR}/benchmarks"
    )
    target_link_libraries(ulog-producer-allocation-test PRIVATE ${_ulog_producer_test_library})
    ulog_configure_target(ulog-producer-allocation-test)
    add_test(NAME ulog.producer-allocation COMMAND ulog-producer-allocation-test)
    set_tests_properties(
      ulog.producer-allocation PROPERTIES LABELS "unit;producer;allocation" TIMEOUT 10
    )

    add_executable(ulog-frontend-performance-test frontend_performance_test.cpp)
    target_compile_features(ulog-frontend-performance-test PRIVATE cxx_std_20)
    target_compile_definitions(ulog-frontend-performance-test PRIVATE ULOG_COMPILE_TIME_MIN_LEVEL=5)
    target_include_directories(
      ulog-frontend-performance-test PRIVATE "${PROJECT_SOURCE_DIR}/src"
                                             "${PROJECT_SOURCE_DIR}/benchmarks"
    )
    target_link_libraries(ulog-frontend-performance-test PRIVATE ${_ulog_producer_test_library})
    ulog_configure_target(ulog-frontend-performance-test)
    add_test(NAME ulog.frontend-performance COMMAND ulog-frontend-performance-test)
    set_tests_properties(
      ulog.frontend-performance PROPERTIES LABELS "unit;frontend;performance;allocation" TIMEOUT 10
    )
  endif()

  add_executable(ulog-frontend-contract frontend_contract_test.cpp)
  target_compile_features(ulog-frontend-contract PRIVATE cxx_std_20)
  target_compile_definitions(ulog-frontend-contract PRIVATE ULOG_COMPILE_TIME_MIN_LEVEL=5)
  target_link_libraries(ulog-frontend-contract PRIVATE ulog::ulog)
  ulog_configure_target(ulog-frontend-contract)
  add_test(NAME ulog.frontend-contract COMMAND ulog-frontend-contract)
  set_tests_properties(ulog.frontend-contract PROPERTIES LABELS "unit;frontend" TIMEOUT 10)
endif()

if(ULOG_BUILD_STRESS_TESTS)
  add_executable(ulog-default-logger-stress stress/default_logger_stress.cpp)
  target_compile_features(ulog-default-logger-stress PRIVATE cxx_std_20)
  target_include_directories(ulog-default-logger-stress PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(ulog-default-logger-stress PRIVATE ulog::ulog Threads::Threads)
  ulog_configure_target(ulog-default-logger-stress)
  add_test(NAME ulog.default-logger-stress COMMAND ulog-default-logger-stress)
  set_tests_properties(
    ulog.default-logger-stress PROPERTIES LABELS "stress;default-logger;concurrency" TIMEOUT 10
  )

  add_executable(ulog-stress-tests stress/memory_resource_stress.cpp)
  target_compile_features(ulog-stress-tests PRIVATE cxx_std_20)
  target_include_directories(ulog-stress-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(ulog-stress-tests PRIVATE ulog::ulog Threads::Threads)
  ulog_configure_target(ulog-stress-tests)
  add_test(NAME ulog.stress COMMAND ulog-stress-tests)
  set_tests_properties(ulog.stress PROPERTIES LABELS "stress" TIMEOUT 10)

  add_executable(ulog-producer-kernel-stress stress/producer_kernel_stress.cpp)
  target_compile_features(ulog-producer-kernel-stress PRIVATE cxx_std_20)
  target_include_directories(ulog-producer-kernel-stress PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(
    ulog-producer-kernel-stress PRIVATE ${_ulog_producer_test_library} Threads::Threads
  )
  ulog_configure_target(ulog-producer-kernel-stress)
  add_test(NAME ulog.producer-kernel-stress COMMAND ulog-producer-kernel-stress)
  set_tests_properties(
    ulog.producer-kernel-stress PROPERTIES LABELS "stress;producer;concurrency" TIMEOUT 10
  )
endif()

if(ULOG_BUILD_DEPENDENCY_SMOKE_TESTS)
  find_package(fmt 12 CONFIG REQUIRED)
  find_package(libuv 1.51 CONFIG REQUIRED)

  if(TARGET libuv::uv_a)
    set(_ulog_libuv_target libuv::uv_a)
  elseif(TARGET libuv::uv)
    set(_ulog_libuv_target libuv::uv)
  elseif(TARGET uv_a)
    set(_ulog_libuv_target uv_a)
  elseif(TARGET uv)
    set(_ulog_libuv_target uv)
  else()
    message(
      FATAL_ERROR
        "libuv was found but exported no supported target. Expected libuv::uv_a, libuv::uv, uv_a, or uv. Use the Conan profile documented in docs/dependencies.md."
    )
  endif()

  add_executable(ulog-dependency-smoke dependency_smoke.cpp)
  target_compile_features(ulog-dependency-smoke PRIVATE cxx_std_20)
  target_link_libraries(ulog-dependency-smoke PRIVATE ulog::ulog fmt::fmt ${_ulog_libuv_target})
  ulog_configure_target(ulog-dependency-smoke)
  add_test(NAME ulog.dependencies COMMAND ulog-dependency-smoke)
  set_tests_properties(ulog.dependencies PROPERTIES LABELS "dependencies" TIMEOUT 20)
endif()

if(ULOG_BUILD_PACKAGE_TESTS)
  find_program(_ulog_consumer_make_program NAMES "${CMAKE_MAKE_PROGRAM}")
  if(NOT _ulog_consumer_make_program)
    set(_ulog_consumer_make_program "${CMAKE_MAKE_PROGRAM}")
  endif()
  configure_file(
    cmake/RunInstalledConsumer.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/RunInstalledConsumer.cmake"
    @ONLY
  )
  add_test(
    NAME ulog.package-consumer
    COMMAND
      "${CMAKE_COMMAND}" -DULOG_TEST_CONFIG=$<CONFIG>
      "-DULOG_TEST_MSVC_RUNTIME_LIBRARY=$<GENEX_EVAL:$<TARGET_PROPERTY:ulog,MSVC_RUNTIME_LIBRARY>>"
      -P "${CMAKE_CURRENT_BINARY_DIR}/RunInstalledConsumer.cmake"
  )
  set_tests_properties(ulog.package-consumer PROPERTIES LABELS "integration;package" TIMEOUT 120)
endif()
