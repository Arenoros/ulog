# Non-format parity scenarios

Issue: [#6](https://github.com/Arenoros/ulog/issues/6)

Parent roadmap: [#1](https://github.com/Arenoros/ulog/issues/1)

<!-- ulog-non-format-scenarios-schema: 1 -->

Catalog baseline:

```text ulog-non-format-baseline
repository: userver
commit: 72e07f717ae46a17822776df21ebd73dbc4ce728
```

This catalog specifies observable logging behavior that cannot be compared as
one encoded record. The baseline is userver commit
[`72e07f717ae46a17822776df21ebd73dbc4ce728`](https://github.com/userver-framework/userver/tree/72e07f717ae46a17822776df21ebd73dbc4ce728),
as recorded in [the migration baseline](baseline.md). The checkout was verified
read-only on `arenoros@dockervm`; it is evidence, not a Ulog build or test input.

Each machine-readable block is a regular Ulog oracle. Regular validation reads
this committed file and [the capability manifest](capability-manifest.md), checks
the JSON schema and stable IDs, and does not find, build, or execute userver.
Source citations and rationale outside the blocks are explanatory evidence.

## Compile-time filtering

### `compile-filter-cutoff-matrix`

```json ulog-non-format-scenario
{
  "id": "compile-filter-cutoff-matrix",
  "category": "compile-time-filtering",
  "feature_ids": ["API-005", "DYN-001", "LIM-001"],
  "difference_ids": ["DEF-011", "DIFF-001"],
  "inputs": {
    "build_cutoffs": ["none", "trace", "debug", "info", "warning", "error"],
    "library_kinds": ["shared", "static"],
    "macro_families": [
      "named-default",
      "named-explicit",
      "named-limited-default",
      "named-limited-explicit",
      "generic",
      "generic-limited",
      "critical"
    ],
    "probe_observables": [
      "argument-evaluation-count",
      "logger-expression-evaluation-count",
      "registered-call-sites",
      "retained-message-literals"
    ]
  },
  "action": [
    "Compile and link the same probe for every cutoff and library kind.",
    "Run each probe and snapshot side-effect counters and the dynamic-debug registry.",
    "Inspect the resulting binary for the unique message literal of every call site."
  ],
  "observable_result": {
    "baseline": [
      "Named default, explicit, and limited macros at or below the cutoff retain type checking but evaluate neither target nor message arguments, register no call site, and retain no unique message literal.",
      "Named macros above the cutoff, generic LOG and LOG_TO, generic limited forms, and Critical remain active.",
      "The LOG_TRACE_TO documentation says that it is unaffected, while its expansion passes through the trace eraser and is removed when the option is set."
    ],
    "decision": [
      "Ulog preserves the coherent implementation matrix, including erasure of named TRACE_TO, and tests documentation examples against that same matrix.",
      "Ulog exposes only ULOG-prefixed configuration names; it does not copy the userver namespace or option name."
    ]
  },
  "parity": "decision-point",
  "determinism": {
    "methods": ["compile-probe", "state"],
    "controls": [
      "Every call site has a unique literal and independent side-effect counters.",
      "The expected survivor set is derived solely from the selected compile definition."
    ]
  }
}
```

Evidence: the option-to-definition mapping is owned by
[`universal/CMakeLists.txt:202-222`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/universal/CMakeLists.txt#L202-L222).
The erased no-op expression and cutoff ladder are in
[`logging/log.hpp:160-218`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/universal/include/userver/logging/log.hpp#L160-L218),
while the ordinary and limited macro families are in
[`logging/log.hpp:241-493`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/universal/include/userver/logging/log.hpp#L241-L493).
The baseline's strongest compile probe checks empty output and absent registered
locations in
[`log_message_skipped_test.cpp:1-87`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/core/src/logging/log_message_skipped_test.cpp#L1-L87).
Manifest rows `API-005`, `LIM-001`, `DYN-001`, `DEF-011`, and `DIFF-001`
own the retained behavior and the explicit decision.

## Default Logger

### `default-logger-exchange`

```json ulog-non-format-scenario
{
  "id": "default-logger-exchange",
  "category": "default-logger",
  "feature_ids": ["API-001", "DFL-001", "DFL-002"],
  "difference_ids": ["DEF-008", "DIFF-003"],
  "inputs": {
    "logger_a": "stable in-memory target admitting Info",
    "logger_b": "stable in-memory target admitting Info",
    "selector": "returns logger A on its first evaluation and logger B on its second",
    "stale_load_gate": "holds one producer after loading logger A and before publication"
  },
  "action": [
    "Submit one admitted explicit-target macro through the side-effecting selector.",
    "Install logger A as the default and hold a producer at the stale-load gate.",
    "Atomically exchange the default to logger B, release the producer, and submit one later default-target record."
  ],
  "observable_result": {
    "baseline": [
      "An admitted LOG_TO evaluates the selector once for filtering and again for construction, so filtering may consult logger A while the record is submitted to logger B.",
      "Default replacement is an atomic non-owning pointer store and does not extend either target's lifetime."
    ],
    "ulog": [
      "Every macro evaluates or loads its target exactly once and uses that stable target through publication.",
      "The gated record lands wholly on logger A, the later record lands on logger B, and both Runtime-owned target addresses remain valid for the documented application lifetime."
    ]
  },
  "parity": "intentional",
  "determinism": {
    "methods": ["barrier", "state"],
    "controls": [
      "The selector's return sequence replaces a probabilistic exchange race with exact counters.",
      "The exchange occurs only after the producer reports that it loaded logger A."
    ]
  }
}
```

Evidence: `LOG_TO` uses its logger expression in both filtering and construction
in
[`logging/log.hpp:241-259`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/universal/include/userver/logging/log.hpp#L241-L259).
The atomic non-owning default pointer, guard, and restoration are implemented in
[`logging/log.cpp:14-57`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/universal/src/logging/log.cpp#L14-L57).
Manifest rows `DFL-001`, `DFL-002`, `DEF-008`, and `DIFF-003` deliberately
replace the double evaluation and implicit lifetime assumptions with a
single-load, stable-address contract.

### `default-logger-initial-null`

```json ulog-non-format-scenario
{
  "id": "default-logger-initial-null",
  "category": "default-logger",
  "feature_ids": ["API-008", "DFL-001", "DST-001"],
  "difference_ids": [],
  "inputs": {
    "installed_default": false,
    "message_levels": ["trace", "debug", "info", "warning", "error", "critical", "none"],
    "message_side_effect_counter": 0
  },
  "action": [
    "Query the initial Default Logger and its threshold.",
    "Attempt to set that logger to Trace, submit every message level, and flush it.",
    "Snapshot the threshold, side-effect counter, and delivered records."
  ],
  "observable_result": {
    "baseline_and_ulog": [
      "The initial process-wide target is a process-lifetime Null Logger at None.",
      "Its threshold cannot be changed, every ordinary level is rejected, message arguments are not evaluated, and flush completes without output.",
      "Installing an explicit stable logger changes only subsequent Default Logger lookups."
    ]
  },
  "parity": "same",
  "determinism": {
    "methods": ["state"],
    "controls": [
      "The scenario runs before any explicit default installation.",
      "All observations are pointer identity, counters, threshold state, and captured records."
    ]
  }
}
```

Evidence: the initial atomic points at `GetNullLogger` in
[`logging/log.cpp:14-36`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/universal/src/logging/log.cpp#L14-L36).
`NullLogger` fixes its level at `None`, ignores `SetLevel`, discards records, and
makes `Flush` empty in
[`null_logger.cpp:25-50`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/universal/src/logging/null_logger.cpp#L25-L50).
Manifest rows `API-008`, `DFL-001`, and `DST-001` retain that observable null
target without retaining baseline ownership types.

## Dynamic debug

### `dynamic-debug-force-matrix`

```json ulog-non-format-scenario
{
  "id": "dynamic-debug-force-matrix",
  "category": "dynamic-debug",
  "feature_ids": ["DYN-002", "LVL-001"],
  "difference_ids": ["DEF-012"],
  "inputs": {
    "force_disabled_boundaries": ["trace", "debug", "info", "warning", "error", "critical", "none"],
    "force_enabled_thresholds": ["trace", "debug", "info", "warning", "error", "critical", "none"],
    "logger_thresholds": ["trace", "debug", "info", "warning", "error", "critical", "none"],
    "message_levels": ["trace", "debug", "info", "warning", "error", "critical", "none"],
    "registered_site": "probe.cpp:41"
  },
  "action": [
    "Apply every logger-threshold, message-level, force-enable, and force-disable combination to the registered site.",
    "Submit exactly one side-effecting message for each state and snapshot eligibility and delivery."
  ],
  "observable_result": {
    "baseline": [
      "A message is filtered when the logger rejects it or its level is below the force-disable boundary, unless force-enable admits that non-None level.",
      "Force-enable wins when both override predicates match, and None is never force-enabled.",
      "Contrary to the baseline enum comment, a force-disabled Critical boundary suppresses Critical.",
      "Removing the override atomically restores ordinary logger-threshold behavior."
    ],
    "decision": [
      "Ulog preserves the observable ordered-level predicate, including explicit Critical suppression, and does not preserve the unconditional enum comment.",
      "Critical priority reserve and bounded shedding remain separate pipeline policies; they do not make Critical immune to explicit filtering."
    ]
  },
  "parity": "decision-point",
  "determinism": {
    "methods": ["state"],
    "controls": [
      "Every cell starts from a freshly assigned atomic override state.",
      "One record counter and one argument counter define the complete truth-table oracle."
    ]
  }
}
```

Evidence: the two override fields and their lock-free atomic representation are
defined in
[`dynamic_debug.hpp:13-40`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/universal/src/logging/dynamic_debug.hpp#L13-L40).
The exact precedence formula is `StaticLogEntry::ShouldNotLog` in
[`logging/log.cpp:139-155`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/universal/src/logging/log.cpp#L139-L155).
The contradictory Critical comment is pinned in
[`logging/level.hpp:20-23`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/universal/include/userver/logging/level.hpp#L20-L23).
Manifest rows `DYN-002`, `LVL-001`, and `DEF-012` own the exhaustive observable
matrix and the explicit decision not to preserve that comment as a contract.

### `dynamic-debug-location-selection`

```json ulog-non-format-scenario
{
  "id": "dynamic-debug-location-selection",
  "category": "dynamic-debug",
  "feature_ids": ["API-006", "DYN-001", "DYN-002"],
  "difference_ids": [],
  "inputs": {
    "erased_site": "probe.cpp:11",
    "exact_site": "probe.cpp:21",
    "missing_site": "missing.cpp:999",
    "prefix": "probe",
    "same_line_site_count": 2
  },
  "action": [
    "Finish static registration and snapshot the normalized path and line registry.",
    "Enable the exact location, then line zero for the path prefix, and remove each override.",
    "Attempt updates for the missing and compile-erased locations."
  ],
  "observable_result": {
    "baseline_and_ulog": [
      "Exact selection updates every registered expansion with the same normalized path and line; line zero updates every registered path with the requested prefix.",
      "Multiple expansions on one line coexist, while a compile-erased site has no registry entry.",
      "An exact or prefix update matching no location fails and identifies the requested path and, when applicable, line."
    ]
  },
  "parity": "same",
  "determinism": {
    "methods": ["compile-probe", "state"],
    "controls": [
      "Fixed source directives give every probe location an exact compiler-selected line.",
      "The registry is queried only after static registration is declared complete."
    ]
  }
}
```

Evidence: registration uses a multiset specifically to retain same-line
expansions in
[`dynamic_debug.hpp:25-50`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/universal/src/logging/dynamic_debug.hpp#L25-L50).
Exact, prefix, missing-location, reset, and registration behavior is implemented
in
[`dynamic_debug.cpp:18-103`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/universal/src/logging/dynamic_debug.cpp#L18-L103)
and exercised without an output-format oracle in
[`dynamic_debug_test.cpp:13-116`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/core/src/logging/dynamic_debug_test.cpp#L13-L116).
Manifest rows `API-006`, `DYN-001`, and `DYN-002` retain location identity and
selection.

## Flush and batching

### `flush-threshold-and-periodic-wakeup`

```json ulog-non-format-scenario
{
  "id": "flush-threshold-and-periodic-wakeup",
  "category": "flush",
  "feature_ids": ["LFC-003", "LVL-003"],
  "difference_ids": [],
  "inputs": {
    "flush_on": "warning",
    "manual_clock_start": "0s",
    "message_queue_capacity": 10,
    "periodic_interval": "2s",
    "route": "gated fake sink with write and flush event counters",
    "threshold_cases": [
      {"requested": 4, "effective": 4},
      {"requested": 8, "effective": 5}
    ]
  },
  "action": [
    "With capacity 10 and requested threshold 4, enqueue three Info records behind the route gate and observe no wake, then enqueue the fourth and observe one wake.",
    "Restart with capacity 10 and requested threshold 8: observe no wake after four Info records and exactly one wake when the fifth record reaches the capped threshold.",
    "Restart below the effective threshold, submit one Warning record, and release the route gate.",
    "Restart below the effective threshold and advance the manual clock from immediately before to exactly at the periodic boundary."
  ],
  "observable_result": {
    "baseline_and_ulog": [
      "A record at or above flush_on wakes asynchronous consumption and flushes every sink after that record, including all earlier queued records.",
      "The uncapped case wakes at four records and not at three; the over-cap request of eight becomes five and wakes at five records but not at four.",
      "The periodic boundary wakes and drains a below-threshold batch without depending on elapsed wall-clock scheduling."
    ]
  },
  "parity": "same",
  "determinism": {
    "methods": ["barrier", "injected-clock", "state"],
    "controls": [
      "The fake sink reports each write and flush through explicit gates and counters.",
      "The manual clock advances directly from before to exactly at the periodic boundary."
    ]
  }
}
```

Evidence: the logger's immediate-flush threshold defaults to Warning and uses an
atomic boundary in
[`logger_base.hpp:47-64`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/universal/include/userver/logging/impl/logger_base.hpp#L47-L64).
`TpLogger::Log` selects immediate, unbatched, or queue-threshold notification in
[`tp_logger.cpp:136-172`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/core/src/logging/tp_logger.cpp#L136-L172).
`StartConsumerTask` caps the requested threshold at half the capacity in
[`tp_logger.cpp:55-67`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/core/src/logging/tp_logger.cpp#L55-L67).
The component's two-second policy and periodic flush live in
[`component.cpp:42-44`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/core/src/logging/component.cpp#L42-L44)
and
[`component.cpp:285-290`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/core/src/logging/component.cpp#L285-L290).
Manifest rows `LVL-003` and `LFC-003` retain the policy while requiring an
injected-clock oracle.

### `flush-watermark`

```json ulog-non-format-scenario
{
  "id": "flush-watermark",
  "category": "flush",
  "feature_ids": ["LFC-002"],
  "difference_ids": ["DIFF-006"],
  "inputs": {
    "deadline": "finite operation deadline",
    "later_record": "C",
    "prior_records": ["A", "B"],
    "routes": ["required fake sink", "best-effort fake sink"]
  },
  "action": [
    "Gate both routes while publishing A and B, request the flush-equivalent operation, and wait until its control node is admitted.",
    "Publish C after that admission watermark, release the prior-record gates, and poll the operation to completion.",
    "Snapshot per-route events and operation accounting at completion."
  ],
  "observable_result": {
    "baseline": [
      "LogFlush enqueues an ordered action, blocks its caller until all earlier queue nodes are processed, and invokes Flush on every sink.",
      "Sink-specific Flush defines the destination effect; the call provides no fsync or remote-acknowledgement contract."
    ],
    "ulog": [
      "Drain returns an asynchronous Operation whose admission watermark includes A and B but excludes C.",
      "Completion reports exact per-route local processing through that watermark, honors the finite deadline, and does not claim fsync or remote acknowledgement."
    ]
  },
  "parity": "intentional",
  "determinism": {
    "methods": ["barrier", "state"],
    "controls": [
      "The operation control node is observed before C is admitted.",
      "Route gates make completion and the included record set explicit."
    ]
  }
}
```

Evidence: `TpLogger::Flush` enqueues a promise-bearing action for both task and
ordinary threads in
[`tp_logger.cpp:111-134`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/core/src/logging/tp_logger.cpp#L111-L134).
The FIFO visitor resolves that promise only after `BackendFlush`, and the
backend attempts every sink, in
[`tp_logger.cpp:20-49`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/core/src/logging/tp_logger.cpp#L20-L49)
and
[`tp_logger.cpp:313-321`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/core/src/logging/tp_logger.cpp#L313-L321).
Manifest row `LFC-002` retains the ordered barrier; `DIFF-006` deliberately
names it `Drain` and gives it asynchronous result semantics.

## Limited logging and dropped counts

### `limited-filtered-attempts`

```json ulog-non-format-scenario
{
  "id": "limited-filtered-attempts",
  "category": "limited-logging",
  "feature_ids": ["DYN-002", "LIM-002"],
  "difference_ids": ["DEF-003"],
  "inputs": {
    "clock": "manual clock fixed inside one limiter interval",
    "fresh_site": "new limited Info call site",
    "logger_threshold": "none",
    "primed_site": "equivalent limited Info call site",
    "site_override": "force-enable Info"
  },
  "action": [
    "Perform one level-filtered attempt at the primed site while leaving the fresh site untouched.",
    "Force-enable both sites without advancing the clock.",
    "Perform the same enabled attempt sequence at both sites and compare records and limited-drop counters."
  ],
  "observable_result": {
    "baseline": [
      "The limiter runs before level and dynamic-debug filtering, so the rejected attempt consumes the primed site's power-of-two schedule.",
      "The primed and fresh sites emit different enabled attempt numbers."
    ],
    "decision": [
      "Ulog accounts a limited attempt only after the call site is eligible and admitted.",
      "The primed site therefore matches the fresh site and the filtered attempt contributes no limited-drop count."
    ]
  },
  "parity": "decision-point",
  "determinism": {
    "methods": ["injected-clock", "state"],
    "controls": [
      "Both sites use fresh explicit limiter state and the same fixed clock value.",
      "Dynamic-debug state changes only after the rejected attempt is observed."
    ]
  }
}
```

Evidence: `LOG_LIMITED_TO` constructs and advances `RateLimiter` before entering
`LOG_TO` in
[`logging/log.hpp:364-375`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/universal/include/userver/logging/log.hpp#L364-L375).
The pinned test records the resulting shifted sequence and labels it a TODO in
[`dynamic_debug_test.cpp:120-151`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/core/src/logging/dynamic_debug_test.cpp#L120-L151).
Manifest row `DEF-003` explicitly decides not to preserve that ordering defect;
`DYN-002` and `LIM-002` identify the interacting capabilities.

### `limited-power-of-two-drops`

```json ulog-non-format-scenario
{
  "id": "limited-power-of-two-drops",
  "category": "limited-logging",
  "feature_ids": ["LIM-001", "LIM-002"],
  "difference_ids": [],
  "inputs": {
    "attempt_numbers": [1, 2, 3, 4, 5, 6, 7, 8],
    "clock": "manual clock fixed inside one limiter interval",
    "default_thresholds": ["info", "debug"],
    "global_limited_states": ["enabled", "disabled"],
    "site_and_thread_count": 2
  },
  "action": [
    "Submit attempts one through eight from one site and thread with limiting enabled at Info.",
    "Repeat with a separate macro expansion and a separate thread.",
    "Repeat at Default Logger threshold Debug and with limited logging globally disabled."
  ],
  "observable_result": {
    "baseline_and_ulog": [
      "Within one interval, attempts 1, 2, 4, and 8 are emitted; attempt 4 reports one accumulated drop and attempt 8 reports three.",
      "Limiter state is independent per macro expansion and per thread.",
      "Default Logger threshold Debug or more verbose, and the global disable control, bypass limiting so every otherwise eligible attempt is emitted."
    ]
  },
  "parity": "same",
  "determinism": {
    "methods": ["injected-clock", "state"],
    "controls": [
      "The clock never crosses the configured reset boundary.",
      "Each site and thread begins with zero attempts and zero accumulated drops."
    ]
  }
}
```

Evidence: the counter, power-of-two admission, accumulated-drop exchange, and
`[N logs dropped]` reporting are implemented in
[`logging/log.cpp:103-136`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/universal/src/logging/log.cpp#L103-L136).
Global enablement, the one-second default interval, and Debug bypass are in
[`rate_limit.cpp:12-39`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/universal/src/logging/rate_limit.cpp#L12-L39).
The baseline tests confirm the power-of-two count and bypass controls in
[`log_message_test.cpp:56-63`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/core/src/logging/log_message_test.cpp#L56-L63),
[`log_message_test.cpp:512-526`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/core/src/logging/log_message_test.cpp#L512-L526),
and
[`ratelimited_log_test.cpp:8-23`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/core/src/logging/ratelimited_log_test.cpp#L8-L23).
Manifest rows `LIM-001` and `LIM-002` retain these observables while requiring
an injected clock.

## Reopen

### `reopen-watermark-partial-failure`

```json ulog-non-format-scenario
{
  "id": "reopen-watermark-partial-failure",
  "category": "reopen",
  "feature_ids": ["DST-008", "LFC-004"],
  "difference_ids": ["DIFF-006"],
  "inputs": {
    "after_record": "after",
    "before_record": "before",
    "file_rotation": "rename the active path after before is written",
    "routes": ["route A reopens successfully", "route B fails its first reopen and succeeds on retry"]
  },
  "action": [
    "Gate both routes after before is admitted, rename the active file, request Reopen, and admit after beyond the operation watermark.",
    "Release the gates, await the first operation, snapshot every route event and statistic, then retry Reopen.",
    "Release after and inspect the rotated and newly opened file paths."
  ],
  "observable_result": {
    "baseline": [
      "Reopen is ordered after prior records, asks every sink to append-open even when one fails, sets the reopen-error flag, and throws one aggregated error; a later successful call clears the flag.",
      "The rotated file contains before and the replacement path receives after; truncate-on-start is a separate configuration action."
    ],
    "ulog": [
      "Reopen is an asynchronous watermark operation that attempts every route and returns exact per-route success and failure accounting instead of throwing.",
      "The first result reports route B's failure, the retry reports recovery, event order is before then reopen then after, and startup truncation remains separate."
    ]
  },
  "parity": "intentional",
  "determinism": {
    "methods": ["barrier", "state"],
    "controls": [
      "Route gates place the file rename and control-node admission between before and after.",
      "Route B uses an explicit fail-first counter rather than an environmental I/O race."
    ]
  }
}
```

Evidence: the queued `ReopenCoro` and wait are in
[`tp_logger.cpp:20-40`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/core/src/logging/tp_logger.cpp#L20-L40)
and
[`tp_logger.cpp:185-198`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/core/src/logging/tp_logger.cpp#L185-L198).
`BackendReopen` continues across sink failures, updates the flag, and aggregates
errors in
[`tp_logger.cpp:323-340`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/core/src/logging/tp_logger.cpp#L323-L340).
The file sink flushes the old handle, append-opens a replacement, closes the old
handle, and swaps in the new one in
[`buffered_file_sink.cpp:18-33`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/core/src/logging/impl/buffered_file_sink.cpp#L18-L33).
Manifest rows `LFC-004` and `DST-008` retain ordering and visible failures;
`DIFF-006` gives Ulog an Operation result rather than the baseline exception.

## Runtime filtering

### `runtime-filter-threshold-matrix`

```json ulog-non-format-scenario
{
  "id": "runtime-filter-threshold-matrix",
  "category": "runtime-filtering",
  "feature_ids": ["API-001", "API-007", "LVL-001"],
  "difference_ids": ["DEF-012"],
  "inputs": {
    "dynamic_override": "neutral",
    "logger_thresholds": ["trace", "debug", "info", "warning", "error", "critical", "none"],
    "message_levels": ["trace", "debug", "info", "warning", "error", "critical", "none"],
    "span_override": "absent",
    "targets": ["default in-memory logger", "explicit in-memory logger"]
  },
  "action": [
    "For each target, set every logger threshold and query every message level through the preflight API.",
    "Submit the same matrix through generic and named macros with a side-effecting message argument.",
    "Snapshot preflight decisions, argument counts, levels, and delivered records."
  ],
  "observable_result": {
    "baseline": [
      "A non-None message is eligible exactly when its level is at least the selected logger threshold; a None threshold suppresses every message level and a None message is always suppressed.",
      "The actual ordered predicate suppresses Critical at a None threshold despite the baseline enum comment claiming that Critical cannot be disabled.",
      "Preflight and macro delivery agree for the same stable target and neutral dynamic-debug state.",
      "A skipped branch does not evaluate message arguments, and explicit-target filtering is independent of the Default Logger threshold."
    ],
    "decision": [
      "Ulog preserves the actual threshold matrix, including Critical suppression at None, and documents Critical priority reserve as a bounded delivery policy rather than immunity from filtering.",
      "Preflight, macro delivery, and argument evaluation must expose the same decision for every matrix cell."
    ]
  },
  "parity": "decision-point",
  "determinism": {
    "methods": ["state"],
    "controls": [
      "Every truth-table cell starts with cleared records and an argument counter of zero.",
      "The scenario excludes span and dynamic-debug overrides so only the ordered-level predicate is observed."
    ]
  }
}
```

Evidence: the owning predicate is `ShouldLogNoSpan` in
[`logger_base.hpp:90-94`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/universal/include/userver/logging/impl/logger_base.hpp#L90-L94),
and the atomic level controls and public preflight wrappers are in
[`logger_base.cpp:15-25`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/universal/src/logging/impl/logger_base.cpp#L15-L25)
and
[`logging/log.cpp:61-95`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/universal/src/logging/log.cpp#L61-L95).
The pinned switching test observes exactly two admitted Trace calls across four
threshold changes in
[`log_test.cpp:15-31`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/core/src/logging/log_test.cpp#L15-L31).
The contradictory Critical comment is pinned in
[`logging/level.hpp:20-23`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/universal/include/userver/logging/level.hpp#L20-L23).
Manifest rows `LVL-001`, `API-001`, `API-007`, and `DEF-012` generalize that
evidence into the exhaustive matrix and make the documentation defect an
explicit decision point.

## Startup forwarding

### `startup-explicit-handoff`

```json ulog-non-format-scenario
{
  "id": "startup-explicit-handoff",
  "category": "startup-forwarding",
  "feature_ids": ["BST-001", "BST-002", "DFL-001"],
  "difference_ids": ["DEF-009", "DIFF-004"],
  "inputs": {
    "bootstrap_logger": "explicit application-lifetime byte-bounded target",
    "early_records": ["A", "B"],
    "initial_default": "null logger",
    "manual_clock_and_context": "distinct producer-time values for every record",
    "runtime_logger": "configured target",
    "stale_call_gate": "holds C after loading the bootstrap target and before publication"
  },
  "action": [
    "Emit one pre-install record to the initial Null Logger, install the BootstrapLogger, and emit A and B.",
    "Hold C at the stale-call gate, hand buffered records to the Runtime Logger, exchange the default, then release C.",
    "Emit D through the new default and snapshot target, order, multiplicity, timestamps, source, context, and pending state."
  ],
  "observable_result": {
    "baseline": [
      "MemLogger itself can replay buffered items in order, clear them, and forward later stale calls, but it re-encodes at handoff and does not retain formatter timestamp or common context.",
      "The pinned LogScope installs MemLogger but invokes ForwardTo on the saved pre-bootstrap logger; with the initial Null Logger, buffered startup items are not replayed to the configured target."
    ],
    "decision": [
      "Ulog keeps initial-default no-capture but makes startup capture an explicit BootstrapLogger choice.",
      "A, B, and stale C reach the Runtime Logger exactly once in controlled FIFO order with producer-time timestamp, source, and context; D follows through the new default, while the pre-install record remains discarded."
    ]
  },
  "parity": "decision-point",
  "determinism": {
    "methods": ["barrier", "injected-clock", "state"],
    "controls": [
      "The stale-call gate fixes C on the old target until handoff has completed.",
      "Injected clock and context values identify preservation independently of encoding time."
    ]
  }
}
```

Evidence: `MemLogger` owns level, class, source, tags, and text, then re-encodes,
clears, and installs a stale-call forward target in
[`mem_logger.cpp:20-104`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/universal/src/logging/impl/mem_logger.cpp#L20-L104).
The framework wiring saves the previous logger, installs `MemLogger`, but calls
`ForwardTo` on the saved logger in
[`components/run.cpp:49-75`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/core/src/components/run.cpp#L49-L75)
and
[`components/run.cpp:147-181`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/core/src/components/run.cpp#L147-L181).
Manifest rows `BST-001`, `BST-002`, `DEF-009`, and `DIFF-004` retain explicit
startup capture but reject the wiring and context-loss defects.

### `startup-overflow-accounting`

```json ulog-non-format-scenario
{
  "id": "startup-overflow-accounting",
  "category": "startup-forwarding",
  "feature_ids": ["BST-001", "DST-008"],
  "difference_ids": ["DEF-001", "DIFF-004", "DIFF-005"],
  "inputs": {
    "bootstrap_byte_budget": 1024,
    "fake_stderr": "empty capture sink",
    "records": [
      "variable-size records that exactly consume 1024 retained bytes",
      "one additional non-empty record"
    ],
    "runtime_logger": "available handoff target"
  },
  "action": [
    "Admit variable-size records up to the exact byte boundary and attempt the additional record.",
    "Snapshot retained bytes and overflow statistics, then hand off every accepted record.",
    "Destroy a separate BootstrapLogger with pending data while observing fake stderr and sink calls."
  ],
  "observable_result": {
    "baseline": [
      "MemLogger uses a hard-coded record count and the greater-than boundary admits 10001 items before silently dropping later calls.",
      "Pending items at static destruction are synchronously written as degraded message text to stderr without full metadata or guaranteed record delimiters."
    ],
    "ulog": [
      "Retained bytes never exceed 1024, the additional record is rejected with exact record and byte counters, and handoff control remains available at the payload boundary.",
      "Every accepted record is handed off exactly once, and destruction performs no implicit stderr, sink, encoding, or other I/O."
    ]
  },
  "parity": "intentional",
  "determinism": {
    "methods": ["state"],
    "controls": [
      "Record sizes and the byte budget are explicit test values with exact allocation accounting.",
      "All diagnostics and I/O seams are fake counters inspected before teardown completes."
    ]
  }
}
```

Evidence: the hard-coded count, greater-than check, silent return, and destructor
`fputs` fallback are in
[`mem_logger.cpp:14-69`](https://github.com/userver-framework/userver/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/universal/src/logging/impl/mem_logger.cpp#L14-L69).
Manifest row `DEF-001` identifies both defects; `BST-001`, `DST-008`,
`DIFF-004`, and `DIFF-005` require a byte-bounded buffer, explicit handoff, and
visible exact accounting instead.

## Validation boundary

The scenario blocks above are intentionally sufficient for regular validation:
their categories, feature IDs, difference IDs, parity shape, and deterministic
methods are closed vocabularies. Pinned userver source is consulted only when a
human reviews or externally refreshes the research. A regular Ulog checkout
therefore validates this catalog offline with no userver source tree, binary,
package, submodule, network request, or subprocess.

Run the same offline seam directly with
`python scripts/non_format_scenarios.py validate --catalog docs/migration/non-format-parity-scenarios.md --manifest docs/migration/capability-manifest.md --baseline docs/migration/baseline.md`.
