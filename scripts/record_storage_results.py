#!/usr/bin/env python3

import argparse
import re
import sys
from pathlib import Path

from benchmark_results import (
    BenchmarkResultsError,
    read_result_file,
    require_close,
    require_equal,
    require_integer,
    require_latency_summary,
    require_number,
)


RESULT_PROTOCOL = "ulog-record-storage-results/2"
TIMING_POLICY = "advisory"
EXPECTED_CANDIDATES = (
    "contiguous-record",
    "chunked-record",
    "hybrid-record",
)
EXPECTED_CANDIDATE_DECLARATION = ",".join(EXPECTED_CANDIDATES)
CANDIDATE_SCHEDULE = "six-permutation-cycle"
CANDIDATE_ORDERS = (
    EXPECTED_CANDIDATES,
    (EXPECTED_CANDIDATES[0], EXPECTED_CANDIDATES[2], EXPECTED_CANDIDATES[1]),
    (EXPECTED_CANDIDATES[1], EXPECTED_CANDIDATES[0], EXPECTED_CANDIDATES[2]),
    (EXPECTED_CANDIDATES[1], EXPECTED_CANDIDATES[2], EXPECTED_CANDIDATES[0]),
    (EXPECTED_CANDIDATES[2], EXPECTED_CANDIDATES[0], EXPECTED_CANDIDATES[1]),
    (EXPECTED_CANDIDATES[2], EXPECTED_CANDIDATES[1], EXPECTED_CANDIDATES[0]),
)
MODE_REPETITIONS = {"controlled": 7, "smoke": 1}
MODE_WARMUP_ROUNDS = {"controlled": 64, "smoke": 1}
SMOKE_MEASURED_ROUNDS = 1
CONTROLLED_MINIMUM_MEASURED_ROUNDS = 64
CONTROLLED_MINIMUM_SAMPLES_PER_CELL = 100_000
PRODUCER_COUNTS = (1, 2, 4, 8, 16, 32)
RECORD_SIZES = (64, 256, 1024, 4096, 16384)
CAPACITY_BYTES = 1_048_576
RECORD_METADATA_BYTES = 48
FIELD_METADATA_BYTES = 24
CANONICAL_FIELD_COUNT = 6
CANONICAL_FIXED_PAYLOAD_BYTES = 96
CANONICAL_METADATA_BYTES = (
    RECORD_METADATA_BYTES + CANONICAL_FIELD_COUNT * FIELD_METADATA_BYTES
)
MAXIMUM_SERIALIZED_BYTES = 16_384
MAXIMUM_STORED_MESSAGE_BYTES = (
    MAXIMUM_SERIALIZED_BYTES
    - CANONICAL_FIXED_PAYLOAD_BYTES
    - CANONICAL_METADATA_BYTES
)
OCCUPANCY_BYTES = {
    "empty": 0,
    "partial": 524_288,
    "near-full": 1_032_192,
    "saturated": CAPACITY_BYTES,
}
ROW_PATTERN = re.compile(
    r"^(?P<identity>UlogRecordStorage/"
    r"(?P<candidate>contiguous-record|chunked-record|hybrid-record)/"
    r"producers:(?P<producers>1|2|4|8|16|32)/"
    r"record_bytes:(?P<record_size>64|256|1024|4096|16384)/"
    r"occupancy:(?P<occupancy>empty|partial|near-full|saturated)/"
    r"repetition:(?P<repetition>[0-6]))(?:/.+)?$"
)

INTEGER_COUNTERS = {
    "producer_count",
    "record_size_bytes",
    "workload_repetition_index",
    "warmup_rounds",
    "measured_rounds",
    "sample_count",
    "accepted_latency_sample_count",
    "rejected_latency_sample_count",
    "attempted_records",
    "accepted_records",
    "rejected_records",
    "accepted_bytes",
    "rejected_bytes",
    "allocation_count",
    "allocation_failure_count",
    "requested_message_bytes",
    "stored_message_bytes",
    "owned_payload_bytes",
    "metadata_bytes",
    "serialized_bytes",
    "fragmentation_bytes",
    "accounting_charge_bytes",
    "minimum_accounting_charge_bytes",
    "record_truncated",
    "truncated_records",
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
    "record_validation_error_count",
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
LOGICAL_METRIC_FIELDS = (
    "requested_message_bytes",
    "stored_message_bytes",
    "owned_payload_bytes",
    "metadata_bytes",
    "serialized_bytes",
    "record_truncated",
)


def require_context(document: dict[str, object]) -> tuple[str, int]:
    context = document.get("context")
    if not isinstance(context, dict):
        raise BenchmarkResultsError(
            "Benchmark result must contain a Google Benchmark 'context' object."
        )

    expected_values = {
        "ulog_result_protocol": RESULT_PROTOCOL,
        "ulog_candidates": EXPECTED_CANDIDATE_DECLARATION,
        "ulog_candidate_schedule": CANDIDATE_SCHEDULE,
        "ulog_timing_policy": TIMING_POLICY,
    }
    for field, expected in expected_values.items():
        actual = context.get(field)
        if actual != expected:
            raise BenchmarkResultsError(
                f"Benchmark context {field!r} must be {expected!r}; found "
                f"{actual!r}. Configure the Record-storage benchmark context and retry."
            )

    mode = context.get("ulog_mode")
    if not isinstance(mode, str) or mode not in MODE_REPETITIONS:
        raise BenchmarkResultsError(
            "Benchmark context 'ulog_mode' must be 'smoke' or 'controlled'; "
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
            f"Benchmark context 'ulog_repetitions' must be {expected_repetitions} "
            f"for {mode!r} mode; found {repetitions_value!r}."
        )
    return mode, expected_repetitions


def expected_measured_rounds(mode: str, producers: int) -> int:
    if mode == "smoke":
        return SMOKE_MEASURED_ROUNDS
    return max(
        CONTROLLED_MINIMUM_MEASURED_ROUNDS,
        (CONTROLLED_MINIMUM_SAMPLES_PER_CELL + producers - 1) // producers,
    )


def round_up(value: int, quantum: int) -> int:
    return ((value + quantum - 1) // quantum) * quantum


def expected_charge(candidate: str, serialized_bytes: int) -> tuple[int, int]:
    if candidate == "contiguous-record":
        minimum = 64
        return max(minimum, round_up(serialized_bytes, minimum)), minimum
    if candidate == "chunked-record":
        minimum = 256
        return max(minimum, round_up(serialized_bytes, minimum)), minimum

    minimum = 512
    if serialized_bytes <= minimum:
        return minimum, minimum
    if serialized_bytes <= 15_872:
        overflow_bytes = serialized_bytes - minimum
        return minimum + round_up(overflow_bytes, 1_024), minimum
    return 16_384, minimum


def validate_row(
    row: object, row_index: int, mode: str, repetitions: int
) -> tuple[tuple[str, int, int, str, int], tuple[int, ...]]:
    if not isinstance(row, dict):
        raise BenchmarkResultsError(f"Benchmark row {row_index} must be a JSON object.")
    row_name = row.get("name")
    if not isinstance(row_name, str):
        raise BenchmarkResultsError(
            f"Benchmark row {row_index} must contain a string 'name'."
        )
    if row.get("error_occurred", False) is not False:
        raise BenchmarkResultsError(
            f"Record-storage row '{row_name}' has Google Benchmark "
            "'error_occurred' set. Fix the failure and regenerate the result."
        )
    iterations = row.get("iterations")
    if isinstance(iterations, bool) or iterations != 1:
        raise BenchmarkResultsError(
            f"Record-storage row '{row_name}' field 'iterations' must be integer 1; "
            f"found {iterations!r}."
        )
    if row.get("run_type") != "iteration":
        raise BenchmarkResultsError(
            f"Record-storage row '{row_name}' field 'run_type' must be 'iteration'; "
            f"found {row.get('run_type')!r}."
        )

    match = ROW_PATTERN.fullmatch(row_name)
    if not match:
        raise BenchmarkResultsError(
            f"Benchmark row name '{row_name}' does not match the required "
            "UlogRecordStorage/<candidate>/producers:<count>/record_bytes:<bytes>/"
            "occupancy:<state>/repetition:<index> format."
        )
    missing = sorted(REQUIRED_COUNTERS - row.keys())
    if missing:
        raise BenchmarkResultsError(
            f"Record-storage row '{row_name}' is missing required counters: "
            f"{', '.join(missing)}. Emit every {RESULT_PROTOCOL} counter."
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
            f"Record-storage row '{row_name}' repetition index {repetition} is "
            f"outside context range 0..{repetitions - 1}."
        )

    dimensions = {
        "producer_count": producers,
        "record_size_bytes": record_size,
        "workload_repetition_index": repetition,
        "warmup_rounds": MODE_WARMUP_ROUNDS[mode],
        "measured_rounds": expected_measured_rounds(mode, producers),
    }
    for field, expected in dimensions.items():
        require_equal(row_name, field, integers[field], expected)

    requested = integers["requested_message_bytes"]
    stored = integers["stored_message_bytes"]
    owned_payload = integers["owned_payload_bytes"]
    metadata = integers["metadata_bytes"]
    serialized = integers["serialized_bytes"]
    fragmentation = integers["fragmentation_bytes"]
    charge = integers["accounting_charge_bytes"]
    truncated = integers["record_truncated"]

    require_equal(row_name, "requested_message_bytes", requested, record_size)
    expected_stored = min(requested, MAXIMUM_STORED_MESSAGE_BYTES)
    expected_owned_payload = expected_stored + CANONICAL_FIXED_PAYLOAD_BYTES
    require_equal(
        row_name,
        "stored_message_bytes from the canonical Record recipe",
        stored,
        expected_stored,
    )
    require_equal(
        row_name,
        "owned_payload_bytes from the canonical Record recipe",
        owned_payload,
        expected_owned_payload,
    )
    require_equal(
        row_name,
        "metadata_bytes from the canonical Record recipe",
        metadata,
        CANONICAL_METADATA_BYTES,
    )
    if stored > requested:
        raise BenchmarkResultsError(
            f"Record-storage row '{row_name}' stored_message_bytes {stored} exceeds "
            f"requested_message_bytes {requested}."
        )
    if owned_payload < stored:
        raise BenchmarkResultsError(
            f"Record-storage row '{row_name}' owned_payload_bytes {owned_payload} "
            f"do not cover stored_message_bytes {stored}."
        )
    require_equal(
        row_name,
        "serialized_bytes from owned_payload_bytes + metadata_bytes",
        serialized,
        owned_payload + metadata,
    )
    require_equal(
        row_name,
        "accounting_charge_bytes from serialized_bytes + fragmentation_bytes",
        charge,
        serialized + fragmentation,
    )
    if truncated not in (0, 1):
        raise BenchmarkResultsError(
            f"Record-storage row '{row_name}' record_truncated must be 0 or 1; "
            f"found {truncated}."
        )
    require_equal(
        row_name,
        "record_truncated consistency",
        truncated,
        int(stored < requested),
    )
    expected_candidate_charge, expected_minimum = expected_charge(candidate, serialized)
    require_equal(
        row_name,
        f"{candidate} charge formula",
        charge,
        expected_candidate_charge,
    )
    require_equal(
        row_name,
        "minimum_accounting_charge_bytes",
        integers["minimum_accounting_charge_bytes"],
        expected_minimum,
    )

    measured_round_count = dimensions["measured_rounds"]
    attempted = producers * measured_round_count
    initial = OCCUPANCY_BYTES[occupancy]
    accepted_per_round = min(producers, (CAPACITY_BYTES - initial) // charge)
    accepted = accepted_per_round * measured_round_count
    rejected = attempted - accepted
    exact = {
        "sample_count": attempted,
        "accepted_latency_sample_count": accepted,
        "rejected_latency_sample_count": rejected,
        "attempted_records": attempted,
        "accepted_records": accepted,
        "rejected_records": rejected,
        "accepted_bytes": accepted * record_size,
        "rejected_bytes": rejected * record_size,
        "allocation_count": 0,
        "allocation_failure_count": 0,
        "truncated_records": accepted * truncated,
        "logical_retained_initial_bytes": initial,
        "logical_retained_high_water_bytes": initial + accepted_per_round * serialized,
        "logical_retained_final_bytes": initial,
        "logical_retained_limit_bytes": CAPACITY_BYTES,
        "physical_retained_initial_bytes": initial,
        "physical_retained_high_water_bytes": initial + accepted_per_round * charge,
        "physical_retained_final_bytes": initial,
        "physical_retained_limit_bytes": CAPACITY_BYTES,
        "accounting_error_count": 0,
        "retained_bound_error_count": 0,
        "record_validation_error_count": 0,
    }
    for field, expected in exact.items():
        require_equal(row_name, field, integers[field], expected)

    for stage in ("initial", "high_water", "final", "limit"):
        logical = integers[f"logical_retained_{stage}_bytes"]
        physical = integers[f"physical_retained_{stage}_bytes"]
        if physical < logical:
            raise BenchmarkResultsError(
                f"Record-storage row '{row_name}' physical retained {stage} bytes "
                f"{physical} are below logical retained {stage} bytes {logical}."
            )

    require_latency_summary(
        row_name,
        "producer",
        integers["sample_count"],
        numbers["producer_latency_p50_ns"],
        numbers["producer_latency_p99_ns"],
        numbers["producer_latency_p999_ns"],
        row_kind="Record-storage",
    )
    require_latency_summary(
        row_name,
        "accepted",
        integers["accepted_latency_sample_count"],
        numbers["accepted_latency_p50_ns"],
        numbers["accepted_latency_p99_ns"],
        numbers["accepted_latency_p999_ns"],
        row_kind="Record-storage",
    )
    require_latency_summary(
        row_name,
        "rejected",
        integers["rejected_latency_sample_count"],
        numbers["rejected_latency_p50_ns"],
        numbers["rejected_latency_p99_ns"],
        numbers["rejected_latency_p999_ns"],
        row_kind="Record-storage",
    )

    wall_time = numbers["wall_time_ns"]
    if wall_time == 0:
        raise BenchmarkResultsError(
            f"Record-storage row '{row_name}' counter 'wall_time_ns' must be positive."
        )
    rate_scale = 1_000_000_000.0 / wall_time
    require_close(
        row_name,
        "attempts_per_second",
        numbers["attempts_per_second"],
        attempted * rate_scale,
    )
    require_close(
        row_name,
        "records_per_second",
        numbers["records_per_second"],
        accepted * rate_scale,
    )
    require_close(
        row_name,
        "bytes_per_second",
        numbers["bytes_per_second"],
        accepted * record_size * rate_scale,
    )
    require_close(
        row_name,
        "cpu_utilization_percent",
        numbers["cpu_utilization_percent"],
        numbers["process_cpu_time_ns"] * 100.0 / wall_time,
    )

    identity = (candidate, producers, record_size, occupancy, repetition)
    logical_metrics = tuple(integers[field] for field in LOGICAL_METRIC_FIELDS)
    return identity, logical_metrics


def validate_candidate_schedule(
    ordered_rows: list[tuple[tuple[str, int, int, str, int], tuple[int, ...]]]
) -> None:
    candidate_count = len(EXPECTED_CANDIDATES)
    if len(ordered_rows) % candidate_count != 0:
        raise BenchmarkResultsError(
            "Benchmark candidate rows must form complete adjacent matrix-cell groups."
        )
    for group_start in range(0, len(ordered_rows), candidate_count):
        group = ordered_rows[group_start : group_start + candidate_count]
        identities = [entry[0] for entry in group]
        first = identities[0]
        if any(identity[1:] != first[1:] for identity in identities[1:]):
            raise BenchmarkResultsError(
                f"Benchmark rows {group_start}..{group_start + candidate_count - 1} "
                "must describe adjacent candidates for the same matrix cell."
            )
        expected_order = CANDIDATE_ORDERS[first[4] % len(CANDIDATE_ORDERS)]
        actual_order = tuple(identity[0] for identity in identities)
        if actual_order != expected_order:
            raise BenchmarkResultsError(
                f"Benchmark rows {group_start}..{group_start + candidate_count - 1} "
                f"have candidate order {actual_order!r}; expected {expected_order!r} "
                f"for repetition {first[4]} in the six-permutation cycle."
            )
        first_metrics = group[0][1]
        if any(entry[1] != first_metrics for entry in group[1:]):
            raise BenchmarkResultsError(
                f"Benchmark rows {group_start}..{group_start + candidate_count - 1} "
                "must report identical same-cell logical Record metrics across candidates."
            )


def validate_document(document: dict[str, object]) -> tuple[int, int, int]:
    mode, repetitions = require_context(document)
    rows = document.get("benchmarks")
    if not isinstance(rows, list) or not rows:
        raise BenchmarkResultsError(
            "Benchmark result must contain a non-empty Google Benchmark "
            "'benchmarks' array."
        )

    observed: set[tuple[str, int, int, str, int]] = set()
    ordered_rows = []
    for row_index, row in enumerate(rows):
        validated = validate_row(row, row_index, mode, repetitions)
        identity = validated[0]
        if identity in observed:
            raise BenchmarkResultsError(
                "Benchmark result contains duplicate record-storage cell "
                f"{identity!r}. Emit every candidate/matrix/repetition cell once."
            )
        observed.add(identity)
        ordered_rows.append(validated)

    expected = {
        (candidate, producers, record_size, occupancy, repetition)
        for repetition in range(repetitions)
        for producers in PRODUCER_COUNTS
        for record_size in RECORD_SIZES
        for occupancy in OCCUPANCY_BYTES
        for candidate in EXPECTED_CANDIDATES
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
                f"unexpected {len(extra)} cell(s), first unexpected cell: {extra[0]!r}"
            )
        raise BenchmarkResultsError(
            "Benchmark record-storage matrix is incomplete: " + "; ".join(details) + "."
        )

    validate_candidate_schedule(ordered_rows)
    return len(rows), len(EXPECTED_CANDIDATES), repetitions


def validate_result_file(path: Path) -> tuple[int, int, int]:
    return validate_document(read_result_file(path))


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate Ulog's structured Record storage benchmark results."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    validate_parser = subparsers.add_parser(
        "validate", help="Validate one record-storage benchmark JSON result."
    )
    validate_parser.add_argument("result", type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        if arguments.command == "validate":
            rows, candidates, repetitions = validate_result_file(arguments.result)
            print(
                f"Validated {rows} record-storage row(s) for {candidates} "
                f"candidate(s) and {repetitions} repetition(s)"
            )
            return 0
    except BenchmarkResultsError as error:
        print(f"record-storage result validation failed: {error}", file=sys.stderr)
        return 1
    raise AssertionError(f"Unhandled command {arguments.command!r}")


if __name__ == "__main__":
    raise SystemExit(main())
