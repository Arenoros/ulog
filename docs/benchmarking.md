# Performance workload harness

The workload harness gives performance-sensitive implementation candidates one
shared workload, measurement window, result schema, and deterministic validator.
It is benchmark infrastructure, not a Ulog public API: all adapters and support
types remain below `benchmarks/` and are not installed.

## Workload matrix

Every candidate runs the Cartesian product below against a fixed 1 MiB payload
capacity:

| Dimension | Values |
| --- | --- |
| Producers | 1, 2, 4, 8, 16, 32 |
| Record size | 64, 256, 1024, 4096, 16384 bytes |
| Initial occupancy | empty (0), partial (1/2), near-full (63/64), saturated (full) |

One round starts all producers together. Each producer attempts one Record, all
attempts finish before accepted reservations are released, and the next round
then starts. Consequently, deterministic admission for each round is:

```text
accepted = min(producers, floor((capacity - initial occupancy) / record size))
```

Smoke mode uses 8 warm-up rounds, 64 measured rounds, and one repetition. It is
short enough for every supported CI platform. Controlled mode uses 64 warm-up
rounds, at least 100,000 measured attempts per matrix cell, and seven
repetitions.

## Candidate adapter seam

`benchmarks/support/workload_harness.hpp` defines the private compile-time
`WorkloadKernel` contract and `RunWorkload`. An adapter supplies:

- a stable lowercase candidate name;
- preparation and measurement-reset operations;
- a non-throwing producer attempt and release pair;
- a non-throwing snapshot with admission, allocation, and retained-memory
  accounting.

Preparation, payload allocation, sample storage, and thread creation happen
outside the measured producer attempts. The template call seam avoids a virtual
or function-pointer dispatch in the hot path. Reservation, Record-storage, and
ingress prototypes can therefore reuse the harness without adding a production
header or installed interface. `ReferenceLedgerKernel` is the deterministic
reference adapter and an example for later candidates.

## Result protocol

The executable emits Google Benchmark JSON with these context fields:

- `ulog_result_protocol`: `ulog-workload-results/1`;
- `ulog_mode`: `smoke` or `controlled`;
- `ulog_timing_policy`: `advisory`;
- `ulog_repetitions`: the number of workload repetitions.

Each workload row is named
`UlogWorkload/<candidate>/producers:<count>/record_bytes:<bytes>/occupancy:<state>/repetition:<index>`.
Google Benchmark may append `/iterations:1/manual_time`.

The custom counters include:

| Category | Counters and units |
| --- | --- |
| Producer latency | `producer_latency_p50_ns`, `producer_latency_p99_ns`, `producer_latency_p999_ns` (p99.9), in nanoseconds |
| Throughput | `attempts_per_second`, accepted `records_per_second`, accepted `bytes_per_second` |
| CPU | `process_cpu_time_ns`; `cpu_utilization_percent` is process CPU time divided by wall time |
| Allocation | `allocation_count`, `allocation_failure_count` |
| Admission | attempted, accepted, and rejected Record and byte counts |
| Retained memory | logical and physical initial, high-water, final, and limit byte counts |
| Validation | `accounting_error_count`, `retained_bound_error_count` |
| Workload identity | producer count, Record size, workload repetition, warm-up rounds, measured rounds, and sample count |

Process CPU utilization can exceed 100% when producer threads run concurrently.
Some operating systems expose process CPU clocks at a coarser resolution than
the smoke workload; those timing fields remain valid advisory observations.

`scripts/benchmark_results.py` strictly validates the protocol, complete matrix,
finite values, exact admission arithmetic, rates, allocation failures, and
retained bounds. `scripts/collect_benchmark_results.py` validates the newest
Conan result before publishing it as a CI artifact.

## Smoke mode

Configure a Release build with `ULOG_BUILD_BENCHMARKS=ON` and Google Benchmark
1.9 available. The Conan workflow in `docs/testing.md` supplies that dependency.
Then build and run the benchmark CTest label:

```shell
cmake --build <build-dir> --config Release
ctest --test-dir <build-dir> -C Release -L benchmark --output-on-failure
```

The tests run the reference workload, write `benchmark-results.json`, and
validate it. Hosted-runner latency, throughput, and CPU values are advisory.
Missing matrix cells, malformed or inconsistent accounting, allocation
failures, and retained-bound violations fail CI.

## Controlled mode

Use a dedicated, otherwise idle machine with a stable power policy and thermal
state. Pin the source revision, compiler, build type, dependency lockfile, and
relevant operating-system configuration. Record that environment beside the
result and compare like-for-like runs only.

Run the Release executable directly so it selects the longer workload:

```shell
<build-dir>/bin/ulog-workload-benchmarks --ulog_mode=controlled \
  --benchmark_color=false \
  --benchmark_out=controlled-results.json \
  --benchmark_out_format=json
python scripts/benchmark_results.py validate controlled-results.json
```

On a multi-config Windows build, the executable is normally
`<build-dir>/bin/Release/ulog-workload-benchmarks.exe`. Archive the raw JSON, use
all seven repetitions for comparisons, and confirm a suspected regression with
a fresh rerun before applying a hard latency or throughput gate. The rules in
`docs/performance-contract.md` remain authoritative.
