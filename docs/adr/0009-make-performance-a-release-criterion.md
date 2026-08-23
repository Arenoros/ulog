# Make performance a release criterion

Ulog treats application-thread overhead and sustained delivery performance as
first-class release criteria rather than optional tuning. Architecture and
feature-parity choices require benchmark evidence and explicit latency, memory,
and throughput trade-offs; concrete budgets belong to a maintained Performance
Contract instead of an untestable claim that the library is "fast."

Producer-side tail latency is the primary optimization target and sustained
delivery throughput is secondary. After warm-up, Ulog-owned disabled paths and
ordinary accepted records within documented limits make no calls to the
general-purpose heap. Per-route FIFO and barriers are required, while completion
ordering between independent routes is deliberately not guaranteed so routes
may execute concurrently.

Every pull request enforces deterministic allocation and memory invariants and
smoke-runs the benchmark binaries on supported platforms. Hosted-runner timing
comparisons are advisory; timing regressions become hard gates only on stable,
controlled runners.
