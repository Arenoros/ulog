add_test(NAME ulog.independence
         COMMAND "${CMAKE_COMMAND}" "-DULOG_SOURCE_DIR=${PROJECT_SOURCE_DIR}" -P
                 "${PROJECT_SOURCE_DIR}/cmake/CheckIndependence.cmake"
)
set_tests_properties(ulog.independence PROPERTIES LABELS "architecture")

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
  set_tests_properties(
    ulog.script-tools PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" LABELS "tooling;unit"
  )
  set_tests_properties(
    ulog.corpus PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" LABELS "migration;tooling"
  )
endif()

if(ULOG_BUILD_UNIT_TESTS)
  find_package(GTest CONFIG REQUIRED)
  add_executable(ulog-unit-tests version_test.cpp memory_resource_test.cpp)
  target_compile_features(ulog-unit-tests PRIVATE cxx_std_20)
  target_include_directories(ulog-unit-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(ulog-unit-tests PRIVATE ulog::ulog GTest::gtest_main)
  ulog_configure_target(ulog-unit-tests)
  add_test(NAME ulog.unit COMMAND ulog-unit-tests)
  set_tests_properties(ulog.unit PROPERTIES LABELS "unit")
endif()

if(ULOG_BUILD_STRESS_TESTS)
  find_package(Threads REQUIRED)
  add_executable(ulog-stress-tests stress/memory_resource_stress.cpp)
  target_compile_features(ulog-stress-tests PRIVATE cxx_std_20)
  target_include_directories(ulog-stress-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(ulog-stress-tests PRIVATE ulog::ulog Threads::Threads)
  ulog_configure_target(ulog-stress-tests)
  add_test(NAME ulog.stress COMMAND ulog-stress-tests)
  set_tests_properties(ulog.stress PROPERTIES LABELS "stress")
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
  set_tests_properties(ulog.dependencies PROPERTIES LABELS "dependencies")
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
  set_tests_properties(ulog.package-consumer PROPERTIES LABELS "integration;package")
endif()
