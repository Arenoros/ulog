# Keep route topology immutable

A Logger's routes, Encoders, Sinks, and ownership graph are immutable after
creation so producer and Dispatcher paths do not coordinate with arbitrary
adapter replacement. Levels and lightweight admission settings remain atomic,
Reopen remains an ordered operation, and a topology change creates another
application-long Logger followed by an atomic default-pointer exchange.
