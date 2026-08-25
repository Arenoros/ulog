# Ingress topology prototype

This private benchmark prototype compares three bounded ways to transfer fully
owned, immutable Records from producers to one consumer. It supplies evidence
for a later ingress design; it does not install a production queue type, define
route fan-out, or create public Ulog API.

The prototype follows [ADR 0004](adr/0004-reimplement-logging-as-a-pipeline.md):
the producer completes Record construction and context capture before
publication. The ingress envelope contains a private Record storage handle and
an admission sequence; adding that sequence does not change the Record layout.

## Common publication seam

All candidates implement the private seam in
`benchmarks/prototypes/ingress/ingress_topology.hpp`:

- `TryPublish(producer_index, handle)` returns accepted, full, contended, or
  invalid, an admission sequence only for accepted Records, and the number of
  bounded topology actions performed;
- `TryConsume()` returns empty, pending publication, or the next Record; and
- `GetSnapshot()` reports exact enqueue, dequeue, rejection-category, retained
  Record, serialized-byte, and physical-charge accounting at quiescent points.

The Record is already owned and immutable when `TryPublish` starts. Publication
does not scan routes, encode output, allocate storage, perform I/O, or block.
`publication_actions` counts bounded accesses to ingress control and accounting
state. It deliberately excludes the already completed Record construction and
the fixed envelope copy, which are common to every candidate.

Every accepted Record receives one monotonically increasing admission sequence
at the candidate's documented linearization point. A rejected attempt never
consumes a sequence. The single consumer emits only the next sequence, so a
reserved but not yet released earlier publication is reported as pending rather
than bypassed.

## Candidates and bounds

The maintained prototype capacity is 64 Records and the maintained workload has
at most 32 producers.

| Candidate | Organization | Admission linearization point | Maximum producer publication actions |
| --- | --- | --- | ---: |
| `bounded-mpsc-ring` | one sequence-tagged fixed MPSC ring | successful global tail compare-and-swap | 70 |
| `chunked-mpsc` | eight independent shared chunks; each producer maps to one eight-cell chunk | global admission-sequence fetch-add after guarded local reservation | 11 |
| `per-producer-lanes` | fixed capacity partitioned among active producer-local lanes | global admission-sequence fetch-add after the lane capacity check | 11 |

Ring publication performs two fixed actions, inspects at most 32 sequence tags
with at most two actions per probe, and performs four infallible publication
actions after reservation. Exhausting the probe budget returns contended.

Chunked publication neither scans nor waits. It tries the guard for its mapped
chunk, checks and reserves that chunk's next physical cell, assigns the global
sequence, publishes, and releases the guard in at most 11 actions. Holding the
guard through readiness makes physical head order within a chunk agree with
global admission order. A busy guard returns contended, and a full mapped chunk
returns full even when another chunk has space. The single consumer scans at
most eight physical chunk heads and merges them by the next global sequence.

The producer-lane path uses its assigned lane only; it does not scan other lanes
or retry. Its single consumer may inspect at most the 32 active lanes to find the
next admission sequence.

No fallible operation follows an accepted candidate's linearization point.
Producer accounting is cache-line sharded by producer; accepted publications do
not increment a shared statistics counter. Dequeue accounting is written only
by the consumer and is updated before a cell or lane slot becomes reusable.
`GetSnapshot()` is race-free but weak during concurrent mutation: it derives
retained values with saturating subtraction from independently sampled
cumulative counters. Snapshot values are exact at the quiescent barriers used by
the model, benchmark, and final stress checks.

## Ownership, ordering, and accounting checks

`IngressKernel` builds the canonical contiguous prototype Record in one
producer-owned slot, then publishes only its handle. The last producer to reach
the round release barrier drains the topology and verifies the handle
generation, Record footprint, FIFO order, and admission sequence before
releasing storage. Warm-up sequences remain monotonic; measurement counters are
reported as a delta from the warm-up baseline.

Unit coverage runs every candidate through the same workload adapter and the
same deterministic randomized FIFO model. The model checks every result and
snapshot against a simple eight-entry global FIFO over 2,048 operations while
respecting the chunk and lane capacity partitions. Candidate tests additionally
cover full rejection without sequence consumption, wrap, lane partitioning,
chunk-local fullness with another chunk free, guarded chunk contention, invalid
input, and exact retained-byte accounting.

The concurrent stress executable first preloads 63 of 64 entries in a pattern
that leaves exactly one usable ring cell, one cell in the target shared chunk,
or one producer-lane cell available. Thirty-two simultaneous publishers must
admit exactly that final slot and reject the other 31 attempts before an exact
FIFO drain. A second phase gates the
consumer while 32 producers issue an initial 256 attempts, proving that every
candidate reaches saturation and rejects excess work. It then runs 131,072 fixed
producer calls per candidate with a single consumer. The test verifies:

- every accepted handle is consumed exactly once with unchanged metadata;
- consumed envelopes follow strict admission-sequence order;
- producer action counts remain within the documented bound;
- final enqueue, dequeue, rejection-category, and retained accounting is exact;
- all fixed producer calls finish and the topology drains; and
- the cooperative five-second deadline and the ten-second CTest timeout prevent
  an unbounded CI run.

## Benchmark protocol

`ulog-ingress-topology-benchmarks` emits Google Benchmark JSON under
`ulog-ingress-results/1`. Its candidates are
`bounded-mpsc-ring,chunked-mpsc,per-producer-lanes`, its timing policy is
advisory, and adjacent same-cell rows use a six-permutation cycle.

Each row has this stable identity:

```text
UlogIngressTopology/<candidate>/producers:<count>/record_bytes:<bytes>/occupancy:<state>/repetition:<index>
```

The benchmark reuses the complete 1 MiB workload matrix: 1, 2, 4, 8, 16, or 32
producers; requested messages of 64, 256, 1,024, 4,096, or 16,384 bytes; and
empty, partial, near-full, or saturated initial byte-budget occupancy. Smoke
mode uses one warm-up round, one measured round, and one repetition, producing
360 rows. Controlled mode uses 64 warm-up rounds, at least 100,000 measured
attempts per cell, and seven repetitions, producing 2,520 rows.

The byte budget is an admission upper bound, not a promise that every candidate
accepts all capacity-eligible attempts. A bounded topology may also return full
or contended. The protocol therefore records actual accepted and rejected
samples plus `maximum_accepted_per_round`; retained high-water accounting is
checked against that observed maximum. Rejection categories and acceptance rate
are part of the candidate trade-off rather than hidden benchmark errors.

Producer latency includes the identical canonical Record build and the selected
topology publication. In addition to common workload counters, every row reports
the topology snapshot, FIFO/sequence/Record validation errors, maximum observed
publication actions, and the candidate's action limit.
`scripts/ingress_results.py` rejects an incomplete inventory or matrix, a wrong
schedule, inconsistent workload or topology accounting, retained state after
drain, validation errors, or an action-bound violation.

## Controlled evidence and prototype recommendation

One controlled run was collected for exact revision
`f3c7e017f71546c6e41ed6d99b89908fcd10d15c` on `dockervm`: a Debian VM with two
11th-generation Intel Core i7-11800H vCPUs, 4 GiB assigned to the benchmark
container, GCC 12.2, and Google Benchmark 1.9.5. Starting load average was
0.02/0.11/0.08. The run completed in 35 minutes 22 seconds under an external
3,600-second deadline. The validator accepted all 2,520 rows and 252,000,000
measured attempts with zero accounting, retained-bound, allocation-failure,
FIFO, sequence, or Record-validation errors. The raw JSON SHA-256 is
`913021da09bb280117e808446ab86785efe08cbbb7ccacc2b74a53ab48c377f0`.
The exact-revision
[GitHub Actions run](https://github.com/Arenoros/ulog/actions/runs/32883565599)
completed all five jobs in 3 minutes 47 seconds. Its Clang 18 TSan job ran and
passed 22 of 22 tests, including every ingress unit, schedule, smoke, validator,
and stress check.

Saturated cells remain part of the correctness evidence but are excluded from
the handoff comparison because the common byte budget rejects them before the
topology is called. Repetitions 0-5 form 540 balanced paired active cells;
repetition 6 supplies 90 holdout cells. Effects below are median paired ratios
relative to producer lanes. Positive p99 means slower, while positive delivered
Records/s means faster.

| Candidate | Observed / published actions | Balanced topology acceptance | Balanced p99 | Balanced Records/s | Holdout p99 | Holdout Records/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `bounded-mpsc-ring` | 12 / 70 | 100% | +0.98% | +0.37% | +0.93% | -0.93% |
| `chunked-mpsc` | 11 / 11 | 99.8876% | +0.34% | -0.22% | +0.45% | -0.62% |
| `per-producer-lanes` | 11 / 11 | 100% | baseline | baseline | baseline | baseline |

The ring and lanes accepted all 49,425,000 balanced topology attempts. Chunked
admitted 49,377,473 of 49,433,054 attempts and rejected the other 55,581 on its
mapped-chunk guard. Those rejections appeared only at 16 and 32 producers, at
0.3650% and 0.3647% respectively; the holdout repeated the deficit. Lanes had
the lower p99 in 319 of 540 balanced comparisons against the ring and 283 of 540
against chunked, but only 51 of 90 and 47 of 90 holdout comparisons. Delivered
throughput differences were small and changed direction for the ring in the
holdout. Position-normalized timing also drifted by about one percent. With
advisory timing on two vCPUs and no independent confirmation run, this evidence
does not establish a latency or throughput winner.

Within this prototype's maintained workload, `per-producer-lanes` is therefore
the best bounded producer handoff: it combines the minimum published producer
bound with lossless topology admission. That recommendation rests on the
deterministic bound and rejection behavior, not on noisy timing. It is not the
final production topology. Lanes require a stable producer identity, partition
capacity, and move selection work to the single consumer. The ring remains the
stronger fallback when shared spare capacity matters more than its larger
producer bound. Chunk mapping did not produce a durable timing advantage that
offsets its high-concurrency contention rejections.

## Reproduction and CI limits

Configure a Release build with `ULOG_BUILD_BENCHMARKS=ON` and
`ULOG_BUILD_STRESS_TESTS=ON`, then run the short acceptance path:

```shell
cmake --build <build-dir> --config Release
ctest --test-dir <build-dir> -C Release -R ulog.ingress-topology --output-on-failure
python scripts/ingress_results.py validate <build-dir>/ingress-results.json
```

The Linux `linux-tsan` GitHub Actions job builds the ingress matrix and stress
path with Clang ThreadSanitizer. It is separate from ASan/UBSan, has a 20-minute
job limit and a 12-minute build/test-step limit. Benchmark smoke tests have a
120-second CTest limit, while the stress test has a ten-second limit. These are
failure guards, not expected runtimes; the smoke matrix and stress path are
intended to complete in seconds.

The allocation-interposition record-storage unit test is excluded only from the
TSan build because it replaces the process-wide `new`/`delete` symbols that the
TSan runtime also defines. Ordinary and ASan/UBSan jobs retain that test. All
ingress unit, model, smoke, and stress paths remain enabled under TSan.

Before collecting controlled timing evidence, validate the complete registration
schedule and run on a dedicated idle machine:

```shell
python scripts/check_ingress_schedule.py <build-dir>/bin/ulog-ingress-topology-benchmarks
timeout --signal=TERM --kill-after=30s 3600s \
  <build-dir>/bin/ulog-ingress-topology-benchmarks --ulog_mode=controlled \
  --benchmark_color=false \
  --benchmark_out=controlled-ingress-results.json \
  --benchmark_out_format=json
python scripts/ingress_results.py validate controlled-ingress-results.json
```

Archive the JSON with the exact revision, compiler, dependency lockfile,
operating system, and machine configuration. Hosted-runner timing remains
advisory. Correctness, bounded work, protocol validation, and sanitizer results
are acceptance evidence; performance measurements compare trade-offs but do not
choose the production topology.
