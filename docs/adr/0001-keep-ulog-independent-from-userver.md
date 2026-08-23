# Keep Ulog independent from userver

Ulog is a public standalone library and must not depend on userver in its
sources, build, installation, or regular test suite. Cross-repository migration
and parity checks may use userver as an external reference, but that dependency
must not become part of the shipped library or its consumer contract; this
keeps the dependency direction from userver adapters toward Ulog.
