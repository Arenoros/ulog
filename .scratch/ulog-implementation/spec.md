Status: open
Type: task
Labels: ready-for-agent
GitHub roadmap: https://github.com/Arenoros/ulog/issues/1
GitHub Project: https://github.com/users/Arenoros/projects/5

# Standalone Ulog Implementation

## Problem Statement

Applications need a standalone, performance-oriented C++20 logging library
that preserves the useful logging capabilities of the pinned userver baseline
without depending on userver or reproducing its internal architecture and known
defects. The implementation must provide predictable producer latency, bounded
memory, native Windows support, asynchronous file and network delivery through
libuv, explicit lifecycle and failure reporting, and a native Ulog API.

The existing repository contains the build, packaging, CI, benchmark, and agent
infrastructure, but only exposes version metadata. Production logging behavior
must now be introduced through measurable vertical slices while preserving the
architectural and performance contracts already recorded in the repository.

## Solution

Implement Ulog as an explicitly owned Runtime containing bounded memory pools,
a producer-side admission and Structured Record path, a Dispatcher and route
workers, and a private libuv I/O loop. Logging calls construct fully owned,
immutable Structured Records only after filtering and admission reservation.
Records are routed and encoded away from producer threads, then delivered by
built-in or custom Sinks with bounded batching, retry, and in-flight state.

Progress through independently verifiable tracer bullets. Begin with parity
evidence and performance experiments for reservation, record storage, and
ingress topology. Then deliver one complete in-memory route, one asynchronous
file route, the full format and overload models, multiple isolated routes,
network transports, operational capabilities, optional OTLP integration, and
finally a userver-owned adapter and release hardening.

## User Stories

1. As an application developer, I want to use Ulog without linking userver, so
   that logging has an independent dependency and release lifecycle.
2. As a C++ developer, I want a native API in the `ulog` namespace, so that the
   standalone library does not expose compatibility-shaped abstractions.
3. As an existing userver logging user, I want the complete `LOG*` call-site
   macro family, so that the same logging capabilities remain convenient.
4. As a build owner, I want Ulog configuration macros to use the `ULOG_*`
   prefix, so that no userver-owned configuration namespace leaks into Ulog.
5. As a latency-sensitive application, I want compile-time-erased and
   runtime-filtered calls not to evaluate their message expressions, so that
   disabled logging has minimal and predictable cost.
6. As a latency-sensitive application, I want an admission-rejected call not to
   evaluate its message expression, so that overload does not add formatting
   work to producer threads.
7. As a performance owner, I want no general-purpose heap allocation on the
   warmed ordinary producer path, so that allocator contention does not shape
   producer tail latency.
8. As an operator, I want a configurable global byte budget, so that logging
   cannot consume unbounded application memory.
9. As an operator, I want open writers, queued Records, encoded batches,
   retries, and in-flight buffers included in the declared memory bound, so that
   the budget represents the whole Ulog payload pipeline.
10. As an operator, I want drop-newest, priority-aware drop-oldest, adaptive
    thinning, and explicit deadline-bounded blocking policies, so that overload
    behavior matches the service's reliability needs.
11. As an operator, I want a reserved budget for Error and Critical Records, so
    that important diagnostics are shed last without synchronous I/O.
12. As an operator, I want oversized Records truncated at a valid UTF-8
    boundary and marked explicitly, so that one message cannot violate the
    configured memory bound.
13. As an application owner, I want Logger route topology to be immutable, so
    that ordinary logging does not coordinate with arbitrary reconfiguration.
14. As an application owner, I want atomic Logger level changes, so that
    verbosity can change without rebuilding Runtime topology.
15. As an application owner, I want an atomic non-owning Default Logger target,
    so that unnamed macros perform one pointer load without locks or reference
    counting.
16. As an application owner, I want stale producers to finish safely after a
    Default Logger exchange, so that the exchange requires no quiescence wait.
17. As an application owner, I want an explicit bounded Bootstrap Logger, so
    that startup Records can be retained before Runtime exists when requested.
18. As a logging consumer, I want ordered typed fields, so that structured
    information survives independently of text formatting.
19. As a logging consumer, I want native scalar, string, fmt, stream, lambda,
    range, map, optional, chrono, exception, hexadecimal, and quoted writing
    capabilities, so that common values can be recorded without intermediate
    strings.
20. As a logging consumer, I want TSKV, LTSV, raw text, JSON, and JSON YaDeploy
    encoders, so that existing text destinations remain supported.
21. As a framework integrator, I want a ContextProvider extension seam, so that
    tracing, task, and request metadata can be captured without a Ulog-to-userver
    dependency.
22. As a format integrator, I want an Encoder extension seam, so that new output
    formats can be added without exposing the private Record layout.
23. As a destination integrator, I want a Sink extension seam with exactly-once
    completion, so that custom synchronous or asynchronous delivery can be
    integrated without exposing libuv types.
24. As an operator, I want Required and Best-Effort Routes, so that a slow
    optional destination does not stall independent delivery while declared
    required work applies pipeline pressure.
25. As an operator, I want route-local byte budgets, so that one destination
    cannot retain the entire global budget unexpectedly.
26. As an operator, I want batching bounded by Records, bytes, and delay, so
    that throughput improvements do not create unbounded latency or memory use.
27. As an operator, I want high-severity Records to wake delivery immediately,
    so that batching does not delay important diagnostics.
28. As an operator, I want asynchronous stdout, stderr, and file delivery, so
    that producer threads never perform filesystem I/O.
29. As an operator, I want file append, truncate, reopen, rotation support, and
    partial-write continuation on every supported platform, so that local file
    logging behaves predictably.
30. As an operator, I want Drain separated from Durable Flush, so that ordinary
    completion does not claim physical persistence and `fsync` remains explicit.
31. As an operator, I want asynchronous TCP delivery with bounded reconnect and
    retry behavior, so that an unavailable endpoint cannot retain work forever.
32. As an operator, I want ambiguous partially delivered stream frames dropped
    and reported rather than replayed, so that recovery does not introduce
    silent duplicates.
33. As a POSIX operator, I want `unix:<path>` local IPC, so that Unix domain
    sockets use their native address and behavior.
34. As a Windows operator, I want `pipe:<name>` local IPC, so that named pipes
    are implemented natively rather than through POSIX emulation.
35. As an operator, I want Drain, Durable Flush, Reopen, and Shutdown to return
    lightweight Operations, so that completion can be polled, observed by one
    callback, or waited for with an explicit deadline.
36. As an operator, I want exact per-route barrier reports, so that work before
    an operation watermark can be reconciled by Records and bytes.
37. As a monitoring owner, I want fixed-cardinality live statistics, so that
    accepted, delivered, shed, failed, retried, and unfinished outcomes are
    visible without dynamically keyed hot-path state.
38. As a performance owner, I want enabling statistics not to add shared
    accepted-path counter traffic, so that observability does not create a
    producer contention point.
39. As a developer, I want rate-limited macros and dropped-count reporting, so
    that noisy call sites can remain diagnostically useful.
40. As a developer, I want source-location dynamic debug, so that selected call
    sites can be enabled or disabled at runtime without a hot-path map lookup.
41. As a developer, I want optional stacktrace capture and symbolization, so
    that expensive diagnostics are available without affecting ordinary logs.
42. As an OTLP user, I want OTLP support isolated in an optional package, so
    that protobuf and transport dependencies do not enter Ulog core.
43. As a userver maintainer, I want framework integration to live in a
    userver-owned adapter, so that dependency direction remains `userver` to
    Ulog only.
44. As a library consumer, I want static and shared packages on Windows, Linux,
    and macOS, so that the same public behavior is installable across supported
    platforms.
45. As a release owner, I want deterministic allocation, memory, ordering,
    accounting, fault, and lifecycle tests plus controlled performance gates,
    so that performance and reliability claims remain measurable.

## Implementation Decisions

- The behavioral reference is the pinned userver revision recorded by the
  migration baseline. Its source is external diagnostic evidence only.
- A committed parity manifest and golden corpus describe preserved capabilities
  and deliberate differences. Regular Ulog builds and tests do not access the
  userver repository.
- The first engineering gate compares reservation, storage, and ingress designs
  under the maintained workload matrix before production interfaces depend on
  their details. The selected design is recorded as an ADR.
- Filtering and byte reservation occur before evaluation of caller message
  expressions. A successful reservation guarantees enough bounded resources to
  finish or validly truncate the accepted Record.
- Structured Record storage is private, compact, fully owned, immutable after
  publication, and never a public wire format.
- Admission sequence is assigned only at the publication linearization point.
  Adaptive thinning uses a separate reservation-attempt nonce because rejected
  Records have no admission sequence.
- Every retained object type has bounded capacity or a minimum accounting
  charge. Payload bytes, metadata nodes, route references, Operations, and I/O
  requests cannot escape declared limits.
- A progress reserve prevents a valid Required Route configuration from
  deadlocking while a Structured Record and its encoded representation coexist.
- Runtime construction validates the full configuration, reserves pools, and
  starts fixed workers before returning a usable Runtime.
- Producers publish through one bounded ingress handoff and never scan routes,
  encode output, call extensions, or perform I/O.
- Routing and encoding run on fixed Ulog workers. Built-in I/O runs on a
  dedicated thread owning a private libuv loop.
- Built-in file operations use `uv_fs_*`; stream and pipe operations use native
  libuv mechanisms. Ulog bounds its submitted filesystem work and does not read
  or change `UV_THREADPOOL_SIZE`.
- Logger topology is immutable. Logger levels and documented lightweight
  admission controls may change atomically. Topology changes create a new
  application-long Logger state.
- The process-wide Default Logger pointer initially targets a static NullLogger.
  Every Logger state ever published as default retains a stable address for the
  application lifetime.
- Bootstrap Logger is a distinct bounded implementation. Only its transition
  path checks forwarding; ordinary Runtime Logger calls do not.
- Encoder consumes ordered RecordView batches and a bounded output writer. It
  performs no I/O and retains neither Records nor output writer state.
- Sink accepts serialized requests with move-only exactly-once completion.
  Calls to one Sink are serialized, and completion may be inline or deferred.
- A bounded BlockingSinkAdapter supports synchronous custom destinations away
  from producer and libuv-loop threads.
- Required Routes apply pressure to global admission. Best-Effort Routes may
  shed within their route budgets without delaying unrelated Routes.
- Drop-newest is the initial and default policy. Drop-oldest, thinning, priority
  reserve, and explicit blocking are added only after the first bounded pipeline
  is model-tested.
- Drain, Durable Flush, Reopen, and Shutdown enqueue preallocated control nodes
  from a separate bounded reserve and return Operations with explicit results.
- Live statistics have closed cardinality. Ordinary accepted producer calls do
  not increment a shared statistics counter.
- Userver-specific tracing, task context, YAML, components, dynamic config,
  signals, testsuite capture, and metrics export belong to a userver-owned
  adapter using Ulog public seams.
- OTLP and stacktrace support are optional capabilities whose dependencies and
  packaging are selected separately from Ulog core.
- `fmt` is the native formatting dependency. libuv is the asynchronous I/O
  dependency. No production dependency on userver is permitted.
- Public hot-path calls do not emit Ulog exceptions. Construction, configuration,
  and ordered operations return explicit failures with correction guidance.
- Concrete defaults for pool sizes, record limits, batch limits, worker counts,
  and timing gates are selected from benchmark evidence and then recorded in
  the Performance Contract.

## Testing Decisions

- The highest common seam is an installed-package consumer that constructs a
  Runtime, logs through public `LOG*` APIs, observes a public test destination,
  temporary file, or loopback endpoint, and completes through Operation.
- Golden behavior tests cover the five built-in formats, escaping, ordered
  fields, timestamp and source semantics, truncation, duplicate and frozen
  fields, macro filtering, rate limiting, dynamic debug, and Bootstrap handoff.
- Side-effect probes prove that erased, filtered, NullLogger, sampled-out, and
  admission-rejected calls do not evaluate message expressions where required.
- Allocation instrumentation verifies zero warmed general-heap allocations on
  disabled and ordinary accepted native paths within documented writer limits.
- Model-based and randomized tests compare byte ledgers, FIFO order, shedding,
  barrier reports, retries, and lifecycle transitions against a simple oracle.
- UTF-8 truncation, escaping, binary Record parsing, and structured encoders
  receive property and fuzz coverage.
- Queue candidates are measured with 1, 2, 4, 8, 16, and 32 producers, multiple
  Record sizes, and empty, partial, near-full, and saturated occupancy.
- Benchmarks record p50, p99, and p99.9 producer latency, Records per second,
  bytes per second, CPU consumption, allocations, and retained high-water.
- Macro code-generation and behavior tests cover MSVC, GCC, and Clang, `_TO`
  forms, nested control flow, stream and fmt forms, and one Default Logger load.
- Route tests prove per-route FIFO, Best-Effort isolation, Required pressure,
  bounded batching, exact completion, and absence of extension calls on producer
  or libuv-loop threads.
- File tests cover append, truncate, reopen, rotation, partial writes, Drain,
  Durable Flush, cancellation, deadline, and bounded in-flight requests.
- TCP and IPC tests use fault-injection peers for slow reads, refused connects,
  resets, ambiguous partial frames, reconnect, retry exhaustion, and shutdown.
- Windows named-pipe and POSIX Unix-domain tests validate native behavior and
  actionable invalid-address diagnostics independently.
- Operation state-machine tests cover callback races, polling, deadline waits,
  full-payload control reserve, watermarks, and exact unfinished accounting.
- Statistics benchmarks compare enabled and disabled configurations and reject
  shared-cacheline contention on the ordinary accepted path.
- Every production slice passes static and shared build, install, CMake and
  Conan consumer, sanitizer, independence, formatting, and supported-platform
  CI checks appropriate to the changed behavior.
- Hosted-runner timing remains advisory. Hard latency and throughput gates run
  repeatedly on controlled hardware and require a confirmation rerun.

## Out of Scope

- Source-level or ABI compatibility with `userver::logging`.
- Reproduction of known baseline defects or contradictory defaults.
- A file-for-file extraction of userver logging implementation.
- A shipped, build-time, or regular-test dependency on userver.
- Userver component, YAML, dynamic-config, signal, tracing, testsuite, or metrics
  integration inside the Ulog repository.
- Process-global ownership or modification of libuv's thread-pool settings.
- Remote acknowledgement guarantees for TCP, pipes, or OTLP.
- Replay of a stream frame after partial delivery becomes ambiguous.
- Unbounded queues, retries, batches, Operations, statistics keys, or payload
  allocation.
- Initial ABI stability for extension interfaces.
- Publishing packages, tags, or a release as part of the implementation effort.
- Final numeric performance thresholds before a controlled workload and runner
  protocol have been measured and approved.

## Further Notes

- The critical path is parity evidence, performance kernel, producer frontend,
  bounded single-route pipeline, asynchronous file delivery, structured
  formats, complete admission, route isolation, and ordered Operations.
- After route and Operation contracts stabilize, network/IPC and advanced
  call-site capabilities can proceed independently.
- OTLP follows network and route work. The downstream userver adapter follows a
  stable Ulog public API and remains a separate repository effort.
- The first production tracer ends at a real asynchronous file route rather
  than only an internal class boundary.
- All implementation work must continue to follow `CONTEXT.md`, the ADRs, and
  the maintained Performance Contract.
