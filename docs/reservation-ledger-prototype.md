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
occupancy matrix. Protocol `ulog-workload-results/2` requires both candidate
names, so a missing implementation or matrix cell fails validation. Exact
admission, accounting conservation, bounds, allocation failures, and callback
suppression are deterministic gates. Hosted timing remains advisory.

The randomized oracle applies reserve, partial/full commit, abandon, release,
credit refill, and credit return transitions and compares every resulting
snapshot with a simple sequential model. Hand-written boundary tests cover
zero, capacity, over-capacity, and integer-overflow requests, callback rejection,
builder failure, move-only ownership, and cross-thread credit return.

Use a controlled result only for performance comparison:

```shell
ulog-workload-benchmarks --ulog_mode=controlled --benchmark_color=false \
  --benchmark_out=controlled-results.json --benchmark_out_format=json
python scripts/benchmark_results.py validate controlled-results.json
```

Compare the seven repetitions per cell, with particular attention to p99.9 for
16 and 32 producers under near-full and saturated occupancy. Keep raw JSON with
the source revision and machine details. Timing evidence informs the later
architecture decision; this prototype deliberately leaves that decision open.
