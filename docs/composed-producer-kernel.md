# Composed producer kernel prototype

This private prototype validates one complete bounded path before production
interfaces depend on it. It composes producer-local byte credits, contiguous
owned Record storage, per-producer ingress lanes, publication ordering, and a
single consuming thread. It does not define `Logger`, `Runtime`, encoder, sink,
route, or I/O APIs.

## Composition and admission order

The maintained path combines:

- `ProducerCreditLedger`, with a 64-byte physical accounting quantum;
- `ContiguousRecordSlot`, whose published charge shrinks from the worst-case
  reservation to the actual immutable Record size; and
- `PerProducerLanes<64>`, partitioned among at most 32 stable producer indices.

`TryProduce` performs these transitions in order:

```text
claim one producer-lane cell
-> reserve the worst-case serialized byte budget
-> invoke the context callback
-> invoke the message callback
-> publish one immutable Record and transfer byte ownership
-> assign the admission sequence
```

Lane-full, contended, invalid-producer, and byte-budget rejection therefore
happen before either caller callback. A successful lane claim is invisible to
topology counters and consumes no sequence until `Publish`; abandoning it after
a byte-budget rejection only releases the lane guard. Once callbacks begin,
the fixed Record slot, physical charge, and lane cell are already held, so the
remaining publication path is infallible for a valid internal Record.

The prototype requires one stable exclusive producer thread per producer
index. This is the same ownership rule as the producer-credit ledger and avoids
shared mutation of producer-local credit or lane state.

The prototype owns one fixed `ContiguousRecordSlot` backing block per ingress
cell, allocated once when the path is prepared. `ComposedProducerPath<64>`
therefore reserves 1,052,672 backing bytes independently of the configured
retained-payload budget. That storage is reusable topology capacity, not live
Record ownership, so logical/physical retained counters intentionally exclude
it. Allocation verification asserts the exact one-block constructor cost and
zero general-purpose allocations on all warm producer/consumer paths.

## Consumer ownership boundary

The topology now exposes a held consumption claim in addition to its original
`TryConsume` compatibility wrapper. The composed consumer performs:

```text
claim the next admission sequence
-> inspect the read-only RecordView in the consumer callback
-> release ProducerCreditLedger ownership to its producer mailbox
-> reset the Record slot
-> acknowledge and make the lane cell reusable
```

The lane read position does not advance while the callback is running. A
producer can use another free cell in its lane, but it cannot wrap onto and
overwrite the claimed Record. The consumer merges lane heads strictly by the
sequence assigned at publication; an open writer has no sequence and is not
reported as a pending earlier publication.

The common workload adapter owns a dedicated consumer thread. Its artificial
round release gate holds accepted Records until the workload harness samples
high-water, then asks that thread to drain and waits for completion. The core
`ComposedProducerPath` itself has no production lifecycle or public thread API.

## Accounting invariants

At quiescent observations:

```text
attempted Records = accepted Records + rejected Records
published Records = consumed Records + ingress-retained Records
logical retained <= physical retained <= configured byte limit
```

Logical retained is the baseline plus actual serialized bytes held by open or
published ownership. Physical retained also includes rounded producer credit,
including credit returned by the consumer but not yet reclaimed by its
producer. `ReturnAllCredits` is a quiescent cleanup operation; it is not part of
producer latency.

The stress begins with a gated wave that fills all 64 lane cells before the
consumer starts. With its fixed-size Record this realizes both structural
ceilings: `64 * serialized_bytes` logically and `64 * accounting_charge_bytes`
physically. The following randomized phase cannot exceed the first ceiling
because no more than 64 lane cells can own Records, or the second because the
ledger never transfers more credit than its configured capacity. The test
checks the realized ceiling, ledger conservation, every accepted Record, and
complete final drain. This proves the run's retained-memory bound without a
shared statistics counter on the producer path.

The common benchmark uses exact-capacity admission because every workload wave
has at most one attempt per producer and therefore cannot hit the two-cell
minimum lane partition before the byte budget. Budget-rejected successful lane
claims remain intentionally absent from topology attempt counters; topology
attempts describe records that reached ingress publication or were rejected by
ingress itself.

## Deterministic verification

The maintained verification covers:

- byte-budget and lane-full rejection without caller evaluation or sequence
  consumption;
- caller-storage mutation after publication, typed context fields, immutable
  Record contents, and FIFO consumption;
- a reserved writer blocked inside its callback while another producer
  publishes sequence zero;
- a consumer blocked inside its callback while the producer is prevented from
  wrapping onto that Record slot;
- 1,024 warm allocation-interposed produce/consume cycles plus both rejection
  paths with zero general-purpose heap allocations;
- the shared `RunWorkload` contract and the full smoke matrix; and
- a 32-producer randomized saturation stress with exact callback, sequence,
  ownership, retained-byte, and topology accounting.

The unit test has a 20-second CTest limit, allocation verification 10 seconds,
and stress 10 seconds plus its own shorter hard watchdog. Benchmark listing and
result validation have 30-second limits. GitHub Actions measures only
`--ulog_mode=smoke`. A list-only check constructs the controlled registry under
its 30-second limit to verify all 840 names, but Google Benchmark executes none
of those workload bodies. Full controlled execution is reserved for an
explicitly bounded `dockervm` run.

## Benchmark protocol

`ulog-composed-producer-benchmarks` emits protocol
`ulog-composed-producer-results/1` with candidate `composed-producer` and the
same producer-count, message-size, occupancy, and repetition dimensions as the
common workload harness. Timing is advisory. Deterministic validation requires
exact admission, callback, publish/consume, FIFO, allocation, and retained-bound
counters before any timing comparison is considered.

Run the bounded smoke protocol locally with:

```shell
ulog-composed-producer-benchmarks --ulog_mode=smoke --benchmark_min_time=0.001s \
  --benchmark_out=composed-producer-results.json --benchmark_out_format=json
python scripts/composed_producer_results.py validate composed-producer-results.json
```

The complete controlled mode must not be added to hosted CI. When evidence is
needed for the following producer-kernel decision, run the exact committed
revision on the controlled VM under an external 3,600-second hard timeout and
retain the JSON, hash, toolchain, machine, and elapsed-time provenance.
