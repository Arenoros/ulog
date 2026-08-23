# Own Runtime and libuv lifecycle explicitly

Applications explicitly own `ulog::Runtime`, which owns bounded pools,
Dispatcher, workers, and a private libuv loop. Logger objects are inexpensive
handles into a Runtime, macros without `_TO` use an explicitly installed default
Logger, and orderly completion requires explicit shutdown rather than relying
on process-global construction or destruction order.

`Runtime::Create` validates the complete configuration, reserves global pools,
and starts its fixed workers before returning a usable Runtime; producer-local
storage may be established during explicit or first-use warm-up. Shutdown first
closes admission, then attempts a final Drain until its deadline, cancels
remaining I/O on timeout, and returns exact remaining-work accounting. Runtime
destruction performs only bounded best-effort cleanup and never waits forever.

Default Logger lookup follows the minimal non-owning model: a process-wide
atomic raw pointer initially targets a static NullLogger, and each macro loads
that pointer exactly once. Publishing a new default performs no locking or
reference-count operation. Consequently, every Logger state ever published as
default must keep a stable address until application termination; its Runtime
owns it for that lifetime even after it stops accepting Records.

Changing the default performs only the atomic pointer exchange. A producer that
already loaded the previous pointer completes against the previous Logger; all
published Logger states remain valid, and ordinary Logger calls do not check a
forwarding pointer. Before an application explicitly installs a Logger, the
static NullLogger discards Records. Applications that need startup capture may
explicitly install Ulog's special BootstrapLogger before constructing Runtime;
only that type pays for transition forwarding.
