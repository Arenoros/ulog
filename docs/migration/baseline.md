# Migration baseline

The behavioral reference for the logging reimplementation is the immutable
source revision:

```text
repository: userver
commit: 72e07f717ae46a17822776df21ebd73dbc4ce728
```

This metadata is diagnostic evidence only. The reference repository is not a
submodule, CMake package, Conan requirement, header source, or regular test
fixture. Behavioral parity work may cite it in migration documents while the
shipped library remains independently buildable and testable.

The explicit capture, normalization, provenance, and regular validation rules
are documented in [the offline baseline corpus workflow](corpus.md).
