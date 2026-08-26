find_package(benchmark 1.9 CONFIG REQUIRED)
find_package(fmt 12 CONFIG REQUIRED)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)

add_library(
  ulog-workload-support STATIC
  prototypes/reservation/central_reservation_kernel.cpp
  prototypes/reservation/central_reservation_ledger.cpp
  prototypes/reservation/producer_credit_ledger.cpp
  prototypes/reservation/producer_credit_reservation_kernel.cpp
  support/process_cpu_clock.cpp
  support/reference_kernel.cpp
  support/workload_harness.cpp
)
target_compile_features(ulog-workload-support PUBLIC cxx_std_20)
target_include_directories(ulog-workload-support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ulog-workload-support PUBLIC Threads::Threads)
ulog_configure_target(ulog-workload-support)

add_executable(ulog-benchmarks version_benchmark.cpp)
target_compile_features(ulog-benchmarks PRIVATE cxx_std_20)
target_link_libraries(ulog-benchmarks PRIVATE ulog::ulog benchmark::benchmark)
ulog_configure_target(ulog-benchmarks)

add_executable(ulog-workload-benchmarks workload_benchmark.cpp)
target_compile_features(ulog-workload-benchmarks PRIVATE cxx_std_20)
target_link_libraries(ulog-workload-benchmarks PRIVATE ulog-workload-support benchmark::benchmark)
ulog_configure_target(ulog-workload-benchmarks)

add_library(ulog-record-storage-support INTERFACE)
target_compile_features(ulog-record-storage-support INTERFACE cxx_std_20)
target_link_libraries(ulog-record-storage-support INTERFACE ulog-workload-support fmt::fmt)

add_executable(ulog-record-storage-benchmarks record_storage_benchmark.cpp)
target_compile_features(ulog-record-storage-benchmarks PRIVATE cxx_std_20)
target_link_libraries(
  ulog-record-storage-benchmarks PRIVATE ulog-record-storage-support benchmark::benchmark
)
ulog_configure_target(ulog-record-storage-benchmarks)

add_executable(ulog-ingress-topology-benchmarks ingress_topology_benchmark.cpp)
target_compile_features(ulog-ingress-topology-benchmarks PRIVATE cxx_std_20)
target_link_libraries(
  ulog-ingress-topology-benchmarks PRIVATE ulog-record-storage-support benchmark::benchmark
)
ulog_configure_target(ulog-ingress-topology-benchmarks)

add_executable(ulog-composed-producer-benchmarks composed_producer_benchmark.cpp)
target_compile_features(ulog-composed-producer-benchmarks PRIVATE cxx_std_20)
target_link_libraries(
  ulog-composed-producer-benchmarks PRIVATE ulog-record-storage-support benchmark::benchmark
)
ulog_configure_target(ulog-composed-producer-benchmarks)

if(NOT ULOG_ENABLE_TSAN)
  set(_ulog_frontend_benchmark_library ulog::ulog)
  if(BUILD_SHARED_LIBS)
    # Private producer symbols stay out of the shared ABI. The benchmark links the same sources.
    add_library(ulog-frontend-benchmark-support STATIC ${ULOG_PRODUCER_SOURCES})
    target_compile_features(ulog-frontend-benchmark-support PRIVATE cxx_std_20)
    target_include_directories(
      ulog-frontend-benchmark-support
      PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include"
              "${ULOG_GENERATED_INCLUDE_DIR}"
    )
    target_link_libraries(ulog-frontend-benchmark-support PUBLIC ulog::ulog)
    ulog_configure_target(ulog-frontend-benchmark-support)
    set(_ulog_frontend_benchmark_library ulog-frontend-benchmark-support)
  endif()

  add_executable(ulog-frontend-benchmarks frontend_benchmark.cpp)
  target_compile_features(ulog-frontend-benchmarks PRIVATE cxx_std_20)
  target_compile_definitions(ulog-frontend-benchmarks PRIVATE ULOG_COMPILE_TIME_MIN_LEVEL=5)
  target_include_directories(ulog-frontend-benchmarks PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(
    ulog-frontend-benchmarks PRIVATE ${_ulog_frontend_benchmark_library} ulog-workload-support
                                     benchmark::benchmark
  )
  ulog_configure_target(ulog-frontend-benchmarks)
endif()

add_executable(ulog-workload-harness-test workload_harness_test.cpp)
target_compile_features(ulog-workload-harness-test PRIVATE cxx_std_20)
target_link_libraries(ulog-workload-harness-test PRIVATE ulog-workload-support)
ulog_configure_target(ulog-workload-harness-test)

add_executable(ulog-reservation-ledger-test reservation_ledger_test.cpp)
target_compile_features(ulog-reservation-ledger-test PRIVATE cxx_std_20)
target_link_libraries(ulog-reservation-ledger-test PRIVATE ulog-workload-support)
ulog_configure_target(ulog-reservation-ledger-test)

if(NOT ULOG_ENABLE_TSAN)
  # This test interposes global allocation functions, whose symbols are also owned by TSan.
  add_executable(ulog-record-storage-test record_storage_test.cpp)
  target_compile_features(ulog-record-storage-test PRIVATE cxx_std_20)
  target_link_libraries(ulog-record-storage-test PRIVATE ulog-record-storage-support)
  ulog_configure_target(ulog-record-storage-test)

  add_executable(
    ulog-composed-producer-allocation-test composed_producer_kernel_allocation_test.cpp
  )
  target_compile_features(ulog-composed-producer-allocation-test PRIVATE cxx_std_20)
  target_link_libraries(ulog-composed-producer-allocation-test PRIVATE ulog-record-storage-support)
  ulog_configure_target(ulog-composed-producer-allocation-test)
endif()

add_executable(ulog-ingress-topology-test ingress_topology_test.cpp)
target_compile_features(ulog-ingress-topology-test PRIVATE cxx_std_20)
target_link_libraries(ulog-ingress-topology-test PRIVATE ulog-record-storage-support)
ulog_configure_target(ulog-ingress-topology-test)

add_executable(ulog-chunked-mpsc-test chunked_mpsc_test.cpp)
target_compile_features(ulog-chunked-mpsc-test PRIVATE cxx_std_20)
target_link_libraries(ulog-chunked-mpsc-test PRIVATE ulog-record-storage-support)
ulog_configure_target(ulog-chunked-mpsc-test)

add_executable(ulog-per-producer-lanes-test per_producer_lanes_test.cpp)
target_compile_features(ulog-per-producer-lanes-test PRIVATE cxx_std_20)
target_link_libraries(ulog-per-producer-lanes-test PRIVATE ulog-record-storage-support)
ulog_configure_target(ulog-per-producer-lanes-test)

add_executable(ulog-composed-producer-kernel-test composed_producer_kernel_test.cpp)
target_compile_features(ulog-composed-producer-kernel-test PRIVATE cxx_std_20)
target_link_libraries(ulog-composed-producer-kernel-test PRIVATE ulog-record-storage-support)
ulog_configure_target(ulog-composed-producer-kernel-test)

if(ULOG_BUILD_STRESS_TESTS)
  add_executable(ulog-ingress-topology-stress ingress_topology_stress.cpp)
  target_compile_features(ulog-ingress-topology-stress PRIVATE cxx_std_20)
  target_link_libraries(
    ulog-ingress-topology-stress PRIVATE ulog-record-storage-support Threads::Threads
  )
  ulog_configure_target(ulog-ingress-topology-stress)

  add_executable(ulog-composed-producer-kernel-stress composed_producer_kernel_stress.cpp)
  target_compile_features(ulog-composed-producer-kernel-stress PRIVATE cxx_std_20)
  target_link_libraries(
    ulog-composed-producer-kernel-stress PRIVATE ulog-record-storage-support Threads::Threads
  )
  ulog_configure_target(ulog-composed-producer-kernel-stress)
endif()

add_test(NAME ulog.workload-harness-unit COMMAND ulog-workload-harness-test)
add_test(NAME ulog.reservation-ledger-unit COMMAND ulog-reservation-ledger-test)
if(TARGET ulog-record-storage-test)
  add_test(NAME ulog.record-storage-unit COMMAND ulog-record-storage-test)
endif()
if(TARGET ulog-composed-producer-allocation-test)
  add_test(NAME ulog.composed-producer-allocation COMMAND ulog-composed-producer-allocation-test)
endif()
add_test(NAME ulog.ingress-topology-unit COMMAND ulog-ingress-topology-test)
add_test(NAME ulog.chunked-mpsc-unit COMMAND ulog-chunked-mpsc-test)
add_test(NAME ulog.per-producer-lanes-unit COMMAND ulog-per-producer-lanes-test)
add_test(NAME ulog.composed-producer-kernel-unit COMMAND ulog-composed-producer-kernel-test)
add_test(
  NAME ulog.benchmark-schedule
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/check_benchmark_schedule.py"
          "$<TARGET_FILE:ulog-workload-benchmarks>"
)
add_test(NAME ulog.version-benchmark-smoke COMMAND ulog-benchmarks --benchmark_min_time=0.001s
                                                   --benchmark_color=false
)
add_test(
  NAME ulog.benchmark-smoke
  COMMAND
    ulog-workload-benchmarks --ulog_mode=smoke --benchmark_min_time=0.001s --benchmark_color=false
    "--benchmark_out=${PROJECT_BINARY_DIR}/benchmark-results.json" --benchmark_out_format=json
)
add_test(NAME ulog.benchmark-results
         COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/benchmark_results.py"
                 validate "${PROJECT_BINARY_DIR}/benchmark-results.json"
)
add_test(
  NAME ulog.record-storage-schedule
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/check_record_storage_schedule.py"
          "$<TARGET_FILE:ulog-record-storage-benchmarks>"
)
add_test(
  NAME ulog.record-storage-benchmark-smoke
  COMMAND
    ulog-record-storage-benchmarks --ulog_mode=smoke --benchmark_min_time=0.001s
    --benchmark_color=false "--benchmark_out=${PROJECT_BINARY_DIR}/record-storage-results.json"
    --benchmark_out_format=json
)
add_test(NAME ulog.record-storage-benchmark-results
         COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/record_storage_results.py"
                 validate "${PROJECT_BINARY_DIR}/record-storage-results.json"
)
add_test(NAME ulog.ingress-topology-schedule
         COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/check_ingress_schedule.py"
                 "$<TARGET_FILE:ulog-ingress-topology-benchmarks>"
)
add_test(
  NAME ulog.ingress-topology-benchmark-smoke
  COMMAND
    ulog-ingress-topology-benchmarks --ulog_mode=smoke --benchmark_min_time=0.001s
    --benchmark_color=false "--benchmark_out=${PROJECT_BINARY_DIR}/ingress-results.json"
    --benchmark_out_format=json
)
add_test(NAME ulog.ingress-topology-benchmark-results
         COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/ingress_results.py"
                 validate "${PROJECT_BINARY_DIR}/ingress-results.json"
)
add_test(
  NAME ulog.composed-producer-schedule
  COMMAND
    "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/check_composed_producer_schedule.py"
    "$<TARGET_FILE:ulog-composed-producer-benchmarks>"
)
add_test(
  NAME ulog.composed-producer-benchmark-smoke
  COMMAND
    ulog-composed-producer-benchmarks --ulog_mode=smoke --benchmark_min_time=0.001s
    --benchmark_color=false "--benchmark_out=${PROJECT_BINARY_DIR}/composed-producer-results.json"
    --benchmark_out_format=json
)
add_test(
  NAME ulog.composed-producer-benchmark-results
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/composed_producer_results.py"
          validate "${PROJECT_BINARY_DIR}/composed-producer-results.json"
)
if(TARGET ulog-frontend-benchmarks)
  add_test(
    NAME ulog.frontend-schedule
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/check_frontend_schedule.py"
            "$<TARGET_FILE:ulog-frontend-benchmarks>"
  )
  add_test(
    NAME ulog.frontend-benchmark-smoke
    COMMAND
      ulog-frontend-benchmarks --ulog_mode=smoke --benchmark_min_time=0.001s
      --benchmark_color=false "--benchmark_out=${PROJECT_BINARY_DIR}/frontend-results.json"
      --benchmark_out_format=json
  )
  add_test(NAME ulog.frontend-benchmark-results
           COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/frontend_results.py"
                   validate "${PROJECT_BINARY_DIR}/frontend-results.json"
  )
endif()
if(ULOG_BUILD_STRESS_TESTS)
  add_test(NAME ulog.ingress-topology-stress COMMAND ulog-ingress-topology-stress)
  add_test(NAME ulog.composed-producer-kernel-stress COMMAND ulog-composed-producer-kernel-stress)
endif()
set_tests_properties(ulog.workload-harness-unit PROPERTIES LABELS "benchmark;unit" TIMEOUT 20)
set_tests_properties(ulog.reservation-ledger-unit PROPERTIES LABELS "benchmark;unit" TIMEOUT 20)
if(TEST ulog.record-storage-unit)
  set_tests_properties(ulog.record-storage-unit PROPERTIES LABELS "benchmark;unit" TIMEOUT 20)
endif()
if(TEST ulog.composed-producer-allocation)
  set_tests_properties(
    ulog.composed-producer-allocation PROPERTIES LABELS "benchmark;unit;allocation" TIMEOUT 10
  )
endif()
set_tests_properties(ulog.ingress-topology-unit PROPERTIES LABELS "benchmark;unit" TIMEOUT 20)
set_tests_properties(ulog.chunked-mpsc-unit PROPERTIES LABELS "benchmark;unit" TIMEOUT 20)
set_tests_properties(ulog.per-producer-lanes-unit PROPERTIES LABELS "benchmark;unit" TIMEOUT 20)
set_tests_properties(
  ulog.composed-producer-kernel-unit PROPERTIES LABELS "benchmark;unit;concurrency" TIMEOUT 20
)
set_tests_properties(
  ulog.benchmark-schedule PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" LABELS
                                     "benchmark;tooling" TIMEOUT 30
)
set_tests_properties(ulog.version-benchmark-smoke PROPERTIES LABELS "benchmark" TIMEOUT 120)
set_tests_properties(ulog.benchmark-smoke PROPERTIES LABELS "benchmark" TIMEOUT 120)
set_tests_properties(
  ulog.benchmark-results
  PROPERTIES DEPENDS
             ulog.benchmark-smoke
             ENVIRONMENT
             "PYTHONDONTWRITEBYTECODE=1"
             LABELS
             "benchmark;tooling"
             TIMEOUT
             30
)
set_tests_properties(
  ulog.record-storage-schedule PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" LABELS
                                          "benchmark;tooling" TIMEOUT 30
)
set_tests_properties(ulog.record-storage-benchmark-smoke PROPERTIES LABELS "benchmark" TIMEOUT 120)
set_tests_properties(
  ulog.record-storage-benchmark-results
  PROPERTIES DEPENDS
             ulog.record-storage-benchmark-smoke
             ENVIRONMENT
             "PYTHONDONTWRITEBYTECODE=1"
             LABELS
             "benchmark;tooling"
             TIMEOUT
             30
)
set_tests_properties(
  ulog.ingress-topology-schedule PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" LABELS
                                            "benchmark;tooling" TIMEOUT 30
)
set_tests_properties(
  ulog.ingress-topology-benchmark-smoke PROPERTIES LABELS "benchmark" TIMEOUT 120
)
set_tests_properties(
  ulog.ingress-topology-benchmark-results
  PROPERTIES DEPENDS
             ulog.ingress-topology-benchmark-smoke
             ENVIRONMENT
             "PYTHONDONTWRITEBYTECODE=1"
             LABELS
             "benchmark;tooling"
             TIMEOUT
             30
)
set_tests_properties(
  ulog.composed-producer-benchmark-smoke PROPERTIES LABELS "benchmark" TIMEOUT 120
)
set_tests_properties(
  ulog.composed-producer-schedule PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" LABELS
                                             "benchmark;tooling" TIMEOUT 30
)
set_tests_properties(
  ulog.composed-producer-benchmark-results
  PROPERTIES DEPENDS
             ulog.composed-producer-benchmark-smoke
             ENVIRONMENT
             "PYTHONDONTWRITEBYTECODE=1"
             LABELS
             "benchmark;tooling"
             TIMEOUT
             30
)
if(TEST ulog.frontend-benchmark-smoke)
  set_tests_properties(
    ulog.frontend-schedule PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" LABELS
                                      "benchmark;frontend;performance;tooling" TIMEOUT 15
  )
  set_tests_properties(
    ulog.frontend-benchmark-smoke PROPERTIES LABELS "benchmark;frontend;performance" TIMEOUT 30
  )
  set_tests_properties(
    ulog.frontend-benchmark-results
    PROPERTIES DEPENDS
               ulog.frontend-benchmark-smoke
               ENVIRONMENT
               "PYTHONDONTWRITEBYTECODE=1"
               LABELS
               "benchmark;frontend;performance;tooling"
               TIMEOUT
               10
  )
endif()
if(ULOG_BUILD_STRESS_TESTS)
  set_tests_properties(
    ulog.ingress-topology-stress PROPERTIES LABELS "benchmark;stress;concurrency" TIMEOUT 10
  )
  set_tests_properties(
    ulog.composed-producer-kernel-stress PROPERTIES LABELS "benchmark;stress;concurrency" TIMEOUT
                                                    10
  )
endif()
