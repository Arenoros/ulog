# Bound retries and reconnect

Every route may configure bounded retry and reconnect behavior with attempt and
deadline limits, exponential backoff with jitter, and a retained-byte budget.
Required Routes propagate exhausted capacity into pipeline backpressure, while
Best-Effort Routes may shed locally; after exhaustion a route enters an explicit
failed state reported by statistics and ordered operations instead of retrying
forever or silently losing errors.

After exhaustion the route opens a circuit breaker, retains no unbounded retry
backlog, and performs bounded single half-open probes; Reopen may request a probe
explicitly. If a TCP or pipe write fails after the peer may have received some
bytes, Ulog always drops that ambiguous frame and never replays it, then
reconnects only for later Records. This avoids duplicates but does not promise
delivery; the drop is reported distinctly. File writes instead continue from a
known partial offset when possible.
