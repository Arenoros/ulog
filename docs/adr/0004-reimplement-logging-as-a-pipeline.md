# Reimplement logging as a pipeline

Ulog is a behavioral reimplementation rather than a source-structure
extraction. Its architecture gives Record, Encoder, Sink, and Dispatcher
separate modules so that structured data, output formats, destinations, and
asynchronous delivery have explicit seams while preserving the observable
capabilities of the extraction baseline.

The ingress queue contains fully owned immutable Structured Records, represented
by a private compact binary layout. Producer threads finish message construction
and context capture before publishing a record; route-specific serialization is
performed later through Encoder. The private layout is not a public wire format,
and the initial implementation does not cache an already encoded primary text
payload without benchmark evidence.

After successful admission the producer captures one wall-clock event timestamp
before Record publication. Sequence defines admission order and Encoder formats
the stored timestamp later; Ulog does not add a second monotonic timestamp to
every Record without a demonstrated use that justifies its producer cost.
