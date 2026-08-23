# Separate Drain from durable file flush

Ulog distinguishes Drain, which waits for prior encoding and local I/O
completion, from Durable Flush, which additionally requests `fsync` for file
routes. TCP and pipe routes support Drain but do not claim remote acknowledgement;
this keeps an expensive durability operation explicit and avoids promising a
delivery guarantee that stream transports cannot provide.

Drain, Durable Flush, Reopen, and Shutdown are named asynchronous methods rather
than alternatives in a public generic command variant. Each enqueues a control
node from a dedicated bounded reserve and returns a lightweight Operation with
polling, one completion callback, and explicit deadline-bounded waiting. Waiting
may block only the caller that requests it; control completion synchronization
does not participate in the Record producer hot path.

Every completed Operation returns an aggregate status and exact per-route
accounting through its admission watermark, including processed, delivered,
shed, failed, retried, and still-unfinished records and bytes. Later admissions
are outside that report, as are Records rejected before obtaining admission and
a sequence number. Those rejections remain visible through weakly consistent
live statistics, and failures are values rather than exceptions.
