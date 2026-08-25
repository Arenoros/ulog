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


RESULT_PROTOCOL = "ulog-composed-producer-results/1"
CANDIDATE = "composed-producer"
PRODUCER_COUNTS = (1, 2, 4, 8, 16, 32)
RECORD_SIZES = (64, 256, 1024, 4096, 16384)
OCCUPANCIES = ("empty", "partial", "near-full", "saturated")
MODE_REPETITIONS = {"smoke": 1, "controlled": 7}
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
CONTIGUOUS_QUANTUM_BYTES = 64
OCCUPANCY_BYTES = {
    "empty": 0,
    "partial": 524_288,
    "near-full": 1_032_192,
    "saturated": CAPACITY_BYTES,
}
ROW_PATTERN = re.compile(
    r"^(?P<identity>UlogComposedProducer/composed-producer/"
    r"producers:(?P<producers>1|2|4|8|16|32)/"
    r"record_bytes:(?P<record_size>64|256|1024|4096|16384)/"
    r"occupancy:(?P<occupancy>empty|partial|near-full|saturated)/"
    r"repetition:(?P<repetition>[0-6]))(?:/.+)?$"
)
REQUIRED_INTEGER_COUNTERS = {
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
    "maximum_accepted_per_round",
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
    "message_callback_count",
    "context_callback_count",
    "fifo_error_count",
    "record_validation_error_count",
    "publication_error_count",
    "lifecycle_error_count",
    "topology_attempted_records",
    "topology_enqueued_records",
    "topology_dequeued_records",
    "topology_rejected_records",
    "topology_full_rejections",
    "topology_contention_rejections",
    "topology_invalid_rejections",
    "topology_retained_records",
    "topology_retained_serialized_bytes",
    "topology_retained_charge_bytes",
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
REQUIRED_COUNTERS = REQUIRED_INTEGER_COUNTERS | NONNEGATIVE_COUNTERS


def require_context(document: dict[str, object]) -> tuple[str, int]:
    context = document.get("context")
    if not isinstance(context, dict):
        raise BenchmarkResultsError(
            "Composed-producer result must contain a Google Benchmark context object."
        )
    expected = {
        "ulog_result_protocol": RESULT_PROTOCOL,
        "ulog_candidates": CANDIDATE,
        "ulog_candidate_schedule": "single-candidate",
        "ulog_timing_policy": "advisory",
    }
    for key, value in expected.items():
        if context.get(key) != value:
            raise BenchmarkResultsError(
                f"Composed-producer context {key!r} must be {value!r}; "
                f"found {context.get(key)!r}."
            )
    mode = context.get("ulog_mode")
    if not isinstance(mode, str) or mode not in MODE_REPETITIONS:
        raise BenchmarkResultsError(
            "Composed-producer context 'ulog_mode' must be 'smoke' or 'controlled'."
        )
    repetitions = MODE_REPETITIONS[mode]
    if context.get("ulog_repetitions") != str(repetitions):
        raise BenchmarkResultsError(
            f"Composed-producer context 'ulog_repetitions' must be {repetitions} "
            f"for mode {mode!r}."
        )
    return mode, repetitions


def expected_measured_rounds(mode: str, producers: int) -> int:
    if mode == "smoke":
        return 1
    return max(64, (100_000 + producers - 1) // producers)


def round_up(value: int, quantum: int) -> int:
    return ((value + quantum - 1) // quantum) * quantum


def expected_identities(repetitions: int) -> list[str]:
    return [
        f"UlogComposedProducer/{CANDIDATE}/producers:{producers}/"
        f"record_bytes:{record_size}/occupancy:{occupancy}/repetition:{repetition}"
        for repetition in range(repetitions)
        for producers in PRODUCER_COUNTS
        for record_size in RECORD_SIZES
        for occupancy in OCCUPANCIES
    ]


def validate_row(row: object, row_index: int, mode: str) -> str:
    if not isinstance(row, dict):
        raise BenchmarkResultsError(
            f"Composed-producer benchmark row {row_index} must be an object."
        )
    name = row.get("name")
    if not isinstance(name, str):
        raise BenchmarkResultsError(
            f"Composed-producer benchmark row {row_index} has no string name."
        )
    match = ROW_PATTERN.fullmatch(name)
    if not match:
        raise BenchmarkResultsError(
            f"Composed-producer row name {name!r} does not match the maintained matrix."
        )
    if row.get("error_occurred", False) is not False:
        raise BenchmarkResultsError(
            f"Composed-producer row {name!r} reports a benchmark error."
        )
    iterations = row.get("iterations")
    if (
        row.get("run_type") != "iteration"
        or not isinstance(iterations, int)
        or isinstance(iterations, bool)
        or iterations != 1
    ):
        raise BenchmarkResultsError(
            f"Composed-producer row {name!r} must be a single benchmark iteration."
        )
    missing = sorted(REQUIRED_COUNTERS - row.keys())
    if missing:
        raise BenchmarkResultsError(
            f"Composed-producer row {name!r} is missing counters: {', '.join(missing)}."
        )
    counters = {
        field: require_integer(row, field, name) for field in REQUIRED_INTEGER_COUNTERS
    }
    numbers = {
        field: require_number(row, field, name) for field in NONNEGATIVE_COUNTERS
    }
    producers = int(match.group("producers"))
    record_size = int(match.group("record_size"))
    occupancy = match.group("occupancy")
    repetition = int(match.group("repetition"))
    measured_rounds = expected_measured_rounds(mode, producers)
    warmup_rounds = 1 if mode == "smoke" else 64
    expected_dimensions = {
        "producer_count": producers,
        "record_size_bytes": record_size,
        "workload_repetition_index": repetition,
        "warmup_rounds": warmup_rounds,
        "measured_rounds": measured_rounds,
    }
    for field, expected in expected_dimensions.items():
        if counters[field] != expected:
            raise BenchmarkResultsError(
                f"Composed-producer row {name!r} counter {field!r} must be "
                f"{expected}; found {counters[field]}."
            )

    expected_stored = min(record_size, MAXIMUM_STORED_MESSAGE_BYTES)
    expected_owned_payload = expected_stored + CANONICAL_FIXED_PAYLOAD_BYTES
    expected_serialized = expected_owned_payload + CANONICAL_METADATA_BYTES
    expected_charge = max(
        CONTIGUOUS_QUANTUM_BYTES,
        round_up(expected_serialized, CONTIGUOUS_QUANTUM_BYTES),
    )
    expected_truncated = int(expected_stored < record_size)
    footprint = {
        "requested_message_bytes": record_size,
        "stored_message_bytes from the canonical Record recipe": expected_stored,
        "owned_payload_bytes from the canonical Record recipe": expected_owned_payload,
        "metadata_bytes from the canonical Record recipe": CANONICAL_METADATA_BYTES,
        "serialized_bytes from the canonical Record recipe": expected_serialized,
        "fragmentation_bytes from the contiguous Record policy": (
            expected_charge - expected_serialized
        ),
        "accounting_charge_bytes from the contiguous Record policy": expected_charge,
        "minimum_accounting_charge_bytes": CONTIGUOUS_QUANTUM_BYTES,
        "record_truncated": expected_truncated,
    }
    footprint_actual = {
        "requested_message_bytes": counters["requested_message_bytes"],
        "stored_message_bytes from the canonical Record recipe": counters[
            "stored_message_bytes"
        ],
        "owned_payload_bytes from the canonical Record recipe": counters[
            "owned_payload_bytes"
        ],
        "metadata_bytes from the canonical Record recipe": counters["metadata_bytes"],
        "serialized_bytes from the canonical Record recipe": counters[
            "serialized_bytes"
        ],
        "fragmentation_bytes from the contiguous Record policy": counters[
            "fragmentation_bytes"
        ],
        "accounting_charge_bytes from the contiguous Record policy": counters[
            "accounting_charge_bytes"
        ],
        "minimum_accounting_charge_bytes": counters[
            "minimum_accounting_charge_bytes"
        ],
        "record_truncated": counters["record_truncated"],
    }
    for description, expected in footprint.items():
        require_equal(name, description, footprint_actual[description], expected)

    attempted = producers * measured_rounds
    initial = OCCUPANCY_BYTES[occupancy]
    accepted_per_round = min(
        producers,
        (CAPACITY_BYTES - initial) // expected_charge,
    )
    accepted = accepted_per_round * measured_rounds
    rejected = attempted - accepted
    exact = {
        "attempted_records": attempted,
        "accepted_records": accepted,
        "rejected_records": rejected,
        "maximum_accepted_per_round": accepted_per_round,
        "sample_count": attempted,
        "accepted_latency_sample_count": accepted,
        "rejected_latency_sample_count": rejected,
        "accepted_bytes": accepted * record_size,
        "rejected_bytes": rejected * record_size,
        "truncated_records": accepted * expected_truncated,
        "message_callback_count": accepted,
        "context_callback_count": accepted,
        "topology_attempted_records": accepted,
        "topology_enqueued_records": accepted,
        "topology_dequeued_records": accepted,
        "topology_rejected_records": 0,
        "topology_full_rejections": 0,
        "topology_contention_rejections": 0,
        "topology_invalid_rejections": 0,
        "topology_retained_records": 0,
        "topology_retained_serialized_bytes": 0,
        "topology_retained_charge_bytes": 0,
        "allocation_count": 0,
        "allocation_failure_count": 0,
        "accounting_error_count": 0,
        "retained_bound_error_count": 0,
        "fifo_error_count": 0,
        "record_validation_error_count": 0,
        "publication_error_count": 0,
        "lifecycle_error_count": 0,
        "logical_retained_initial_bytes": initial,
        "logical_retained_high_water_bytes": (
            initial + accepted_per_round * expected_serialized
        ),
        "logical_retained_final_bytes": initial,
        "logical_retained_limit_bytes": CAPACITY_BYTES,
        "physical_retained_initial_bytes": (
            initial + accepted_per_round * expected_charge
        ),
        "physical_retained_high_water_bytes": (
            initial + accepted_per_round * expected_charge
        ),
        "physical_retained_final_bytes": (
            initial + accepted_per_round * expected_charge
        ),
        "physical_retained_limit_bytes": CAPACITY_BYTES,
    }
    for field, expected in exact.items():
        if counters[field] != expected:
            raise BenchmarkResultsError(
                f"Composed-producer row {name!r} counter {field!r} must be "
                f"{expected}; found {counters[field]}."
            )

    retained_invariants = (
        counters["logical_retained_high_water_bytes"]
        <= counters["logical_retained_limit_bytes"]
        and counters["physical_retained_high_water_bytes"]
        <= counters["physical_retained_limit_bytes"]
    )
    if not retained_invariants:
        raise BenchmarkResultsError(
            f"Composed-producer row {name!r} violates retained baseline or bounds."
        )

    require_latency_summary(
        name,
        "producer",
        counters["sample_count"],
        numbers["producer_latency_p50_ns"],
        numbers["producer_latency_p99_ns"],
        numbers["producer_latency_p999_ns"],
        row_kind="Composed-producer",
    )
    require_latency_summary(
        name,
        "accepted",
        counters["accepted_latency_sample_count"],
        numbers["accepted_latency_p50_ns"],
        numbers["accepted_latency_p99_ns"],
        numbers["accepted_latency_p999_ns"],
        row_kind="Composed-producer",
    )
    require_latency_summary(
        name,
        "rejected",
        counters["rejected_latency_sample_count"],
        numbers["rejected_latency_p50_ns"],
        numbers["rejected_latency_p99_ns"],
        numbers["rejected_latency_p999_ns"],
        row_kind="Composed-producer",
    )

    wall_time = numbers["wall_time_ns"]
    if wall_time == 0:
        raise BenchmarkResultsError(
            f"Composed-producer row '{name}' counter 'wall_time_ns' must be positive."
        )
    rate_scale = 1_000_000_000.0 / wall_time
    require_close(
        name,
        "attempts_per_second",
        numbers["attempts_per_second"],
        attempted * rate_scale,
    )
    require_close(
        name,
        "records_per_second",
        numbers["records_per_second"],
        accepted * rate_scale,
    )
    require_close(
        name,
        "bytes_per_second",
        numbers["bytes_per_second"],
        accepted * record_size * rate_scale,
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
        raise BenchmarkResultsError(
            "Composed-producer result must contain a benchmark row list."
        )
    identities = [validate_row(row, index, mode) for index, row in enumerate(rows)]
    expected = expected_identities(repetitions)
    if identities != expected:
        raise BenchmarkResultsError(
            "Composed-producer rows are missing, duplicated, or out of matrix order."
        )
    return len(rows), 1, repetitions


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate deterministic composed-producer benchmark results."
    )
    parser.add_argument("command", choices=("validate",))
    parser.add_argument("result", type=Path)
    arguments = parser.parse_args()
    try:
        rows, candidates, repetitions = validate_result_file(arguments.result)
    except BenchmarkResultsError as error:
        print(f"composed-producer result validation failed: {error}", file=sys.stderr)
        return 1
    print(
        f"Validated {rows} composed-producer row(s) for {candidates} candidate(s) "
        f"and {repetitions} repetition(s)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
