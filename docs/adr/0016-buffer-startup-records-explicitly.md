# Buffer startup Records explicitly

Ulog provides an explicitly constructed, byte-bounded BootstrapLogger that can
replace the initial static NullLogger before a Runtime exists. It stores owned
Structured Records without background I/O, then hands buffered Records to a
real Runtime Logger and forwards concurrent stale-pointer calls during that
transition. BootstrapLogger remains address-stable for the application lifetime;
ordinary Logger implementations retain the cheaper no-forwarding hot path.
