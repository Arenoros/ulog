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

add_executable(ulog-workload-harness-test workload_harness_test.cpp)
target_compile_features(ulog-workload-harness-test PRIVATE cxx_std_20)
target_link_libraries(ulog-workload-harness-test PRIVATE ulog-workload-support)
ulog_configure_target(ulog-workload-harness-test)

add_executable(ulog-reservation-ledger-test reservation_ledger_test.cpp)
target_compile_features(ulog-reservation-ledger-test PRIVATE cxx_std_20)
target_link_libraries(ulog-reservation-ledger-test PRIVATE ulog-workload-support)
ulog_configure_target(ulog-reservation-ledger-test)

add_executable(ulog-record-storage-test record_storage_test.cpp)
target_compile_features(ulog-record-storage-test PRIVATE cxx_std_20)
target_link_libraries(ulog-record-storage-test PRIVATE ulog-record-storage-support)
ulog_configure_target(ulog-record-storage-test)

add_test(NAME ulog.workload-harness-unit COMMAND ulog-workload-harness-test)
add_test(NAME ulog.reservation-ledger-unit COMMAND ulog-reservation-ledger-test)
add_test(NAME ulog.record-storage-unit COMMAND ulog-record-storage-test)
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
set_tests_properties(ulog.workload-harness-unit PROPERTIES LABELS "benchmark;unit")
set_tests_properties(ulog.reservation-ledger-unit PROPERTIES LABELS "benchmark;unit")
set_tests_properties(ulog.record-storage-unit PROPERTIES LABELS "benchmark;unit")
set_tests_properties(
  ulog.benchmark-schedule PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" LABELS
                                     "benchmark;tooling"
)
set_tests_properties(ulog.version-benchmark-smoke PROPERTIES LABELS "benchmark")
set_tests_properties(ulog.benchmark-smoke PROPERTIES LABELS "benchmark")
set_tests_properties(
  ulog.benchmark-results PROPERTIES DEPENDS ulog.benchmark-smoke ENVIRONMENT
                                    "PYTHONDONTWRITEBYTECODE=1" LABELS "benchmark;tooling"
)
set_tests_properties(
  ulog.record-storage-schedule PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" LABELS
                                          "benchmark;tooling"
)
set_tests_properties(ulog.record-storage-benchmark-smoke PROPERTIES LABELS "benchmark")
set_tests_properties(
  ulog.record-storage-benchmark-results
  PROPERTIES DEPENDS ulog.record-storage-benchmark-smoke ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
             LABELS "benchmark;tooling"
)
