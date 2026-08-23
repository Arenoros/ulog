# Use a native Ulog API

The standalone library exposes its C++ API from the `ulog` namespace, owns the
complete existing `LOG*` call-site macro family, and uses `ULOG_*` names for its
configuration macros without legacy `USERVER_*` aliases. No compatibility API
for `userver::logging` is required; userver integrations migrate to the native
Ulog interface, keeping Ulog's public API and release lifecycle independent.
