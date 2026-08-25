# Ulog

Ulog is a public standalone library that reimplements the logging behavior of
the userver extraction baseline without requiring userver.

## Language

**Ulog**:
The standalone logging library and its native public API.
_Avoid_: userver logging subsystem, extracted logger

**Native API**:
The Ulog-owned API exposed from the `ulog` C++ namespace together with the
complete `LOG*` call-site macro family and `ULOG_*` configuration macros.
_Avoid_: userver logging API, compatibility API

**Feature parity**:
The state in which existing userver logging capabilities remain available
end-to-end after extraction, even when userver-specific behavior is supplied by
an adapter rather than by Ulog itself. Known defects of the extraction baseline
are not compatibility requirements.
_Avoid_: code parity, file-for-file extraction

**Performance contract**:
The measurable producer-overhead, delivery-throughput, allocation, and bounded
memory criteria that every Ulog release must satisfy.
_Avoid_: fast logging, best-effort optimization

**Pipeline byte budget**:
The configured upper bound for Ulog-owned payload retained across open writers,
queued Records, encoded batches, retries, and in-flight I/O buffers.
_Avoid_: queue length, ingress limit

**Shedding**:
The configured non-blocking overload response that rejects a new Record, evicts
an eligible ingress Record, or adaptively samples admissions before saturation.
_Avoid_: silent loss, unbounded buffering

**Required Route**:
A delivery route whose retained work applies pressure to the whole pipeline
rather than being discarded independently when that route is slow.
_Avoid_: reliable delivery, lossless route

**Best-Effort Route**:
A delivery route allowed to shed its own retained output so that a slow target
does not delay independent routes.
_Avoid_: optional logger, unreliable sink

**Runtime**:
The explicitly owned Ulog instance that contains memory pools, Dispatcher,
libuv loop, workers, and lifecycle state shared by its Logger handles.
_Avoid_: global logger, per-logger thread

**Default Logger**:
The non-owning process-wide Logger target loaded atomically by `LOG*` macros
that do not name a Logger explicitly. Every target ever published in this role
has an application-long stable address.
_Avoid_: shared logger, implicitly created runtime

**Null Logger**:
The process-lifetime Logger state at the suppressing `None` threshold. It owns
no Runtime or delivery state, and it is the initial Default Logger target.
_Avoid_: startup buffer, empty runtime

**Bootstrap Logger**:
An explicitly created, application-long default target that stores early
Structured Records in a bounded in-memory buffer and later hands them to a
Runtime Logger while forwarding stale calls during the transition.
_Avoid_: initial NullLogger, general logger forwarding

**Drain**:
An ordered barrier that waits for prior Records to finish encoding and local I/O
completion without promising durable storage or remote acknowledgement.
_Avoid_: durable flush, delivery acknowledgement

**Durable Flush**:
A file-specific ordered barrier that performs Drain and then requests `fsync`.
_Avoid_: socket flush, guaranteed physical persistence

**Operation**:
The lightweight completion handle returned by an ordered Runtime action such as
Drain, Durable Flush, Reopen, or Shutdown. It supports polling, one callback,
and explicit deadline-bounded waiting without entering the logging hot path.
_Avoid_: control command, logging future

**Userver adapter**:
The userver-owned integration layer that connects framework-specific behavior
to Ulog without introducing a dependency from Ulog to userver.
_Avoid_: Ulog compatibility mode, userver backend

**Behavioral reimplementation**:
A new implementation that preserves the observable logging capabilities of the
extraction baseline without copying its module structure or internal design.
_Avoid_: source extraction, file-for-file port

**Structured Record**:
The fully owned, immutable logical log event admitted to the asynchronous
pipeline. It contains the completed message, source information, timestamps,
ordered typed fields, and producer-captured context. Its compact binary storage
is private implementation rather than a public wire format.
_Avoid_: encoded log line, borrowed formatting arguments

**Producer kernel**:
The private bounded part of a Runtime that admits, completes, and transfers one
Structured Record from an application producer into the asynchronous pipeline.
_Avoid_: public queue, producer backend

**Encoding**:
Route-specific serialization of a Structured Record into TSKV, LTSV, JSON,
raw text, OTLP, or another sink payload after ingress admission. Message
construction and context capture still finish on the producer before the
record is published.
_Avoid_: deferred evaluation of caller-owned values

**I/O loop**:
The dedicated thread that owns Ulog's private libuv loop and performs built-in
sink I/O. Routing and encoding execute on a separate worker so extension code
cannot stall libuv callbacks.
_Avoid_: external libuv loop, encoder thread
