# Use native local IPC addresses

Local IPC sinks use explicit platform-native address schemes: `unix:<path>` on
POSIX and `pipe:<name>` on Windows. Both may use libuv's pipe facilities behind
the Sink interface, but neither scheme impersonates the semantics of the other
platform.
