# Use producer credits, contiguous Records, and producer lanes

Ulog's first production producer kernel is one private module composed from a
producer-credit byte ledger backed by a central hard bound, contiguous owned
Structured Record storage, and fixed per-producer ingress lanes. This
composition prioritizes bounded producer tail work, exact byte accounting,
low fragmentation, and a lossless publication handoff under the maintained
workload. Ledger tokens, Record layout, lane cells, and publication envelopes
remain implementation details rather than public API or ABI.

## Admission and publication

Each active producer owns one exclusive Runtime producer slot. The initial
profile has 32 slots and 64 ingress cells, giving every slot two cells. Slot
registration may happen during documented warm-up. When no slot is available,
the call is rejected as drop-newest before context or message evaluation. A
retiring slot is reusable only after its open writer is gone, its lane is
drained, and its cached and returned credit is reconciled with the central
bound; a generation prevents a stale producer-local handle from using the
reassigned slot.

An ordinary attempt performs these operations in order:

```text
claim the producer's next lane cell
-> reserve the worst-case serialized Record charge
-> capture context and construct the message into private storage
-> commit the actual immutable Record size
-> publish the Record
```

Lane or byte exhaustion abandons all tentative state, invokes no caller
callback, and consumes no admission sequence. Callback or internal validation
failure before publication likewise abandons the lane claim and byte
reservation. The initial policy is drop-newest: the producer neither waits,
evicts an older Record, scans another lane, nor scans routes.

The successful global admission-sequence fetch inside publication is the
linearization point. Record and byte-charge ownership transfer to the pipeline
at that point. All validation and other fallible work precedes it; the remaining
cell publication is infallible. A consumer that observes a later sequence while
the next cell is still becoming visible reports pending instead of bypassing the
earlier publication.

The consumer holds its lane claim and read-only Record view throughout the
consumer callback. It then releases the Record's ledger ownership, resets the
private slot, and finally acknowledges the lane cell. A producer therefore
cannot wrap around and overwrite storage while it is still observed.

## Accounting and initial profile

The first implementation uses a 64-byte accounting quantum and a 64-byte
minimum Record charge. If `S` is the serialized size, its charge is
`max(64, round_up(S, 64))`. An open writer contributes its complete worst-case
serialized size to logical retained bytes and its rounded charge to physical
retained bytes. Commit shrinks logical Record ownership to the actual serialized
size and gives that Record its actual rounded charge. Any unused difference
becomes returned or cached producer credit, so total physical retained bytes
need not fall until that credit is reclaimed even though no logical Record owns
it.

The evidence-backed, configurable initial profile is:

| Parameter | Initial value |
| --- | ---: |
| Accounting quantum and minimum charge | 64 bytes |
| Maximum serialized Record | 16,384 bytes |
| Producer slots | 32 |
| Ingress cells | 64 |

The 1 MiB capacity used by the workload is a comparison fixture, not a
production pipeline-budget default. The exact prototype's 64 fixed Record slots
reserve 1,052,672 backing bytes independently of live retained charges. A
production Runtime must disclose and bound fixed backing storage separately,
then include it with retained payload, producer state, and all other pools in
its declared maximum Ulog-owned memory.

## Evidence

The reservation experiment ran two independent 1,680-row controlled matrices.
Producer credits improved advisory p99.9 for many accepted small and medium
Record cells with 16 or 32 producers, but the central ledger won for saturated
rejections and several large-Record cells. Credits retained up to 524,288 bytes
above logical occupancy while remaining inside the hard bound. The crossover
supports the selected priority--accepted high-concurrency producer tail
latency--without claiming that credits win every workload. Raw result hashes
and ratios are recorded in the
[reservation prototype](../reservation-ledger-prototype.md).

The storage experiment validated all 2,520 controlled rows at revision
`2da97030ea3b2dcb779c3acde5fcc4fe3c90260f`. Contiguous storage has a 64-byte
minimum charge and less than 64 bytes of fragmentation. Chunked storage has a
256-byte minimum and up to 224
bytes of fragmentation; hybrid storage has a 512-byte minimum and up to 992
bytes of fragmentation. Timing crossed over between layouts and did not
identify a winner, so contiguous storage is selected for deterministic memory,
simple traversal, and implementation locality rather than a timing claim. The
raw result SHA-256 is
`6733634d9716ca13b519e1cb80cd493cf1d2456f7bad631c9e2a126bb7c3f8ba`.

The ingress experiment validated 2,520 rows and 252,000,000 measured attempts
at revision `f3c7e017f71546c6e41ed6d99b89908fcd10d15c`. Producer lanes and the
ring admitted every capacity-eligible topology attempt. Lanes published an 11-action producer
bound, while the ring published 70. Chunked ingress also published an 11-action
bound but rejected 55,581 balanced attempts on mapped-chunk contention, with no
stable latency or throughput advantage. The raw result SHA-256 is
`913021da09bb280117e808446ab86785efe08cbbb7ccacc2b74a53ab48c377f0`.

The selected composition then validated all 840 controlled rows and 84,000,000
measured attempts at exact code revision
`28858895cc197ffa748baa2426720b0e9320ee82` on the two-vCPU `dockervm`.
It accepted 57,662,500 attempts and rejected 26,337,500 before callbacks,
exactly as the byte model requires. Allocation, allocation-failure, accounting,
retained-bound, FIFO, Record-validation, publication, lifecycle, and topology
rejection counters were all zero. The run finished in 7 minutes 47 seconds
under a 1,200-second external limit; raw result SHA-256 is
`0412a92d3b8b214d13e76d9e5cb25895748b1a14dacbc248bf2887331273f3ae`.

Representative medians across its seven advisory repetitions show the cost of
Record size and oversubscription; they are baselines, not hard gates:

| Empty workload by requested message size | Accepted p99 | Accepted p99.9 | Records/s | Requested bytes/s |
| --- | ---: | ---: | ---: | ---: |
| 1 producer, 64-byte message | 0.515 us | 7.555 us | 359,644 | 23.0 MB |
| 32 producers, 64-byte message | 0.951 us | 9.693 us | 284,809 | 18.2 MB |
| 1 producer, 16,384-byte message | 18.639 us | 31.299 us | 30,145 | 0.494 GB |
| 32 producers, 16,384-byte message | 16.641 us | 41.898 us | 120,547 | 1.975 GB |

At 32 producers, a near-full 16,384-byte requested-message cell accepted 3.125%
of attempts; its
median accepted p99/p99.9 was 15.602/31.328 us, while rejected p99 was 0.198 us.
A saturated cell evaluated no callbacks and had median rejected p99/p99.9 of
0.176/4.280 us. These timings come from two vCPUs with no confirmation run and
therefore do not establish hardware-independent thresholds.

The bounded composition stress fills all 64 ingress cells with 32 producers,
realizes the exact logical and physical ceilings, then checks every randomized
Record, sequence, ownership transition, conservation identity, and final drain.
Warm allocation instrumentation reports zero general-purpose allocations. The
exact revision also passed all five hosted jobs, including Clang ThreadSanitizer,
in [Actions run 32898071823](https://github.com/Arenoros/ulog/actions/runs/32898071823).

## Rejected alternatives and consequences

A central ledger is simpler, retains no idle credit, and is better for some
large-Record and rejection-heavy workloads. It is retained as a fallback if
production evidence reverses the priority, but it is not the initial kernel.
Chunked and hybrid Record storage do not offer a deterministic advantage that
offsets their larger minimum charges, fragmentation, and segmented traversal.
The ring retains shared spare capacity but exposes a much larger producer-work
bound. Chunked ingress combines fixed-capacity skew with additional contention
rejection.

Producer credits and lanes deliberately trade utilization for bounded producer
work. Credit owned by an idle producer remains unavailable until reconciliation,
and a hot lane may reject while another lane is empty. The initial path does not
steal cells or credit on a producer call. The single consumer may inspect at
most the configured producer-slot count to find the next sequence. These costs
must remain visible in statistics and later controlled evidence.

Future drop-oldest, adaptive sampling, priority, route selection, and progress
metadata remain private ingress-envelope concerns. They may change admission
before publication or let a non-producer owner reclaim an unclaimed Record, but
they do not expose the Record representation or add a route scan to producers.
Producer credits draw only from the payload budget; control and progress
reserves are independently bounded and remain available at payload saturation.
