# Reservation ledger prototype

This experiment compares two private bounded-accounting candidates. It does not
select a production design or publish reservation types. The question is whether
producer-local credits improve producer tail latency enough to justify their
additional retained-memory state when compared with a single central byte pool.

## Shared contract

A successful reservation charges the complete worst-case writer size before a
message-building callback runs. The callback receives that reserved size and may
commit any valid retained size no larger than it. Commit transfers the retained
charge without reacquiring capacity; abandonment releases the open-writer
charge. Rejection returns before invoking the callback.

The accounting definitions are:

- **logical retained**: baseline occupancy plus open writer bytes and committed
  Record bytes;
- **physical retained**: every byte unavailable from the hard central bound,
  including unused producer-local credit;
- **allocation count**: heap allocation performed by the measured candidate
  path, reported separately from retained-byte accounting.

At every quiescent observation:

```text
logical retained <= physical retained <= configured payload capacity
```

Both candidates use move-only reservation and ownership tokens. Destroying an
open reservation abandons it; destroying committed ownership releases it.

## Candidates

`central-reservation` uses one atomic retained-byte counter. Admission is an
overflow-safe conditional compare-and-exchange. Logical and physical retained
bytes are identical because the candidate has no credit cache.

`producer-credit-reservation` backs fixed producer slots with one central
available-byte counter. Charges are rounded to a 64-byte accounting quantum.
A producer first consumes returned local credit and refills only the exact
deficit for its current reservation. Commit returns unused rounded tail credit;
abandonment and Record release return credit to the originating producer. A
quiescent explicit return moves cached credit back to the central pool.

Warm-up credits remain backed during the measured window. Consequently their
cost is visible in physical initial, high-water, and final values while logical
initial and final values remain the workload baseline. Cleanup runs only after
the final accounting snapshot and outside producer latency measurement.

Admission statistics use fixed cache-line-separated producer shards and are
aggregated after workers join. The first measured wave samples retained
high-water at the post-attempt barrier, after each producer latency timer stops
and before any ownership release. Every later wave repeats the same fixed
admission state; final physical retained is also included in the reported
physical high-water. This keeps shared observation counters out of the accepted
producer path. Both candidates use only fixed in-object state and contain no
allocation call site, so their allocation counters are structurally zero.

## Evidence rules

Both candidates run every cell of the shared producer-count, Record-size, and
occupancy matrix. Protocol `ulog-workload-results/4` requires both candidate
names, so a missing implementation or matrix cell fails validation. Exact
admission, accounting conservation, bounds, allocation failures, and callback
suppression are deterministic gates. Hosted timing remains advisory.

Candidates execute as adjacent pairs for each matrix cell, with the first
candidate alternating by repetition. This prevents a full candidate block from
receiving a systematically different machine phase than the other candidate;
the result validator enforces the paired-alternating schedule.

The randomized oracle applies reserve, partial/full commit, abandon, release,
credit refill, and credit return transitions and compares every resulting
snapshot with a simple sequential model. Hand-written boundary tests cover
zero, capacity, over-capacity, and integer-overflow requests, callback rejection,
builder failure, move-only ownership, and cross-thread credit return.

## Advisory controlled observations

Two independent paired protocol-v3 runs were collected on `WORKPC` with 16
logical CPUs, an MSVC Release build, and Google Benchmark 1.9.5. The validator
accepted all 1,680 rows in each run with no accounting, retained-bound,
allocation, or allocation-failure errors. Timing remains advisory, but the
direction below was consistent in both AB and BA order strata in all 20 primary
16/32-producer near-full and saturated cells.

The table reports the range across both runs of the median
`producer-credit-reservation` p99.9 divided by the median
`central-reservation` p99.9. Values below one favor producer credits.

| Producers and occupancy | Record bytes | Accepted per wave | Credit / central p99.9 |
| --- | ---: | ---: | ---: |
| 16, near-full | 64-1024 | 16 | 0.32-0.56 |
| 16, near-full | 4096 | 4 | 0.60-0.91 |
| 16, near-full | 16384 | 1 | 1.67-1.90 |
| 16, saturated | 64-16384 | 0 | 1.50-2.67 |
| 32, near-full | 64-256 | 32 | 0.84-0.88 |
| 32, near-full | 1024 | 16 | 0.95 |
| 32, near-full | 4096 | 4 | 1.06-1.16 |
| 32, near-full | 16384 | 1 | 2.38-4.22 |
| 32, saturated | 64-16384 | 0 | 2.31-4.63 |

Central reservation had zero physical-minus-logical retained overhead at every
observation. Producer credits also had zero overhead at logical high-water, but
cached between zero and 524,288 physical bytes above logical initial and final
occupancy. The performance evidence therefore shows a workload-dependent
crossover rather than a universal winner: cached credit reduces tail cost when
many producers can consume it, while refill/rejection bookkeeping and retained
idle credit dominate small or rejected waves. Final design selection remains a
separate architecture decision. [ADR 0017](adr/0017-use-producer-credits-contiguous-records-and-producer-lanes.md)
subsequently selects producer credits for the initial private kernel while
retaining this crossover as an explicit trade-off.

The raw result SHA-256 values are
`fda06db02777972d69d88f20c4dce353e38d079d13ed9ec1e5f8f0ecb25f0127`
and `c7057d36921fd013106a3c49e328c4e112746ad118d5674061b7395155dad2b9`.
Those historical runs did not retain external-timeout provenance; future
reproductions use the 1,800-second limit below rather than copying that omission.

Use a controlled result only for performance comparison:

```shell
timeout --signal=TERM --kill-after=30s 1800s \
  ulog-workload-benchmarks --ulog_mode=controlled --benchmark_color=false \
  --benchmark_out=controlled-results.json --benchmark_out_format=json
python scripts/benchmark_results.py validate controlled-results.json
```

The controlled body is forbidden in hosted CI. A timeout or partial JSON is
diagnostic only; validate and retain results only after a complete bounded run.

Compare the seven repetitions per cell, with particular attention to p99.9 for
16 and 32 producers under near-full and saturated occupancy. Keep raw JSON with
the source revision and machine details. This prototype evidence alone did not
select a design; ADR 0017 combines it with storage, ingress, and composed stress
evidence.
