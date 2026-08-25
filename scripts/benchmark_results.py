#!/usr/bin/env python3

import argparse
import json
import math
import re
import sys
from pathlib import Path


RESULT_PROTOCOL = "ulog-workload-results/2"
TIMING_POLICY = "advisory"
EXPECTED_CANDIDATES = (
    "central-reservation",
    "producer-credit-reservation",
)
EXPECTED_CANDIDATE_DECLARATION = ",".join(EXPECTED_CANDIDATES)
MODE_REPETITIONS = {"controlled": 7, "smoke": 1}
MODE_WARMUP_ROUNDS = {"controlled": 64, "smoke": 8}
SMOKE_MEASURED_ROUNDS = 64
CONTROLLED_MINIMUM_SAMPLES_PER_CELL = 100_000
MODES = set(MODE_REPETITIONS)
PRODUCER_COUNTS = (1, 2, 4, 8, 16, 32)
RECORD_SIZES = (64, 256, 1024, 4096, 16384)
LOGICAL_LIMIT_BYTES = 1_048_576
OCCUPANCY_BYTES = {
    "empty": 0,
    "partial": 524_288,
    "near-full": 1_032_192,
    "saturated": LOGICAL_LIMIT_BYTES,
}
WORKLOAD_ROW_PATTERN = re.compile(
    r"^UlogWorkload/(?P<candidate>[a-z0-9.-]+)/"
    r"producers:(?P<producers>1|2|4|8|16|32)/"
    r"record_bytes:(?P<record_size>64|256|1024|4096|16384)/"
    r"occupancy:(?P<occupancy>empty|partial|near-full|saturated)/"
    r"repetition:(?P<repetition>[0-6])"
    r"(?:/iterations:1/manual_time)?$"
)

INTEGER_COUNTERS = {
    "producer_count",
    "record_size_bytes",
    "workload_repetition_index",
    "warmup_rounds",
    "measured_rounds",
    "sample_count",
    "attempted_records",
    "accepted_records",
    "rejected_records",
    "accepted_bytes",
    "rejected_bytes",
    "allocation_count",
    "allocation_failure_count",
    "logical_retained_initial_bytes",
    "logical_retained_high_water_bytes",
    "logical_retained_final_bytes",
    "logical_retained_limit_bytes",
    "physical_retained_initial_bytes",
    "physical_retained_high_water_bytes",
    "physical_retained_final_bytes",
    "physical_retained_limit_bytes",
    "accounting_error_count",
    "retained_bound_error_count",
}
NONNEGATIVE_COUNTERS = {
    "wall_time_ns",
    "process_cpu_time_ns",
    "cpu_utilization_percent",
    "producer_latency_p50_ns",
    "producer_latency_p99_ns",
    "producer_latency_p999_ns",
    "attempts_per_second",
    "records_per_second",
    "bytes_per_second",
}
REQUIRED_COUNTERS = INTEGER_COUNTERS | NONNEGATIVE_COUNTERS
RATE_RELATIVE_TOLERANCE = 1e-6
RATE_ABSOLUTE_TOLERANCE = 1e-9


class BenchmarkResultsError(RuntimeError):
    pass


def reject_duplicate_keys(
    pairs: list[tuple[str, object]],
) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r} is not allowed")
        result[key] = value
    return result


def reject_nonstandard_number(value: str) -> None:
    raise ValueError(f"non-standard JSON number {value!r} is not allowed")


def parse_strict_json(contents: str) -> object:
    return json.loads(
        contents,
        object_pairs_hook=reject_duplicate_keys,
        parse_constant=reject_nonstandard_number,
    )


def read_result_file(path: Path) -> dict[str, object]:
    try:
        raw_contents = path.read_bytes()
    except OSError as error:
        raise BenchmarkResultsError(
            f"Unable to read benchmark result '{path}': {error}. "
            "Check the path and retry."
        ) from error
    if raw_contents.startswith(b"\xef\xbb\xbf"):
        raise BenchmarkResultsError(
            f"Benchmark result '{path}' must be UTF-8 without a BOM. "
            "Write plain UTF-8 JSON and retry."
        )
    try:
        contents = raw_contents.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise BenchmarkResultsError(
            f"Benchmark result '{path}' is not valid UTF-8 at byte "
            f"{error.start}. Write plain UTF-8 JSON and retry."
        ) from error
    try:
        document = parse_strict_json(contents)
    except (json.JSONDecodeError, ValueError) as error:
        raise BenchmarkResultsError(
            f"Benchmark result '{path}' is not strict JSON: {error}. "
            "Regenerate the benchmark output and retry."
        ) from error
    if not isinstance(document, dict):
        raise BenchmarkResultsError(
            f"Benchmark result '{path}' must contain one JSON object."
        )
    return document


def require_context(document: dict[str, object]) -> tuple[str, int, tuple[str, ...]]:
    context = document.get("context")
    if not isinstance(context, dict):
        raise BenchmarkResultsError(
            "Benchmark result must contain a Google Benchmark 'context' object."
        )

    expected_context = {
        "ulog_result_protocol": RESULT_PROTOCOL,
        "ulog_timing_policy": TIMING_POLICY,
    }
    for key, expected in expected_context.items():
        if context.get(key) != expected:
            raise BenchmarkResultsError(
                f"Benchmark context {key!r} must be {expected!r}; found "
                f"{context.get(key)!r}. Configure the workload benchmark context "
                "and retry."
            )

    candidate_declaration = context.get("ulog_candidates")
    if candidate_declaration != EXPECTED_CANDIDATE_DECLARATION:
        raise BenchmarkResultsError(
            "Benchmark context 'ulog_candidates' must be the canonical candidate "
            f"declaration {EXPECTED_CANDIDATE_DECLARATION!r}; found "
            f"{candidate_declaration!r}. Register every reservation candidate in "
            "the workload executable and retry."
        )

    mode = context.get("ulog_mode")
    if not isinstance(mode, str) or mode not in MODES:
        choices = ", ".join(sorted(MODES))
        raise BenchmarkResultsError(
            f"Benchmark context 'ulog_mode' must be one of: {choices}; "
            f"found {mode!r}."
        )

    repetitions_value = context.get("ulog_repetitions")
    if not isinstance(repetitions_value, str) or not re.fullmatch(
        r"[1-9][0-9]*", repetitions_value
    ):
        raise BenchmarkResultsError(
            "Benchmark context 'ulog_repetitions' must be a canonical positive "
            f"decimal string; found {repetitions_value!r}."
        )
    expected_repetitions = MODE_REPETITIONS[mode]
    if repetitions_value != str(expected_repetitions):
        raise BenchmarkResultsError(
            f"Benchmark context 'ulog_repetitions' must be "
            f"{expected_repetitions} for {mode!r} mode; found {repetitions_value!r}. "
            f"Run the benchmark with --ulog_mode={mode} and retry."
        )
    return mode, expected_repetitions, EXPECTED_CANDIDATES


def expected_measured_rounds(mode: str, producers: int) -> int:
    if mode == "smoke":
        return SMOKE_MEASURED_ROUNDS
    return max(
        SMOKE_MEASURED_ROUNDS,
        (CONTROLLED_MINIMUM_SAMPLES_PER_CELL + producers - 1) // producers,
    )


def require_number(row: dict[str, object], field: str, row_name: str) -> float:
    value = row[field]
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise BenchmarkResultsError(
            f"Workload row '{row_name}' counter '{field}' must be numeric; "
            f"found {value!r}."
        )
    try:
        number = float(value)
    except OverflowError as error:
        raise BenchmarkResultsError(
            f"Workload row '{row_name}' counter '{field}' must be finite and "
            f"representable; found an out-of-range number."
        ) from error
    if not math.isfinite(number) or number < 0:
        raise BenchmarkResultsError(
            f"Workload row '{row_name}' counter '{field}' must be finite and "
            f"nonnegative; found {value!r}."
        )
    return number


def require_integer(row: dict[str, object], field: str, row_name: str) -> int:
    value = row[field]
    if isinstance(value, int) and not isinstance(value, bool):
        if value < 0:
            raise BenchmarkResultsError(
                f"Workload row '{row_name}' counter '{field}' must be finite and "
                f"nonnegative; found {value!r}."
            )
        return value
    number = require_number(row, field, row_name)
    if not number.is_integer():
        raise BenchmarkResultsError(
            f"Workload row '{row_name}' counter '{field}' must be an integer; "
            f"found {row[field]!r}."
        )
    return int(number)


def require_equal(row_name: str, description: str, actual: object, expected: object) -> None:
    if actual != expected:
        raise BenchmarkResultsError(
            f"Workload row '{row_name}' has {description} {actual!r}; "
            f"expected {expected!r}. Fix the benchmark accounting and retry."
        )


def require_close(row_name: str, field: str, actual: float, expected: float) -> None:
    if not math.isclose(
        actual,
        expected,
        rel_tol=RATE_RELATIVE_TOLERANCE,
        abs_tol=RATE_ABSOLUTE_TOLERANCE,
    ):
        raise BenchmarkResultsError(
            f"Workload row '{row_name}' counter '{field}' is {actual!r}; "
            f"expected {expected!r} from its exact accounting and wall time."
        )


def validate_row(
    row: object, row_index: int, mode: str, repetitions: int
) -> tuple[str, int, int, str, int]:
    if not isinstance(row, dict):
        raise BenchmarkResultsError(
            f"Benchmark row {row_index} must be a JSON object."
        )
    row_name = row.get("name")
    if not isinstance(row_name, str):
        raise BenchmarkResultsError(
            f"Benchmark row {row_index} must contain a string 'name'."
        )
    if row.get("error_occurred", False) is not False:
        raise BenchmarkResultsError(
            f"Workload row '{row_name}' has Google Benchmark 'error_occurred' set. "
            "Fix the benchmark failure and regenerate the result."
        )
    iterations = row.get("iterations")
    if isinstance(iterations, bool) or iterations != 1:
        raise BenchmarkResultsError(
            f"Workload row '{row_name}' field 'iterations' must be integer 1; "
            f"found {iterations!r}. Keep the deterministic single-iteration wrapper."
        )
    if row.get("run_type") != "iteration":
        raise BenchmarkResultsError(
            f"Workload row '{row_name}' field 'run_type' must be 'iteration'; "
            f"found {row.get('run_type')!r}. Disable aggregate-only output."
        )
    match = WORKLOAD_ROW_PATTERN.fullmatch(row_name)
    if not match:
        raise BenchmarkResultsError(
            f"Benchmark row name '{row_name}' does not match the required "
            "UlogWorkload/<candidate>/producers:<count>/record_bytes:<bytes>/"
            "occupancy:<state>/repetition:<index> format."
        )

    missing = sorted(REQUIRED_COUNTERS - row.keys())
    if missing:
        raise BenchmarkResultsError(
            f"Workload row '{row_name}' is missing required counters: "
            f"{', '.join(missing)}. Emit every ulog-workload-results/2 counter."
        )

    integers = {
        field: require_integer(row, field, row_name) for field in INTEGER_COUNTERS
    }
    numbers = {
        field: require_number(row, field, row_name)
        for field in NONNEGATIVE_COUNTERS
    }

    candidate = match.group("candidate")
    producers = int(match.group("producers"))
    record_size = int(match.group("record_size"))
    occupancy = match.group("occupancy")
    repetition = int(match.group("repetition"))
    if repetition >= repetitions:
        raise BenchmarkResultsError(
            f"Workload row '{row_name}' repetition index {repetition} is outside "
            f"context range 0..{repetitions - 1}."
        )

    require_equal(
        row_name, "producer_count", integers["producer_count"], producers
    )
    require_equal(
        row_name, "record_size_bytes", integers["record_size_bytes"], record_size
    )
    require_equal(
        row_name,
        "workload_repetition_index",
        integers["workload_repetition_index"],
        repetition,
    )

    require_equal(
        row_name,
        "warmup_rounds",
        integers["warmup_rounds"],
        MODE_WARMUP_ROUNDS[mode],
    )
    measured_rounds = expected_measured_rounds(mode, producers)
    require_equal(
        row_name,
        "measured_rounds",
        integers["measured_rounds"],
        measured_rounds,
    )
    attempted_records = producers * measured_rounds
    initial_bytes = OCCUPANCY_BYTES[occupancy]
    accepted_per_round = min(
        producers, (LOGICAL_LIMIT_BYTES - initial_bytes) // record_size
    )
    accepted_records = accepted_per_round * measured_rounds
    rejected_records = attempted_records - accepted_records

    exact_values = {
        "sample_count": attempted_records,
        "attempted_records": attempted_records,
        "accepted_records": accepted_records,
        "rejected_records": rejected_records,
        "accepted_bytes": accepted_records * record_size,
        "rejected_bytes": rejected_records * record_size,
        "logical_retained_initial_bytes": initial_bytes,
        "logical_retained_high_water_bytes": (
            initial_bytes + accepted_per_round * record_size
        ),
        "logical_retained_final_bytes": initial_bytes,
        "logical_retained_limit_bytes": LOGICAL_LIMIT_BYTES,
        "allocation_failure_count": 0,
        "accounting_error_count": 0,
        "retained_bound_error_count": 0,
    }
    for field, expected in exact_values.items():
        require_equal(row_name, field, integers[field], expected)

    for suffix, stage in (
        ("initial_bytes", "initial"),
        ("high_water_bytes", "high-water"),
        ("final_bytes", "final"),
        ("limit_bytes", "limit"),
    ):
        logical = integers[f"logical_retained_{suffix}"]
        physical = integers[f"physical_retained_{suffix}"]
        if physical < logical:
            raise BenchmarkResultsError(
                f"Workload row '{row_name}' physical retained {stage} bytes "
                f"{physical} are below logical retained {stage} bytes {logical}. "
                "Fix candidate retained-memory accounting and retry."
            )

        if candidate == "central-reservation":
            require_equal(
                row_name,
                f"central candidate physical retained {stage} bytes",
                physical,
                logical,
            )

    for prefix in ("logical", "physical"):
        initial = integers[f"{prefix}_retained_initial_bytes"]
        high_water = integers[f"{prefix}_retained_high_water_bytes"]
        final = integers[f"{prefix}_retained_final_bytes"]
        limit = integers[f"{prefix}_retained_limit_bytes"]
        require_equal(
            row_name, f"{prefix} retained final occupancy", final, initial
        )
        if high_water < initial or high_water < final:
            raise BenchmarkResultsError(
                f"Workload row '{row_name}' {prefix} retained high-water "
                f"{high_water} is below retained occupancy {max(initial, final)}."
            )
        if initial > limit or high_water > limit or final > limit:
            raise BenchmarkResultsError(
                f"Workload row '{row_name}' exceeds its {prefix} retained limit "
                f"{limit}: initial={initial}, high-water={high_water}, final={final}."
            )

    p50 = numbers["producer_latency_p50_ns"]
    p99 = numbers["producer_latency_p99_ns"]
    p999 = numbers["producer_latency_p999_ns"]
    if not p50 <= p99 <= p999:
        raise BenchmarkResultsError(
            f"Workload row '{row_name}' producer latency percentiles must satisfy "
            f"p50 <= p99 <= p99.9; found {p50}, {p99}, {p999}."
        )

    wall_time_ns = numbers["wall_time_ns"]
    if wall_time_ns == 0:
        raise BenchmarkResultsError(
            f"Workload row '{row_name}' counter 'wall_time_ns' must be positive."
        )
    rate_scale = 1_000_000_000.0 / wall_time_ns
    require_close(
        row_name,
        "attempts_per_second",
        numbers["attempts_per_second"],
        attempted_records * rate_scale,
    )
    require_close(
        row_name,
        "records_per_second",
        numbers["records_per_second"],
        accepted_records * rate_scale,
    )
    require_close(
        row_name,
        "bytes_per_second",
        numbers["bytes_per_second"],
        accepted_records * record_size * rate_scale,
    )
    require_close(
        row_name,
        "cpu_utilization_percent",
        numbers["cpu_utilization_percent"],
        numbers["process_cpu_time_ns"] * 100.0 / wall_time_ns,
    )

    return candidate, producers, record_size, occupancy, repetition


def validate_document(document: dict[str, object]) -> tuple[int, int, int]:
    mode, repetitions, declared_candidates = require_context(document)
    rows = document.get("benchmarks")
    if not isinstance(rows, list) or not rows:
        raise BenchmarkResultsError(
            "Benchmark result must contain a non-empty Google Benchmark "
            "'benchmarks' array."
        )

    observed: set[tuple[str, int, int, str, int]] = set()
    candidates: set[str] = set()
    for row_index, row in enumerate(rows):
        key = validate_row(row, row_index, mode, repetitions)
        if key in observed:
            raise BenchmarkResultsError(
                "Benchmark result contains duplicate workload cell "
                f"{key!r}. Emit each candidate/matrix/repetition cell once."
            )
        observed.add(key)
        candidates.add(key[0])

    declared_candidate_set = set(declared_candidates)
    if candidates != declared_candidate_set:
        missing_candidates = sorted(declared_candidate_set - candidates)
        undeclared_candidates = sorted(candidates - declared_candidate_set)
        details = []
        if missing_candidates:
            details.append("missing " + ", ".join(missing_candidates))
        if undeclared_candidates:
            details.append("undeclared " + ", ".join(undeclared_candidates))
        raise BenchmarkResultsError(
            "Benchmark rows must match context 'ulog_candidates' exactly: "
            + "; ".join(details)
            + ". Register every declared candidate and remove undeclared rows."
        )

    expected = {
        (candidate, producers, record_size, occupancy, repetition)
        for candidate in declared_candidates
        for repetition in range(repetitions)
        for producers in PRODUCER_COUNTS
        for record_size in RECORD_SIZES
        for occupancy in OCCUPANCY_BYTES
    }
    missing = sorted(expected - observed)
    extra = sorted(observed - expected)
    if missing or extra:
        details = []
        if missing:
            details.append(
                f"missing {len(missing)} cell(s), first missing cell: {missing[0]!r}"
            )
        if extra:
            details.append(
                f"unexpected {len(extra)} cell(s), first: {extra[0]!r}"
            )
        raise BenchmarkResultsError(
            "Benchmark workload matrix is incomplete: " + "; ".join(details) + "."
        )

    return len(rows), len(candidates), repetitions


def validate_result_file(path: Path) -> tuple[int, int, int]:
    return validate_document(read_result_file(path))


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate Ulog's versioned Google Benchmark result protocol."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    validate_parser = subparsers.add_parser(
        "validate", help="Validate one benchmark-results.json file."
    )
    validate_parser.add_argument("result", type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        if arguments.command == "validate":
            rows, candidates, repetitions = validate_result_file(arguments.result)
            print(
                f"Validated {rows} workload row(s) for {candidates} candidate(s) "
                f"and {repetitions} repetition(s)"
            )
            return 0
    except BenchmarkResultsError as error:
        print(f"benchmark result validation failed: {error}", file=sys.stderr)
        return 1
    raise AssertionError(f"Unhandled command {arguments.command!r}")


if __name__ == "__main__":
    raise SystemExit(main())
