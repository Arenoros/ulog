#!/usr/bin/env python3

import argparse
import re
import sys
from pathlib import Path

from benchmark_results import (
    BenchmarkResultsError,
    read_result_file,
    require_close,
    require_integer,
    require_latency_summary,
    require_number,
)


RESULT_PROTOCOL = "ulog-frontend-results/1"
CANDIDATE = "producer-frontend"
PATHS = (
    "compile-erased",
    "runtime-filtered",
    "null-logger",
    "admission-rejected",
    "ordinary-accepted",
)
MODE_REPETITIONS = {"smoke": 1, "controlled": 7}
SMOKE_WARMUP_ATTEMPTS = 1
SMOKE_MEASURED_ATTEMPTS = 1_024
CONTROLLED_WARMUP_ATTEMPTS = 1_024
CONTROLLED_MEASURED_ATTEMPTS = 100_000
MESSAGE_BYTES = 8
ROW_PATTERN = re.compile(
    r"^(?P<identity>UlogFrontend/producer-frontend/"
    r"path:(?P<path>compile-erased|runtime-filtered|null-logger|"
    r"admission-rejected|ordinary-accepted)/"
    r"repetition:(?P<repetition>[0-6]))(?:/.+)?$"
)
INTEGER_COUNTERS = {
    "workload_repetition_index",
    "warmup_attempts",
    "measured_attempts",
    "sample_count",
    "accepted_latency_sample_count",
    "rejected_latency_sample_count",
    "attempted_records",
    "accepted_records",
    "rejected_records",
    "compile_erased_records",
    "runtime_filtered_records",
    "null_logger_records",
    "admission_rejected_records",
    "message_evaluation_count",
    "default_logger_load_count",
    "accepted_bytes",
    "allocation_count",
    "allocation_failure_count",
    "logical_retained_final_bytes",
    "physical_retained_final_bytes",
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
    "accepted_latency_p50_ns",
    "accepted_latency_p99_ns",
    "accepted_latency_p999_ns",
    "rejected_latency_p50_ns",
    "rejected_latency_p99_ns",
    "rejected_latency_p999_ns",
    "attempts_per_second",
    "records_per_second",
    "bytes_per_second",
}
REQUIRED_COUNTERS = INTEGER_COUNTERS | NONNEGATIVE_COUNTERS


def require_context(document: dict[str, object]) -> tuple[str, int]:
    context = document.get("context")
    if not isinstance(context, dict):
        raise BenchmarkResultsError(
            "Frontend result must contain a Google Benchmark context object."
        )
    expected = {
        "ulog_result_protocol": RESULT_PROTOCOL,
        "ulog_candidates": CANDIDATE,
        "ulog_candidate_schedule": "repetition-major",
        "ulog_frontend_paths": ",".join(PATHS),
        "ulog_timing_policy": "advisory",
    }
    for key, value in expected.items():
        if context.get(key) != value:
            raise BenchmarkResultsError(
                f"Frontend context {key!r} must be {value!r}; "
                f"found {context.get(key)!r}."
            )
    mode = context.get("ulog_mode")
    if not isinstance(mode, str) or mode not in MODE_REPETITIONS:
        raise BenchmarkResultsError(
            "Frontend context 'ulog_mode' must be 'smoke' or 'controlled'."
        )
    repetitions = MODE_REPETITIONS[mode]
    if context.get("ulog_repetitions") != str(repetitions):
        raise BenchmarkResultsError(
            f"Frontend context 'ulog_repetitions' must be {repetitions} "
            f"for mode {mode!r}."
        )
    return mode, repetitions


def expected_identities(repetitions: int) -> list[str]:
    return [
        f"UlogFrontend/{CANDIDATE}/path:{path}/repetition:{repetition}"
        for repetition in range(repetitions)
        for path in PATHS
    ]


def expected_attempts(mode: str) -> tuple[int, int]:
    if mode == "smoke":
        return SMOKE_WARMUP_ATTEMPTS, SMOKE_MEASURED_ATTEMPTS
    return CONTROLLED_WARMUP_ATTEMPTS, CONTROLLED_MEASURED_ATTEMPTS


def validate_row(row: object, row_index: int, mode: str, repetitions: int) -> str:
    if not isinstance(row, dict):
        raise BenchmarkResultsError(
            f"Frontend benchmark row {row_index} must be an object."
        )
    name = row.get("name")
    if not isinstance(name, str):
        raise BenchmarkResultsError(
            f"Frontend benchmark row {row_index} must contain a string name."
        )
    match = ROW_PATTERN.fullmatch(name)
    if not match:
        raise BenchmarkResultsError(
            f"Frontend row name {name!r} does not match the maintained paths."
        )
    if row.get("error_occurred", False) is not False:
        raise BenchmarkResultsError(f"Frontend row {name!r} reports a benchmark error.")
    iterations = row.get("iterations")
    if (
        row.get("run_type") != "iteration"
        or not isinstance(iterations, int)
        or isinstance(iterations, bool)
        or iterations != 1
    ):
        raise BenchmarkResultsError(
            f"Frontend row {name!r} must be a single benchmark iteration."
        )
    missing = sorted(REQUIRED_COUNTERS - row.keys())
    if missing:
        raise BenchmarkResultsError(
            f"Frontend row {name!r} is missing counters: {', '.join(missing)}."
        )
    counters = {field: require_integer(row, field, name) for field in INTEGER_COUNTERS}
    numbers = {
        field: require_number(row, field, name) for field in NONNEGATIVE_COUNTERS
    }

    path = match.group("path")
    repetition = int(match.group("repetition"))
    if repetition >= repetitions or counters["workload_repetition_index"] != repetition:
        raise BenchmarkResultsError(
            f"Frontend row {name!r} has an invalid workload repetition index."
        )
    warmup_attempts, measured_attempts = expected_attempts(mode)
    exact = {
        "warmup_attempts": warmup_attempts,
        "measured_attempts": measured_attempts,
        "sample_count": measured_attempts,
        "attempted_records": measured_attempts,
        "accepted_records": measured_attempts if path == "ordinary-accepted" else 0,
        "rejected_records": 0 if path == "ordinary-accepted" else measured_attempts,
        "compile_erased_records": measured_attempts if path == "compile-erased" else 0,
        "runtime_filtered_records": measured_attempts if path == "runtime-filtered" else 0,
        "null_logger_records": measured_attempts if path == "null-logger" else 0,
        "admission_rejected_records": (
            measured_attempts if path == "admission-rejected" else 0
        ),
        "message_evaluation_count": (
            measured_attempts if path == "ordinary-accepted" else 0
        ),
        "default_logger_load_count": 0 if path == "compile-erased" else measured_attempts,
        "accepted_bytes": (
            measured_attempts * MESSAGE_BYTES if path == "ordinary-accepted" else 0
        ),
        "allocation_count": 0,
        "allocation_failure_count": 0,
        "logical_retained_final_bytes": 0,
        "physical_retained_final_bytes": 0,
        "accounting_error_count": 0,
        "retained_bound_error_count": 0,
    }
    exact["accepted_latency_sample_count"] = exact["accepted_records"]
    exact["rejected_latency_sample_count"] = exact["rejected_records"]
    for field, expected in exact.items():
        if counters[field] != expected:
            raise BenchmarkResultsError(
                f"Frontend row {name!r} counter {field!r} must be {expected}; "
                f"found {counters[field]}."
            )

    require_latency_summary(
        name,
        "producer",
        counters["sample_count"],
        numbers["producer_latency_p50_ns"],
        numbers["producer_latency_p99_ns"],
        numbers["producer_latency_p999_ns"],
        row_kind="Frontend",
    )
    for prefix in ("accepted", "rejected"):
        require_latency_summary(
            name,
            prefix,
            counters[f"{prefix}_latency_sample_count"],
            numbers[f"{prefix}_latency_p50_ns"],
            numbers[f"{prefix}_latency_p99_ns"],
            numbers[f"{prefix}_latency_p999_ns"],
            row_kind="Frontend",
        )

    wall_time = numbers["wall_time_ns"]
    if wall_time == 0:
        raise BenchmarkResultsError(
            f"Frontend row {name!r} counter 'wall_time_ns' must be positive."
        )
    rate_scale = 1_000_000_000.0 / wall_time
    require_close(
        name,
        "attempts_per_second",
        numbers["attempts_per_second"],
        measured_attempts * rate_scale,
    )
    require_close(
        name,
        "records_per_second",
        numbers["records_per_second"],
        exact["accepted_records"] * rate_scale,
    )
    require_close(
        name,
        "bytes_per_second",
        numbers["bytes_per_second"],
        exact["accepted_bytes"] * rate_scale,
    )
    require_close(
        name,
        "cpu_utilization_percent",
        numbers["cpu_utilization_percent"],
        numbers["process_cpu_time_ns"] * 100.0 / wall_time,
    )
    return match.group("identity")


def validate_result_file(path: Path) -> tuple[int, int, int]:
    document = read_result_file(path)
    mode, repetitions = require_context(document)
    rows = document.get("benchmarks")
    if not isinstance(rows, list):
        raise BenchmarkResultsError("Frontend result must contain a benchmark row list.")
    identities = [
        validate_row(row, row_index, mode, repetitions)
        for row_index, row in enumerate(rows)
    ]
    expected = expected_identities(repetitions)
    if identities != expected:
        raise BenchmarkResultsError(
            "Frontend rows are missing, duplicated, or out of repetition-major order."
        )
    return len(rows), 1, repetitions


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate deterministic producer-frontend benchmark results."
    )
    parser.add_argument("command", choices=("validate",))
    parser.add_argument("result", type=Path)
    arguments = parser.parse_args()
    try:
        rows, candidates, repetitions = validate_result_file(arguments.result)
    except BenchmarkResultsError as error:
        print(f"frontend result validation failed: {error}", file=sys.stderr)
        return 1
    print(
        f"Validated {rows} frontend row(s) for {candidates} candidate(s) "
        f"and {repetitions} repetition(s)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
