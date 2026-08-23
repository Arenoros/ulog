# Support Windows in the first release

The first public Ulog release must support native Windows in addition to Linux
and macOS, even though the extraction baseline of userver does not build on
Windows. Windows receives first-class implementations rather than emulations of
POSIX behavior, so portability is a design constraint for the API, runtime,
sinks, tests, and CI rather than a later compatibility project.
