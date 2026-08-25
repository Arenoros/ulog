# Structured Record storage prototype

This private benchmark prototype compares three ways to retain one fully owned,
immutable Structured Record. It supplies design evidence for a later production
layout; it does not install a Record type, define a public binary format, select
a winner, or implement an Encoder.

The prototype follows the producer contract in
[ADR 0004](adr/0004-reimplement-logging-as-a-pipeline.md): construction and
context capture finish before publication, and a consumer may inspect the
Record after the producer and all caller-owned inputs have gone away. Publication
ends mutation. Candidate-specific storage and read views remain under
`benchmarks/` and are not public Ulog API.

## Common Record recipe

All candidates build the same deterministic logical Record so comparisons do
not accidentally measure different work:

| Part | Stored value |
| --- | --- |
| Event timestamp | `1,704,067,200,123,456` |
| Source | path `benchmarks/record_storage_benchmark.cpp`, function `TryProduce`, line `91` |
| Message | workload bytes, written through the fmt-oriented output path |
| Ordered field 1 | string `kind="benchmark"` |
| Ordered field 2 | signed 64-bit integer `signed=-7` |
| Ordered field 3 | unsigned 64-bit integer `unsigned=42` |
| Ordered field 4 | double `ratio=1.25` |
| Ordered field 5 | bool `sampled=true` |
| Ordered field 6 | null `optional=null` |

Source strings, message bytes, field names, and string field values are copied
into candidate-owned storage. Scalars and type information are stored by value.
No view, formatting argument, or reference into caller storage survives
publication, and the published Record offers read-only access.

The private writer represents native string and scalar paths as well as a
fmt-oriented message path. The maintained benchmark uses the fmt-oriented path;
unit coverage exercises the native paths and segmented writes across candidate
boundaries. These paths cover the native zero-allocation scope in the
[Performance Contract](performance-contract.md), not allocations performed by
caller expressions or custom formatters.

Each candidate has a 16,384-byte serialized-Record limit. When the complete
Record would not fit, only the message is shortened, to the largest prefix that
leaves room for source data, ordered fields, and metadata and ends on a valid
UTF-8 boundary. The footprint reports the shorter stored message and the
benchmark reports the truncation flag and accepted truncated-Record count. This
prototype flag is evidence for the production truncation contract; the final
production layout must also preserve the `ulog.truncated=true` behavior required
by the Performance Contract.

Warm ordinary writes within that limit use fixed, prepared candidate storage
and make no general-purpose heap allocation. Workload setup, producer-thread
creation, and result sample storage happen outside the measured producer
attempt.

## Candidates

The candidates differ in their real backing organization, not only in reported
accounting:

| Candidate | Private storage organization | Accounting unit |
| --- | --- | --- |
| `contiguous-record` | one contiguous 16,384-byte region | 64 bytes |
| `chunked-record` | 64 independent 256-byte chunks | 256 bytes |
| `hybrid-record` | one inline 512-byte region, 15 independent 1,024-byte overflow chunks, and one final 512-byte region | 512 bytes inline, then 1,024-byte overflow steps |

Chunked and hybrid reads and writes traverse segment boundaries. Their
published views therefore do not depend on the logical message, source, or
field bytes being contiguous.

## Footprint and budget accounting

Every benchmark row reports these common terms:

- `requested_message_bytes`: message bytes requested by the workload;
- `stored_message_bytes`: message bytes retained after any UTF-8-safe
  truncation;
- `owned_payload_bytes`: the retained message, source path and function, every
  field key, and the string field value;
- `metadata_bytes`: one 48-byte Record header plus one 24-byte entry for each
  field; scalar values live in those field entries;
- `serialized_bytes`: logical bytes used by the complete private Record;
- `fragmentation_bytes`: bytes charged because of the candidate's accounting
  granularity;
- `accounting_charge_bytes`: physical byte-budget charge for one admitted
  Record; and
- `minimum_accounting_charge_bytes`: the smallest charge for the candidate.

The validator enforces both identities exactly:

```text
serialized_bytes = owned_payload_bytes + metadata_bytes
accounting_charge_bytes = serialized_bytes + fragmentation_bytes
accounting_charge_bytes = owned_payload_bytes + metadata_bytes + fragmentation_bytes
```

For the common recipe, fixed owned strings occupy 96 bytes and the six fields
bring metadata to 192 bytes. The largest message that can coexist with the
recipe is therefore 16,096 bytes; the 16,384-byte workload cell exercises
truncation in every candidate.

Let `S` be `serialized_bytes`, and let `round_up(value, quantum)` return the
smallest multiple of `quantum` that is not less than `value`. Candidate charges
are:

```text
contiguous-record: max(64, round_up(S, 64))
chunked-record:    max(256, round_up(S, 256))

hybrid-record:
  512                                     when S <= 512
  512 + round_up(S - 512, 1,024)          when 512 < S <= 15,872
  16,384                                  when 15,872 < S <= 16,384
```

Accordingly, the minimum charges are 64, 256, and 512 bytes. Logical retained
memory uses `serialized_bytes`; admission and physical retained memory use
`accounting_charge_bytes`. This distinction lets the shared byte-budget
workload expose candidate fragmentation without changing the logical Record.

## Benchmark protocol

The executable emits Google Benchmark JSON under the sibling protocol
`ulog-record-storage-results/2`. Its canonical candidates are
`contiguous-record,chunked-record,hybrid-record`, its timing policy is
`advisory`, and its schedule is `six-permutation-cycle`.

Each row has this stable identity (Google Benchmark may append its own suffix):

```text
UlogRecordStorage/<candidate>/producers:<count>/record_bytes:<bytes>/occupancy:<state>/repetition:<index>
```

The workload matrix is the same 1 MiB byte-budget matrix described in
[Benchmarking](benchmarking.md): 1, 2, 4, 8, 16, or 32 producers; requested
messages of 64, 256, 1,024, 4,096, or 16,384 bytes; and empty, partial,
near-full, or saturated initial occupancy. Admission uses the candidate's
physical charge, while accepted and rejected workload bytes retain the requested
message-size identity.

For each matrix cell, candidate rows are adjacent. Repetition indices cycle
through all six orders before repeating:

```text
contiguous, chunked, hybrid
contiguous, hybrid, chunked
chunked, contiguous, hybrid
chunked, hybrid, contiguous
hybrid, contiguous, chunked
hybrid, chunked, contiguous
```

Smoke mode has one repetition and therefore 360 rows. Controlled mode has seven
repetitions and 2,520 rows. The seventh repetition starts the cycle again. This
ordering reduces persistent first-run bias; it does not turn shared-host timing
into a release gate.

In addition to the common latency, throughput, CPU, admission, allocation, and
logical/physical retained-memory counters, each row reports the footprint terms,
accepted- and rejected-attempt latency summaries, truncation count, and
`record_validation_error_count`. `scripts/record_storage_results.py` rejects an
incomplete candidate inventory or matrix, the wrong schedule, cross-candidate
logical differences, invalid footprint or charge arithmetic, unexpected
allocations, retained-bound failures, and Record validation failures.

## Reproduction

Configure a Release build with `ULOG_BUILD_BENCHMARKS=ON`; the Conan workflow in
[Testing](testing.md) provides Google Benchmark and fmt. Build the Record-storage
benchmark and use its smoke mode for a short protocol check:

```shell
cmake --build <build-dir> --config Release --target ulog-record-storage-benchmarks
<build-dir>/bin/ulog-record-storage-benchmarks --ulog_mode=smoke \
  --benchmark_color=false \
  --benchmark_out=record-storage-results.json \
  --benchmark_out_format=json
python scripts/record_storage_results.py validate record-storage-results.json
```

The benchmark CTest label also smoke-runs and validates the result on supported
CI platforms:

```shell
ctest --test-dir <build-dir> -C Release -L benchmark --output-on-failure
```

Before collecting controlled measurements, validate the complete registered
schedule, then run on a dedicated, otherwise idle machine:

```shell
python scripts/check_record_storage_schedule.py \
  <build-dir>/bin/ulog-record-storage-benchmarks
<build-dir>/bin/ulog-record-storage-benchmarks --ulog_mode=controlled \
  --benchmark_color=false \
  --benchmark_out=controlled-record-storage-results.json \
  --benchmark_out_format=json
python scripts/record_storage_results.py validate \
  controlled-record-storage-results.json
```

On a multi-config Windows build, the executable is normally
`<build-dir>/bin/Release/ulog-record-storage-benchmarks.exe`. Archive the raw
JSON with the exact source revision, compiler, dependency lockfile, operating
system, and machine configuration.

The deterministic ownership, immutability, allocation, UTF-8, accounting, and
protocol checks are acceptance evidence. Latency and throughput remain advisory
until repeated on controlled hardware and confirmed by a fresh rerun. Results
from this prototype compare trade-offs; they do not select the final private
layout or create a public Record-storage contract.
