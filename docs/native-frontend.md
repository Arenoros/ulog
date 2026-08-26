# Native frontend

The installed package exposes the first native Ulog frontend through
`<ulog/level.hpp>`, `<ulog/source_location.hpp>`, and `<ulog/logger.hpp>`.

`Level` is ordered from `kTrace` through `kCritical`; `kNone` is the suppressing
threshold and is never an enabled message level. `IsLevelEnabled` provides the
same predicate used by Logger.

`SourceLocation::Current()` must be called at the application call site. It
captures file, function, line, and column without allocation. `Custom` accepts
borrowed file and function names for adapters; those names must remain valid
until the synchronous Logger call returns. A future accepted call copies source
data into its owned Record before returning.

`Logger` is a pointer-sized, trivially copied non-owning handle. A
default-constructed Logger, `GetNullLogger()`, and the initial
`GetDefaultLogger()` all refer to the same address-stable static Null Logger.
The Null Logger reports `kNone`, suppresses every level, and performs no message
evaluation, allocation, locking, reference counting, or reclamation.
`ShouldLog()` is an advisory preflight query; `Log()` checks the current level
again so a future concurrent level change cannot bypass filtering.

Message construction is explicitly lazy:

```cpp
logger.Log<ulog::Level::kInfo>(ulog::SourceLocation::Current(), [&]() -> std::string_view {
  return BuildMessageView();
});
```

The factory body is invoked synchronously at most once and only after
compile-time and runtime filtering. Put all message work and side effects inside
that body: C++ constructs the factory argument before calling `Log`, so an eager
init-capture such as `[message = BuildMessage()]` is outside the lazy guarantee.
The result must be usable as `std::string_view` and is consumed before the
factory result is destroyed. Caller exceptions can only occur if the factory is
admitted and remain outside Ulog's no-exception guarantee.

The private production producer kernel now uses this same Logger dispatch. It
claims a producer-local ingress cell and reserves the complete configured
worst-case Record charge before evaluating the factory, then copies the source
and message into bounded owned storage. Rejected calls therefore preserve the
lazy contract. Runtime construction and ownership of that private kernel remain
later roadmap work, so the installed public package still exposes only the Null
and Default Logger accessors.

## Compile-time cutoff

Define `ULOG_COMPILE_TIME_MIN_LEVEL` consistently for all translation units in
one target. It accepts an integer from `0` (`kTrace`) through `5`
(`kCritical`) and defaults to `0`. A templated `Logger::Log` below that minimum
still type-checks the factory contract but does not dispatch or invoke its body.
`kNone` calls compile and are always suppressed. Invalid cutoff values fail
compilation with a correction hint.

The complete basic LOG macro family is a later roadmap slice. Its erased
unnamed forms will apply the same cutoff before loading the process-wide Default
Logger.
