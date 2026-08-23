# Use libuv for asynchronous I/O

Ulog uses libuv as its cross-platform asynchronous I/O foundation for files,
sockets, and the first-class Windows implementation. The library must expose
Ulog-owned abstractions rather than libuv types so that event-loop ownership,
ordering, backpressure, shutdown, and error reporting remain explicit parts of
the Ulog runtime contract.

Routing and encoding run on a dedicated worker separate from the dedicated
thread that owns the private libuv loop. Encoder and other extension code must
not execute on the loop thread. Built-in file sinks use the normal asynchronous
`uv_fs_*` interface, accepting libuv's process-wide worker-pool implementation;
socket and pipe sinks use libuv's native evented mechanisms. Ulog limits its own
in-flight requests and does not expose or require an application-owned
`uv_loop_t`.

Ulog never reads, sets, or otherwise manages the process-global
`UV_THREADPOOL_SIZE` setting. Each Runtime instead applies its own configurable
limit to submitted filesystem requests and retains excess work in Ulog's
bounded queues. Applications may tune libuv's global pool before process startup
when they own that process-level policy, but doing so is not required for Ulog
correctness.
