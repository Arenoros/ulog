# Bound pipeline memory and configure shedding

Each Ulog pipeline has a configurable byte budget instead of relying only on a
record count. When retaining another record would exceed that budget, the
configured shedding algorithm selects drop-newest, drop-oldest, or thinning;
Ulog never falls back to unbounded payload allocation. The budget covers open
RecordWriters, queued Records, encoded batches, retry queues, and Ulog-owned
in-flight buffers. A separate bounded reserve keeps Flush, Reopen, and Shutdown
available when the payload budget is exhausted.

Drop-oldest may evict only the oldest Record not yet claimed by Dispatcher; it
never retracts encoding or I/O, and falls back to rejecting the incoming Record
when immediate eviction is impossible. Adaptive sampling accepts everything
below a low watermark, then deterministically reduces admission as occupancy
approaches the limit. Error and Critical Records use a configurable priority
reserve and are shed last, but never trigger hidden synchronous I/O.

Drop-newest is the default because admission can reject before message
construction without scanning or eviction. Priority-aware drop-oldest lets an
incoming Record evict only an equal- or lower-priority eligible Record, choosing
the oldest candidate. Adaptive sampling uses a stateless deterministic hash of
call-site identity and sequence against an occupancy-dependent keep ratio;
Ulog does not retain a dynamic table of call-site sampling state.

The pipeline hard budget is supplemented by configurable route budgets so one
slow route cannot retain the whole budget. The priority and control reserves
remain separately bounded and are included in the Runtime's declared maximum
Ulog-owned payload memory.

Blocking remains a separate explicit `Block(deadline)` mode and is never the
default or available on Ulog-owned threads. A Record exceeding
`max_record_bytes` is truncated at a valid UTF-8 boundary, receives the
`ulog.truncated=true` technical field, and increments a dedicated counter; Ulog
does not allocate outside the budget.
