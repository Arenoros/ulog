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

Smoke mode uses one warm-up round, one measured round, and one repetition. It
checks the complete matrix and protocol shape without producing timing evidence.
Controlled mode uses 64 warm-up rounds, at least 100,000 measured attempts per
matrix cell, and seven repetitions.

## Candidate adapter seam

`benchmarks/support/workload_harness.hpp` defines the private compile-time
`WorkloadKernel` contract and `RunWorkload`. An adapter supplies:

- a stable lowercase candidate name;
- preparation and measurement-reset operations;
- a post-attempt retained high-water observation;
- a post-snapshot quiescent cleanup operation;
- a non-throwing producer attempt and release pair;
- a non-throwing snapshot with admission, allocation, and retained-memory
  accounting.

Preparation, payload allocation, sample storage, and thread creation happen
outside the measured producer attempts. The template call seam avoids a virtual
or function-pointer dispatch in the hot path. Reservation, Record-storage, and
ingress prototypes can therefore reuse the harness without adding a production
header or installed interface. `ReferenceLedgerKernel` is the deterministic
adapter used by harness unit tests. The reservation experiment registers the
`central-reservation` and `producer-credit-reservation` candidates; neither is
installed or exposed as Ulog API.

## Result protocol

The executable emits Google Benchmark JSON with these context fields:

- `ulog_result_protocol`: `ulog-workload-results/4`;
- `ulog_candidates`: `central-reservation,producer-credit-reservation`;
- `ulog_candidate_schedule`: `paired-alternating`;
- `ulog_mode`: `smoke` or `controlled`;
- `ulog_timing_policy`: `advisory`;
- `ulog_repetitions`: the number of workload repetitions.

The candidate inventory is canonical and mandatory, so omitting an entire
implementation cannot accidentally produce a valid result. Each workload row is named
`UlogWorkload/<candidate>/producers:<count>/record_bytes:<bytes>/occupancy:<state>/repetition:<index>`.
Google Benchmark may append `/iterations:1/manual_time`.

Both candidates for one matrix cell execute next to each other. The central
candidate runs first on even repetitions and the producer-credit candidate runs
first on odd repetitions. The validator rejects any other row schedule. Pairing
limits long-lived machine drift, while alternating which candidate starts avoids
giving either implementation a permanent first-run advantage.

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

`scripts/benchmark_results.py` strictly validates the protocol, exact candidate
inventory, paired-alternating schedule, complete matrix, finite values, exact
admission arithmetic, rates, allocation failures, and retained bounds. Physical
retained values must cover their corresponding logical values.
`scripts/collect_benchmark_results.py` validates the newest Conan result before
publishing it as a CI artifact.

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

## Structured Record storage experiment

The private Structured Record experiment reuses this matrix and measurement
window for three physically distinct storage candidates. It emits the sibling
`ulog-record-storage-results/2` protocol, keeps same-cell candidate rows adjacent
in a six-permutation cycle, and reports logical Record footprint separately from
candidate fragmentation and physical byte-budget charge. Its CI smoke result is
`record-storage-results.json` and is validated independently:

```shell
python scripts/record_storage_results.py validate record-storage-results.json
```

See [Structured Record storage prototype](record-storage-prototype.md) for the
ownership and immutability contract, exact candidate organizations and charge
formulas, fmt/native write paths, UTF-8 truncation, controlled reproduction, and
interpretation limits. The implementations and result protocol are benchmark
infrastructure, not a selected production layout or public API.

## Ingress topology experiment

The private ingress experiment reuses the same matrix with fully owned prototype
Records. It compares bounded MPSC ring, chunked MPSC, and per-producer-lane
handoff, emits `ulog-ingress-results/1`, and publishes topology accounting,
FIFO/sequence validation, and bounded producer-action counters. Its CI smoke
artifact is `ingress-results.json`:

```shell
python scripts/ingress_results.py validate ingress-results.json
```

See [Ingress topology prototype](ingress-topology-prototype.md) for the common
publication seam, candidate linearization points and action bounds, randomized
model and saturated stress checks, ThreadSanitizer path, controlled reproduction,
and interpretation limits. The experiment does not select or expose a production
queue.

## Composed producer kernel

The private composed-producer experiment connects the selected producer-credit
ledger, contiguous Record slot, and per-producer lanes behind one lazy producer
path. Its CI smoke artifact is `composed-producer-results.json` and is validated
independently:

```shell
python scripts/composed_producer_results.py validate composed-producer-results.json
```

See [Composed producer kernel](composed-producer-kernel.md) for callback ordering,
publication and consumption linearization points, ownership lifetime, accounting,
stress coverage, and the bounded CI/controlled-run policy. The composition is
selected in
[ADR 0017](adr/0017-use-producer-credits-contiguous-records-and-producer-lanes.md),
but this executable remains benchmark infrastructure rather than public logging
API.

## Controlled mode

Use a dedicated, otherwise idle machine with a stable power policy and thermal
state. Pin the source revision, compiler, build type, dependency lockfile, and
relevant operating-system configuration. Record that environment beside the
result and compare like-for-like runs only.

Run the Release executable directly so it selects the longer workload:

```shell
timeout --signal=TERM --kill-after=30s 1800s \
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
`docs/performance-contract.md` remain authoritative. On Windows, run through an
equivalent external supervisor with the same TERM/KILL semantics and wall-clock
limit; the benchmark binary is not its own timeout guard.

Every future controlled execution must have an external wall-clock limit chosen
from a short pilot and recorded with the result. Historical evidence that did
not retain timeout provenance must say so explicitly. The composed producer
matrix uses a 1,200-second hard limit and normally finishes in under eight
minutes on `dockervm`. A timed-out or partial JSON file is diagnostic only and
must never be merged, published, or treated as evidence. Full controlled bodies
are forbidden in GitHub-hosted workflows.
