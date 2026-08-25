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


RESULT_PROTOCOL = "ulog-ingress-results/1"
TIMING_POLICY = "advisory"
PUBLICATION_ACTION_UNIT = "bounded topology actions per TryPublish call"
EXPECTED_CANDIDATES = (
    "bounded-mpsc-ring",
    "chunked-mpsc",
    "per-producer-lanes",
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
PUBLICATION_ACTION_LIMITS = {
    "bounded-mpsc-ring": 70,
    "chunked-mpsc": 11,
    "per-producer-lanes": 11,
}
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
CONTIGUOUS_QUANTUM_BYTES = 64
OCCUPANCY_BYTES = {
    "empty": 0,
    "partial": 524_288,
    "near-full": 1_032_192,
    "saturated": CAPACITY_BYTES,
}
ROW_PATTERN = re.compile(
    r"^(?P<identity>UlogIngressTopology/"
    r"(?P<candidate>bounded-mpsc-ring|chunked-mpsc|per-producer-lanes)/"
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
    "fifo_error_count",
    "sequence_error_count",
    "record_validation_error_count",
    "maximum_publication_actions_observed",
    "publication_action_limit",
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
LOGICAL_RECORD_FIELDS = (
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
        "ulog_publication_action_unit": PUBLICATION_ACTION_UNIT,
    }
    for field, expected in expected_values.items():
        actual = context.get(field)
        if actual != expected:
            raise BenchmarkResultsError(
                f"Benchmark context {field!r} must be {expected!r}; found "
                f"{actual!r}. Configure the Ingress benchmark context and retry."
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
            f"Ingress row '{row_name}' has Google Benchmark 'error_occurred' set. "
            "Fix the deterministic failure and regenerate the result."
        )
    iterations = row.get("iterations")
    if isinstance(iterations, bool) or iterations != 1:
        raise BenchmarkResultsError(
            f"Ingress row '{row_name}' field 'iterations' must be integer 1; "
            f"found {iterations!r}."
        )
    if row.get("run_type") != "iteration":
        raise BenchmarkResultsError(
            f"Ingress row '{row_name}' field 'run_type' must be 'iteration'; "
            f"found {row.get('run_type')!r}."
        )

    match = ROW_PATTERN.fullmatch(row_name)
    if not match:
        raise BenchmarkResultsError(
            f"Benchmark row name '{row_name}' does not match the required "
            "UlogIngressTopology/<candidate>/producers:<count>/record_bytes:<bytes>/"
            "occupancy:<state>/repetition:<index> format."
        )
    missing = sorted(REQUIRED_COUNTERS - row.keys())
    if missing:
        raise BenchmarkResultsError(
            f"Ingress row '{row_name}' is missing required counters: "
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
            f"Ingress row '{row_name}' repetition index {repetition} is outside "
            f"context range 0..{repetitions - 1}."
        )

    measured_rounds = expected_measured_rounds(mode, producers)
    dimensions = {
        "producer_count": producers,
        "record_size_bytes": record_size,
        "workload_repetition_index": repetition,
        "warmup_rounds": MODE_WARMUP_ROUNDS[mode],
        "measured_rounds": measured_rounds,
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

    expected_stored = min(record_size, MAXIMUM_STORED_MESSAGE_BYTES)
    expected_owned_payload = expected_stored + CANONICAL_FIXED_PAYLOAD_BYTES
    expected_serialized = expected_owned_payload + CANONICAL_METADATA_BYTES
    expected_charge = max(
        CONTIGUOUS_QUANTUM_BYTES,
        round_up(expected_serialized, CONTIGUOUS_QUANTUM_BYTES),
    )
    footprint_values = {
        "requested_message_bytes": record_size,
        "stored_message_bytes from the canonical Record recipe": expected_stored,
        "owned_payload_bytes from the canonical Record recipe": expected_owned_payload,
        "metadata_bytes from the canonical Record recipe": CANONICAL_METADATA_BYTES,
        "serialized_bytes from the canonical Record recipe": expected_serialized,
        "accounting_charge_bytes from the contiguous Record policy": expected_charge,
        "minimum_accounting_charge_bytes": CONTIGUOUS_QUANTUM_BYTES,
        "record_truncated": int(expected_stored < record_size),
    }
    footprint_actual = {
        "requested_message_bytes": requested,
        "stored_message_bytes from the canonical Record recipe": stored,
        "owned_payload_bytes from the canonical Record recipe": owned_payload,
        "metadata_bytes from the canonical Record recipe": metadata,
        "serialized_bytes from the canonical Record recipe": serialized,
        "accounting_charge_bytes from the contiguous Record policy": charge,
        "minimum_accounting_charge_bytes": integers[
            "minimum_accounting_charge_bytes"
        ],
        "record_truncated": truncated,
    }
    for description, expected in footprint_values.items():
        require_equal(row_name, description, footprint_actual[description], expected)
    require_equal(
        row_name,
        "fragmentation_bytes from accounting charge minus serialized bytes",
        fragmentation,
        charge - serialized,
    )

    attempted = producers * measured_rounds
    initial = OCCUPANCY_BYTES[occupancy]
    capacity_accepted_per_round = min(producers, (CAPACITY_BYTES - initial) // charge)
    accepted = integers["accepted_records"]
    rejected = integers["rejected_records"]
    maximum_accepted_per_round = integers["maximum_accepted_per_round"]
    require_equal(
        row_name,
        "attempted_records from accepted_records + rejected_records",
        attempted,
        accepted + rejected,
    )
    maximum_capacity_accepts = capacity_accepted_per_round * measured_rounds
    if accepted > maximum_capacity_accepts:
        raise BenchmarkResultsError(
            f"Ingress row '{row_name}' accepted_records {accepted} exceed the "
            f"capacity upper bound {maximum_capacity_accepts}. Fix admission or "
            "retained-byte accounting and retry."
        )
    if maximum_accepted_per_round > capacity_accepted_per_round:
        raise BenchmarkResultsError(
            f"Ingress row '{row_name}' maximum_accepted_per_round "
            f"{maximum_accepted_per_round} exceeds the capacity upper bound "
            f"{capacity_accepted_per_round}. Fix the per-round counter and retry."
        )
    if maximum_accepted_per_round > accepted or accepted > (
        maximum_accepted_per_round * measured_rounds
    ):
        raise BenchmarkResultsError(
            f"Ingress row '{row_name}' maximum_accepted_per_round "
            f"{maximum_accepted_per_round} is inconsistent with {accepted} accepted "
            f"Records over {measured_rounds} round(s). Publish the observed per-round "
            "maximum and retry."
        )
    exact_workload = {
        "sample_count": attempted,
        "accepted_latency_sample_count": accepted,
        "rejected_latency_sample_count": rejected,
        "attempted_records": attempted,
        "accepted_bytes": accepted * record_size,
        "rejected_bytes": rejected * record_size,
        "allocation_count": 0,
        "allocation_failure_count": 0,
        "truncated_records": accepted * truncated,
        "logical_retained_initial_bytes": initial,
        "logical_retained_high_water_bytes": initial
        + maximum_accepted_per_round * serialized,
        "logical_retained_final_bytes": initial,
        "logical_retained_limit_bytes": CAPACITY_BYTES,
        "physical_retained_initial_bytes": initial,
        "physical_retained_high_water_bytes": initial
        + maximum_accepted_per_round * charge,
        "physical_retained_final_bytes": initial,
        "physical_retained_limit_bytes": CAPACITY_BYTES,
        "accounting_error_count": 0,
        "retained_bound_error_count": 0,
    }
    for field, expected in exact_workload.items():
        require_equal(row_name, field, integers[field], expected)

    topology_attempted = integers["topology_attempted_records"]
    topology_enqueued = integers["topology_enqueued_records"]
    topology_dequeued = integers["topology_dequeued_records"]
    topology_rejected = integers["topology_rejected_records"]
    require_equal(
        row_name,
        "topology_attempted_records from topology_enqueued_records + "
        "topology_rejected_records",
        topology_attempted,
        topology_enqueued + topology_rejected,
    )
    require_equal(
        row_name,
        "topology_enqueued_records from accepted_records",
        topology_enqueued,
        accepted,
    )
    require_equal(
        row_name,
        "topology_dequeued_records from enqueued_records",
        topology_dequeued,
        topology_enqueued,
    )
    rejection_categories = (
        integers["topology_full_rejections"]
        + integers["topology_contention_rejections"]
        + integers["topology_invalid_rejections"]
    )
    require_equal(
        row_name,
        "topology_rejected_records from topology_full_rejections + "
        "topology_contention_rejections + topology_invalid_rejections",
        topology_rejected,
        rejection_categories,
    )
    if topology_attempted > attempted:
        raise BenchmarkResultsError(
            f"Ingress row '{row_name}' topology_attempted_records "
            f"{topology_attempted} exceed workload attempted_records {attempted}."
        )
    if topology_rejected > rejected:
        raise BenchmarkResultsError(
            f"Ingress row '{row_name}' topology_rejected_records "
            f"{topology_rejected} exceed workload rejected_records {rejected}."
        )

    exact_zero = (
        "topology_invalid_rejections",
        "topology_retained_records",
        "topology_retained_serialized_bytes",
        "topology_retained_charge_bytes",
        "fifo_error_count",
        "sequence_error_count",
        "record_validation_error_count",
    )
    for field in exact_zero:
        require_equal(row_name, field, integers[field], 0)

    expected_action_limit = PUBLICATION_ACTION_LIMITS[candidate]
    require_equal(
        row_name,
        "publication_action_limit",
        integers["publication_action_limit"],
        expected_action_limit,
    )
    observed_actions = integers["maximum_publication_actions_observed"]
    if observed_actions > expected_action_limit:
        raise BenchmarkResultsError(
            f"Ingress row '{row_name}' maximum_publication_actions_observed "
            f"{observed_actions} exceeds published limit {expected_action_limit}. "
            "Keep producer publication bounded and retry."
        )
    if topology_attempted != 0 and observed_actions == 0:
        raise BenchmarkResultsError(
            f"Ingress row '{row_name}' maximum_publication_actions_observed must "
            "be positive when topology_attempted_records is nonzero. Publish the "
            "observed bounded action count and retry."
        )

    require_latency_summary(
        row_name,
        "producer",
        integers["sample_count"],
        numbers["producer_latency_p50_ns"],
        numbers["producer_latency_p99_ns"],
        numbers["producer_latency_p999_ns"],
        row_kind="Ingress",
    )
    require_latency_summary(
        row_name,
        "accepted",
        integers["accepted_latency_sample_count"],
        numbers["accepted_latency_p50_ns"],
        numbers["accepted_latency_p99_ns"],
        numbers["accepted_latency_p999_ns"],
        row_kind="Ingress",
    )
    require_latency_summary(
        row_name,
        "rejected",
        integers["rejected_latency_sample_count"],
        numbers["rejected_latency_p50_ns"],
        numbers["rejected_latency_p99_ns"],
        numbers["rejected_latency_p999_ns"],
        row_kind="Ingress",
    )

    wall_time = numbers["wall_time_ns"]
    if wall_time == 0:
        raise BenchmarkResultsError(
            f"Ingress row '{row_name}' counter 'wall_time_ns' must be positive."
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
    logical_metrics = tuple(integers[field] for field in LOGICAL_RECORD_FIELDS)
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
                "Benchmark result contains duplicate ingress cell "
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
            details.append(f"unexpected {len(extra)} cell(s), first: {extra[0]!r}")
        raise BenchmarkResultsError(
            "Benchmark ingress matrix is incomplete: " + "; ".join(details) + "."
        )

    validate_candidate_schedule(ordered_rows)
    return len(rows), len(EXPECTED_CANDIDATES), repetitions


def validate_result_file(path: Path) -> tuple[int, int, int]:
    return validate_document(read_result_file(path))


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate Ulog's versioned Ingress benchmark result protocol."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    validate_parser = subparsers.add_parser(
        "validate", help="Validate one ingress-results.json file."
    )
    validate_parser.add_argument("result", type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        if arguments.command == "validate":
            rows, candidates, repetitions = validate_result_file(arguments.result)
            print(
                f"Validated {rows} ingress row(s) for {candidates} candidate(s) "
                f"and {repetitions} repetition(s)"
            )
            return 0
    except BenchmarkResultsError as error:
        print(f"ingress result validation failed: {error}", file=sys.stderr)
        return 1
    raise AssertionError(f"Unhandled command {arguments.command!r}")


if __name__ == "__main__":
    raise SystemExit(main())
