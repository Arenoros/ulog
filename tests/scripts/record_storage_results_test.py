import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_DIRECTORY = PROJECT_ROOT / "scripts"
VALIDATOR = SCRIPTS_DIRECTORY / "record_storage_results.py"
sys.path.insert(0, str(SCRIPTS_DIRECTORY))

import check_record_storage_schedule as schedule  # noqa: E402


CANDIDATES = ("contiguous-record", "chunked-record", "hybrid-record")
CANDIDATE_ORDERS = (
    CANDIDATES,
    (CANDIDATES[0], CANDIDATES[2], CANDIDATES[1]),
    (CANDIDATES[1], CANDIDATES[0], CANDIDATES[2]),
    (CANDIDATES[1], CANDIDATES[2], CANDIDATES[0]),
    (CANDIDATES[2], CANDIDATES[0], CANDIDATES[1]),
    (CANDIDATES[2], CANDIDATES[1], CANDIDATES[0]),
)
PRODUCER_COUNTS = (1, 2, 4, 8, 16, 32)
RECORD_SIZES = (64, 256, 1024, 4096, 16384)
OCCUPANCY_BYTES = {
    "empty": 0,
    "partial": 524_288,
    "near-full": 1_032_192,
    "saturated": 1_048_576,
}
CAPACITY_BYTES = 1_048_576
MODE_REPETITIONS = {"controlled": 7, "smoke": 1}
MODE_WARMUP_ROUNDS = {"controlled": 64, "smoke": 8}


def measured_rounds(mode: str, producers: int) -> int:
    if mode == "smoke":
        return 64
    return max(64, (100_000 + producers - 1) // producers)


def logical_footprint(record_size: int) -> tuple[int, int, int, int, int, int]:
    stored = min(record_size, 16_096)
    owned_payload = stored + 96
    metadata = 192
    serialized = owned_payload + metadata
    truncated = int(stored < record_size)
    return record_size, stored, owned_payload, metadata, serialized, truncated


def accounting_charge(candidate: str, serialized: int) -> tuple[int, int]:
    if candidate == "contiguous-record":
        minimum = 64
        charge = max(minimum, ((serialized + 63) // 64) * 64)
    elif candidate == "chunked-record":
        minimum = 256
        charge = max(minimum, ((serialized + 255) // 256) * 256)
    else:
        minimum = 512
        if serialized <= 512:
            charge = 512
        elif serialized <= 15_872:
            charge = 512 + ((serialized - 512 + 1_023) // 1_024) * 1_024
        else:
            charge = 16_384
    return charge, minimum


def make_row(
    candidate: str,
    producers: int,
    record_size: int,
    occupancy: str,
    repetition: int,
    mode: str,
) -> dict[str, object]:
    requested, stored, owned, metadata, serialized, truncated = logical_footprint(
        record_size
    )
    charge, minimum_charge = accounting_charge(candidate, serialized)
    rounds = measured_rounds(mode, producers)
    attempted = producers * rounds
    initial = OCCUPANCY_BYTES[occupancy]
    accepted_per_round = min(producers, (CAPACITY_BYTES - initial) // charge)
    accepted = accepted_per_round * rounds
    rejected = attempted - accepted
    wall_time_ns = attempted * 1_000_000
    return {
        "name": (
            f"UlogRecordStorage/{candidate}/producers:{producers}/"
            f"record_bytes:{record_size}/occupancy:{occupancy}/"
            f"repetition:{repetition}"
        ),
        "run_type": "iteration",
        "iterations": 1,
        "real_time": 1.0,
        "cpu_time": 1.0,
        "time_unit": "ns",
        "producer_count": producers,
        "record_size_bytes": record_size,
        "workload_repetition_index": repetition,
        "warmup_rounds": MODE_WARMUP_ROUNDS[mode],
        "measured_rounds": rounds,
        "sample_count": attempted,
        "wall_time_ns": wall_time_ns,
        "process_cpu_time_ns": wall_time_ns / 2,
        "cpu_utilization_percent": 50.0,
        "producer_latency_p50_ns": 10.0,
        "producer_latency_p99_ns": 20.0,
        "producer_latency_p999_ns": 30.0,
        "accepted_latency_sample_count": accepted,
        "accepted_latency_p50_ns": 10.0 if accepted else 0.0,
        "accepted_latency_p99_ns": 20.0 if accepted else 0.0,
        "accepted_latency_p999_ns": 30.0 if accepted else 0.0,
        "rejected_latency_sample_count": rejected,
        "rejected_latency_p50_ns": 10.0 if rejected else 0.0,
        "rejected_latency_p99_ns": 20.0 if rejected else 0.0,
        "rejected_latency_p999_ns": 30.0 if rejected else 0.0,
        "attempts_per_second": 1_000.0,
        "records_per_second": accepted * 1_000.0 / attempted,
        "bytes_per_second": accepted * record_size * 1_000.0 / attempted,
        "attempted_records": attempted,
        "accepted_records": accepted,
        "rejected_records": rejected,
        "accepted_bytes": accepted * record_size,
        "rejected_bytes": rejected * record_size,
        "allocation_count": 0,
        "allocation_failure_count": 0,
        "requested_message_bytes": requested,
        "stored_message_bytes": stored,
        "owned_payload_bytes": owned,
        "metadata_bytes": metadata,
        "serialized_bytes": serialized,
        "fragmentation_bytes": charge - serialized,
        "accounting_charge_bytes": charge,
        "minimum_accounting_charge_bytes": minimum_charge,
        "record_truncated": truncated,
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


def make_document(mode: str = "smoke") -> dict[str, object]:
    repetitions = MODE_REPETITIONS[mode]
    rows = [
        make_row(candidate, producers, record_size, occupancy, repetition, mode)
        for repetition in range(repetitions)
        for producers in PRODUCER_COUNTS
        for record_size in RECORD_SIZES
        for occupancy in OCCUPANCY_BYTES
        for candidate in CANDIDATE_ORDERS[repetition % len(CANDIDATE_ORDERS)]
    ]
    return {
        "context": {
            "ulog_result_protocol": "ulog-record-storage-results/1",
            "ulog_candidates": ",".join(CANDIDATES),
            "ulog_candidate_schedule": "six-permutation-cycle",
            "ulog_mode": mode,
            "ulog_timing_policy": "advisory",
            "ulog_repetitions": str(repetitions),
        },
        "benchmarks": rows,
    }


class RecordStorageResultsTest(unittest.TestCase):
    def run_validator(
        self, document: object | str | bytes
    ) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temporary_directory:
            result_path = Path(temporary_directory) / "record-storage-results.json"
            if isinstance(document, bytes):
                result_path.write_bytes(document)
            elif isinstance(document, str):
                result_path.write_text(document, encoding="utf-8")
            else:
                result_path.write_text(json.dumps(document), encoding="utf-8")
            return subprocess.run(
                [sys.executable, str(VALIDATOR), "validate", str(result_path)],
                capture_output=True,
                check=False,
                text=True,
            )

    def assert_invalid(self, document: object, expected: str) -> None:
        result = self.run_validator(document)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn(expected, result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_smoke_and_controlled_complete_matrices_are_valid(self):
        smoke = self.run_validator(make_document())
        controlled = self.run_validator(make_document("controlled"))

        self.assertEqual(smoke.returncode, 0, smoke.stderr)
        self.assertIn("Validated 360 record-storage row(s)", smoke.stdout)
        self.assertEqual(controlled.returncode, 0, controlled.stderr)
        self.assertIn("Validated 2520 record-storage row(s)", controlled.stdout)

    def test_context_and_candidate_inventory_are_exact(self):
        mutations = {
            "ulog_result_protocol": "ulog-record-storage-results/2",
            "ulog_candidates": "chunked-record,contiguous-record,hybrid-record",
            "ulog_candidate_schedule": "candidate-blocks",
            "ulog_mode": "fast",
            "ulog_timing_policy": "gate",
            "ulog_repetitions": "01",
        }
        for field, value in mutations.items():
            with self.subTest(field=field):
                document = make_document()
                document["context"][field] = value
                self.assert_invalid(document, field)

    def test_schedule_is_adjacent_and_follows_all_six_permutations(self):
        nonadjacent = make_document()
        nonadjacent["benchmarks"][1], nonadjacent["benchmarks"][4] = (
            nonadjacent["benchmarks"][4],
            nonadjacent["benchmarks"][1],
        )
        self.assert_invalid(nonadjacent, "same matrix cell")

        controlled = make_document("controlled")
        for repetition in range(7):
            with self.subTest(repetition=repetition):
                document = copy.deepcopy(controlled)
                start = next(
                    index
                    for index, row in enumerate(document["benchmarks"])
                    if row["workload_repetition_index"] == repetition
                )
                document["benchmarks"][start], document["benchmarks"][start + 1] = (
                    document["benchmarks"][start + 1],
                    document["benchmarks"][start],
                )
                self.assert_invalid(document, "candidate order")

    def test_missing_duplicate_and_undeclared_cells_are_rejected(self):
        missing = make_document()
        missing["benchmarks"].pop()
        self.assert_invalid(missing, "matrix is incomplete")

        duplicate = make_document()
        duplicate["benchmarks"].append(copy.deepcopy(duplicate["benchmarks"][0]))
        self.assert_invalid(duplicate, "duplicate record-storage cell")

        undeclared = make_document()
        undeclared["benchmarks"][0]["name"] = undeclared["benchmarks"][0][
            "name"
        ].replace("contiguous-record", "unknown-record")
        self.assert_invalid(undeclared, "does not match the required")

    def test_canonical_logical_record_metrics_are_exact(self):
        document = make_document()
        row = document["benchmarks"][1]
        row["metadata_bytes"] += 1
        row["serialized_bytes"] += 1
        row["fragmentation_bytes"] -= 1
        row["logical_retained_high_water_bytes"] += 1

        self.assert_invalid(document, "canonical Record recipe")

    def test_record_footprint_relationships_are_enforced(self):
        cases = []
        for field, value, expected in (
            ("requested_message_bytes", 63, "requested_message_bytes"),
            ("stored_message_bytes", 65, "stored_message_bytes"),
            ("owned_payload_bytes", 63, "owned_payload_bytes"),
            ("serialized_bytes", 111, "serialized_bytes"),
            ("accounting_charge_bytes", 127, "accounting_charge_bytes"),
            ("record_truncated", 1, "record_truncated"),
            ("truncated_records", 1, "truncated_records"),
        ):
            document = make_document()
            document["benchmarks"][0][field] = value
            cases.append((expected, document))
        for expected, document in cases:
            with self.subTest(expected=expected):
                self.assert_invalid(document, expected)

    def test_candidate_charge_formula_and_minimum_are_exact(self):
        for row_index, candidate in enumerate(CANDIDATES):
            with self.subTest(candidate=candidate):
                document = make_document()
                row = document["benchmarks"][row_index]
                row["accounting_charge_bytes"] += 64
                row["fragmentation_bytes"] += 64
                self.assert_invalid(document, "charge formula")

                document = make_document()
                document["benchmarks"][row_index][
                    "minimum_accounting_charge_bytes"
                ] += 1
                self.assert_invalid(document, "minimum_accounting_charge_bytes")

    def test_admission_and_requested_byte_accounting_are_exact(self):
        for field in (
            "sample_count",
            "attempted_records",
            "accepted_records",
            "rejected_records",
            "accepted_bytes",
            "rejected_bytes",
            "accepted_latency_sample_count",
            "rejected_latency_sample_count",
        ):
            with self.subTest(field=field):
                document = make_document()
                document["benchmarks"][0][field] += 1
                self.assert_invalid(document, field)

    def test_retained_high_water_and_final_baseline_are_exact(self):
        for field in (
            "logical_retained_initial_bytes",
            "logical_retained_high_water_bytes",
            "logical_retained_final_bytes",
            "logical_retained_limit_bytes",
            "physical_retained_initial_bytes",
            "physical_retained_high_water_bytes",
            "physical_retained_final_bytes",
            "physical_retained_limit_bytes",
        ):
            with self.subTest(field=field):
                document = make_document()
                document["benchmarks"][0][field] += 1
                self.assert_invalid(document, field)

    def test_allocations_failures_and_error_counters_are_zero(self):
        for field in (
            "allocation_count",
            "allocation_failure_count",
            "accounting_error_count",
            "retained_bound_error_count",
            "record_validation_error_count",
        ):
            with self.subTest(field=field):
                document = make_document()
                document["benchmarks"][0][field] = 1
                self.assert_invalid(document, field)

    def test_numeric_rates_and_latency_summaries_are_validated(self):
        mutations = (
            ("wall_time_ns", 0, "wall_time_ns"),
            ("records_per_second", float("inf"), "strict JSON"),
            ("bytes_per_second", 1.0, "bytes_per_second"),
            ("cpu_utilization_percent", 51.0, "cpu_utilization_percent"),
            ("producer_latency_p99_ns", 9.0, "p50 <= p99"),
            ("accepted_latency_p999_ns", 9.0, "accepted latency"),
        )
        for field, value, expected in mutations:
            with self.subTest(field=field):
                document = make_document()
                document["benchmarks"][0][field] = value
                self.assert_invalid(document, expected)

        document = make_document()
        saturated = next(
            row for row in document["benchmarks"] if "/occupancy:saturated/" in row["name"]
        )
        saturated["accepted_latency_p50_ns"] = 1
        saturated["accepted_latency_p99_ns"] = 1
        saturated["accepted_latency_p999_ns"] = 1
        self.assert_invalid(document, "zero-sample accepted latency")

    def test_google_benchmark_suffix_and_strict_json_handling(self):
        suffixed = make_document()
        for row in suffixed["benchmarks"]:
            row["name"] += "/iterations:1/manual_time"
        valid = self.run_validator(suffixed)
        self.assertEqual(valid.returncode, 0, valid.stderr)

        encoded = json.dumps(make_document())
        duplicate = encoded.replace(
            '"ulog_mode": "smoke"',
            '"ulog_mode": "smoke", "ulog_mode": "controlled"',
            1,
        )
        self.assert_invalid(duplicate, "duplicate JSON key")
        self.assert_invalid(b"\xef\xbb\xbf" + encoded.encode(), "without a BOM")

    def test_schedule_checker_normalizes_suffixes_and_rejects_wrong_order(self):
        expected = schedule.expected_listing()
        self.assertEqual(len(expected), 2_520)
        normalized = [
            schedule.normalize_listing_row(name + "/iterations:1/manual_time", index)
            for index, name in enumerate(expected)
        ]
        self.assertEqual(schedule.validate_listing(normalized), 2_520)

        wrong = normalized.copy()
        wrong[0], wrong[1] = wrong[1], wrong[0]
        with self.assertRaisesRegex(schedule.RecordStorageScheduleError, "row 0"):
            schedule.validate_listing(wrong)

        with self.assertRaisesRegex(schedule.RecordStorageScheduleError, "unrecognized"):
            schedule.normalize_listing_row("BM_Unrelated/0", 0)


if __name__ == "__main__":
    unittest.main()
