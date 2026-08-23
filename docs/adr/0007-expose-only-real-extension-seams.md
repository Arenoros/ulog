# Expose only real extension seams

Ulog provides source-stable interfaces for Encoder, Sink, and ContextProvider,
which each have multiple production or test adapters. Dispatcher internals,
queues, libuv handles, and requests remain private so callers learn the logging
contract rather than the asynchronous implementation; no initial ABI stability
is promised for the extension interfaces.

Sink receives serialized high-level requests with a move-only completion that
must be completed exactly once; calls for one Sink are serialized, completion
may be inline or asynchronous, and no libuv type crosses the interface. Ulog
provides a bounded BlockingSinkAdapter for synchronous implementations. Encoder
receives ordered RecordView batches and a bounded output writer, performs no
I/O, and retains neither views nor the writer. A single-record adapter is
provided for simple formats.

ContextProvider runs on the producer after successful admission reservation and
before Record publication. It may be called concurrently, must not block, and
copies context into a bounded ContextWriter so no producer-local reference
survives capture.
