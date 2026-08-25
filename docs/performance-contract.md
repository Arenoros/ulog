# Performance Contract

Performance is a release criterion for Ulog. This contract separates
deterministic invariants from measurements that depend on hardware, operating
system, compiler, adapters, and workload.

## Priorities

1. Minimize producer-side tail latency, including p99 and p99.9 behavior.
2. Maximize sustained record and byte throughput without violating memory or
   ordering invariants.

## Initial producer kernel profile

The first production producer kernel follows
[ADR 0017](adr/0017-use-producer-credits-contiguous-records-and-producer-lanes.md).
Its configurable initial profile uses a 64-byte accounting quantum and minimum
Record charge, a 16,384-byte maximum serialized Record, 32 producer slots, and
64 ingress cells. The 1 MiB prototype workload capacity is not a production
pipeline-budget default.

One active application producer exclusively owns a registered producer slot.
Slot exhaustion is a drop-newest rejection before caller evaluation. A slot is
not reused until its writer is closed, its lane is drained, and its credit is
reconciled; stale producer-local registrations cannot address a reused slot.
The default two cells per slot allow one cell to remain held by the consumer
while its producer uses the other.

An admission attempt claims its producer-lane cell and reserves the complete
worst-case Record charge before context or message callbacks. Open-writer
logical accounting uses that worst-case serialized size. Physical accounting
uses `max(64, round_up(serialized_size, 64))` and includes cached or returned
producer credit. Commit shrinks logical ownership to the actual immutable
Record and assigns its actual rounded charge; unused physical credit remains
accounted until reclamation. Lane, slot, or byte exhaustion invokes no callback
and consumes no admission sequence.

The admission sequence is assigned by the successful global fetch inside
publication. That operation is the linearization point and transfers Record
and charge ownership to the pipeline; no fallible work follows it. The consumer
holds its read-only Record claim until ownership is released and private storage
is reset, then acknowledges the lane cell.

Fixed preallocated Record backing is not live retained payload and is reported
separately from logical and physical retained counters. Runtime configuration
must nevertheless declare a maximum that includes this backing, producer state,
all payload pools, and the independent control and progress reserves.

## Deterministic hot-path invariants

- Compile-time-erased, runtime-filtered, and admission-rejected records do not
  evaluate the message expression.
- After warm-up, Ulog makes no general-purpose heap allocation for disabled
  records or ordinary accepted records that fit the documented native
  RecordWriter limits.
- Producer threads do not call Encoder, Sink, libuv, filesystem, or network
  operations.
- Publishing an accepted Record does not scan routes and performs one bounded
  handoff to the pipeline.
- A default-target macro performs one atomic non-owning Logger pointer load and
  no ownership or reclamation operation. Every published default target remains
  address-stable for the application lifetime.
- A default Logger exchange performs no quiescence wait. Producers that already
  loaded the old pointer finish against the old, still-live Logger without a
  forwarding check on ordinary calls.
- Startup capture is opt-in through a separately constructed byte-bounded
  BootstrapLogger. Only that special Logger performs a forwarding check during
  handoff; ordinary Runtime Logger calls retain the no-forwarding contract.
- Runtime construction validates configuration, reserves global pools, and
  starts the configured fixed workers before logging begins; producer-local
  storage may be initialized during warm-up.
- Ulog-owned payload memory is bounded by a configurable pipeline byte limit.
  It covers open RecordWriters, ingress Records, encoded batches, retry queues,
  and Ulog-owned in-flight buffers. Control operations use a separate bounded
  reserve.
- The configured non-blocking shedding algorithm is drop-newest, ingress-only
  drop-oldest, or deterministic occupancy-adaptive sampling.
- Drop-newest is the default. Priority-aware drop-oldest evicts only equal- or
  lower-priority eligible Records, and adaptive sampling uses stateless hashing
  rather than dynamic per-site state.
- The initial producer kernel does not steal credit or cells from another
  producer on the calling thread. A hot producer may therefore reject while
  another bounded producer slot has idle capacity; this remains an explicit
  accounted trade-off rather than hidden blocking or scanning.
- Error and Critical Records have a configurable reserved byte budget and are
  shed last without introducing synchronous producer-side I/O.
- Explicit `Block(deadline)` is supported separately, is never the default, and
  is forbidden on Ulog-owned threads.
- A Record exceeding `max_record_bytes` is truncated at a valid UTF-8 boundary,
  marked with `ulog.truncated=true`, and accounted separately. It never expands
  storage beyond the configured bound.
- A global hard budget is combined with route budgets and separately bounded
  priority and control reserves.
- Required Routes apply pressure to the pipeline. Best-Effort Routes may shed
  their own output so a slow destination cannot stall independent routes.
- Route batching is bounded by configurable record count, encoded bytes, and
  maximum delay. Configured high-severity levels wake the pipeline immediately.
- Producer payload credits never consume the independently bounded control or
  progress reserves. Future drop and routing policies may extend private
  ingress metadata without exposing the private Record layout or adding route
  work to the producer path.

The zero-allocation guarantee covers Ulog's native scalar, string, and fmt
paths. It cannot cover allocations performed while evaluating caller values,
custom fmt formatters, custom ContextProvider implementations, or other custom
adapters.

Ulog does not emit its own exceptions from the native logging hot path. Runtime
construction and ordered control operations report failures through explicit
results; exceptions thrown while evaluating caller expressions remain outside
Ulog's guarantee.

Context capture occurs only after admission capacity has been reserved. Encoder
and Sink extension code never executes on producer threads, and Encoder never
executes on the libuv loop thread.

## Statistics overhead

- Statistics have fixed cardinality by Runtime, Logger, route, level, and a
  closed set of outcome reasons.
- The ordinary accepted producer path performs no shared statistics-counter
  increment, lock, or allocation. Dispatcher and I/O owners account accepted,
  encoded, delivered, retry, and failure outcomes from existing Record metadata.
- Producer-only exceptional outcomes such as admission rejection and truncation
  use isolated pre-registered shards; no dynamically keyed per-site table is
  created.
- Snapshot aggregation may be weakly consistent while producers run. Ordered
  operation reports provide exact accounting for admitted Records through their
  barrier watermark. Records rejected before admission have no sequence in that
  watermark and appear only in weakly consistent sharded statistics.
- Allocation and latency benchmarks enforce that enabling built-in statistics
  does not introduce shared-cacheline contention on the ordinary producer path.

## Ordered-operation overhead

- Drain, Durable Flush, Reopen, and Shutdown enqueue preallocated control nodes
  from a reserve that remains available when the payload budget is exhausted.
- Their Operation completion state is independent of Record admission and does
  not add locks, reference counting, or shared-counter traffic to `LOG*` calls.
- Callback completion does not block an operating-system thread. `WaitUntil`
  blocks only when a caller explicitly selects the waiting interface.

## Ordering

- Each route and sink observes its selected Records in FIFO admission order.
- Flush and Reopen provide explicit ordered barriers.
- Completion order between independent routes is not guaranteed, allowing the
  implementation to execute routes concurrently.

## Validation

- Required cross-platform CI checks allocation ceilings, memory bounds,
  ordering, barriers, and exact shedding accounting deterministically.
- Benchmark executables are smoke-tested on every supported platform.
- Timing comparisons on shared hosted runners are advisory.
- Hard latency and throughput regression gates require stable controlled
  hardware, repeated measurements, and a confirmation rerun.
- The pinned userver extraction baseline is diagnostic evidence rather than an
  installed, build-time, or test dependency of Ulog. Ulog owns its performance
  SLOs rather than promising to outperform userver in every comparable case.
- A stream frame whose partial delivery is ambiguous is counted and dropped,
  never replayed. Reconnect and circuit-breaker recovery apply only to later
  Records and remain bounded by route budgets.
