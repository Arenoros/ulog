import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


PROJECT_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_DIRECTORY = PROJECT_ROOT / "scripts"
VALIDATOR = SCRIPTS_DIRECTORY / "ingress_results.py"
sys.path.insert(0, str(SCRIPTS_DIRECTORY))

import check_ingress_schedule as schedule  # noqa: E402
import ingress_results as ingress  # noqa: E402


CANDIDATES = (
    "bounded-mpsc-ring",
    "chunked-mpsc",
    "per-producer-lanes",
)
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
MODE_WARMUP_ROUNDS = {"controlled": 64, "smoke": 1}
ACTION_LIMITS = {
    "bounded-mpsc-ring": 70,
    "chunked-mpsc": 11,
    "per-producer-lanes": 11,
}


def measured_rounds(mode: str, producers: int) -> int:
    if mode == "smoke":
        return 1
    return max(64, (100_000 + producers - 1) // producers)


def record_footprint(record_size: int) -> tuple[int, int, int, int, int, int, int]:
    stored = min(record_size, 16_096)
    owned = stored + 96
    metadata = 192
    serialized = owned + metadata
    charge = max(64, ((serialized + 63) // 64) * 64)
    return record_size, stored, owned, metadata, serialized, charge, int(stored < record_size)


def make_row(
    candidate: str,
    producers: int,
    record_size: int,
    occupancy: str,
    repetition: int,
    mode: str,
) -> dict[str, object]:
    requested, stored, owned, metadata, serialized, charge, truncated = record_footprint(
        record_size
    )
    rounds = measured_rounds(mode, producers)
    attempted = producers * rounds
    initial = OCCUPANCY_BYTES[occupancy]
    accepted_per_round = min(producers, (CAPACITY_BYTES - initial) // charge)
    accepted = accepted_per_round * rounds
    rejected = attempted - accepted
    wall_time_ns = attempted * 1_000_000
    return {
        "name": (
            f"UlogIngressTopology/{candidate}/producers:{producers}/"
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
        "maximum_accepted_per_round": accepted_per_round,
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
        "minimum_accounting_charge_bytes": 64,
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
        "fifo_error_count": 0,
        "sequence_error_count": 0,
        "record_validation_error_count": 0,
        "maximum_publication_actions_observed": min(ACTION_LIMITS[candidate], 5) if accepted else 0,
        "publication_action_limit": ACTION_LIMITS[candidate],
    }


def make_document(mode: str = "smoke") -> dict[str, object]:
    repetitions = MODE_REPETITIONS[mode]
    return {
        "context": {
            "ulog_result_protocol": "ulog-ingress-results/1",
            "ulog_candidates": ",".join(CANDIDATES),
            "ulog_candidate_schedule": "six-permutation-cycle",
            "ulog_mode": mode,
            "ulog_timing_policy": "advisory",
            "ulog_repetitions": str(repetitions),
            "ulog_publication_action_unit": "bounded topology actions per TryPublish call",
        },
        "benchmarks": [
            make_row(candidate, producers, record_size, occupancy, repetition, mode)
            for repetition in range(repetitions)
            for producers in PRODUCER_COUNTS
            for record_size in RECORD_SIZES
            for occupancy in OCCUPANCY_BYTES
            for candidate in CANDIDATE_ORDERS[repetition % len(CANDIDATE_ORDERS)]
        ],
    }


class IngressResultsTest(unittest.TestCase):
    def run_validator(
        self, document: object | str | bytes
    ) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temporary_directory:
            result_path = Path(temporary_directory) / "ingress-results.json"
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
        self.assertIn("Validated 360 ingress row(s)", smoke.stdout)
        self.assertEqual(controlled.returncode, 0, controlled.stderr)
        self.assertIn("Validated 2520 ingress row(s)", controlled.stdout)

    def test_context_and_candidate_inventory_are_exact(self):
        mutations = {
            "ulog_result_protocol": "ulog-ingress-results/2",
            "ulog_candidates": "chunked-mpsc,bounded-mpsc-ring,per-producer-lanes",
            "ulog_candidate_schedule": "candidate-blocks",
            "ulog_mode": "fast",
            "ulog_timing_policy": "gate",
            "ulog_repetitions": "01",
            "ulog_publication_action_unit": "atomic operations",
        }
        for field, value in mutations.items():
            with self.subTest(field=field):
                document = make_document()
                document["context"][field] = value
                self.assert_invalid(document, field)

    def test_workload_result_counters_are_exact(self):
        for field in (
            "sample_count",
            "accepted_latency_sample_count",
            "rejected_latency_sample_count",
            "attempted_records",
            "accepted_records",
            "rejected_records",
            "maximum_accepted_per_round",
            "accepted_bytes",
            "rejected_bytes",
            "logical_retained_high_water_bytes",
            "physical_retained_high_water_bytes",
        ):
            with self.subTest(field=field):
                document = make_document()
                document["benchmarks"][0][field] += 1
                self.assert_invalid(document, field)

    def test_candidate_admission_rejection_below_capacity_is_valid(self):
        document = make_document()
        row = next(
            row
            for row in document["benchmarks"]
            if row["name"].startswith("UlogIngressTopology/chunked-mpsc/")
            and "/producers:32/" in row["name"]
            and "/record_bytes:16384/" in row["name"]
            and "/occupancy:near-full/" in row["name"]
        )
        row["accepted_latency_sample_count"] = 0
        row["accepted_latency_p50_ns"] = 0
        row["accepted_latency_p99_ns"] = 0
        row["accepted_latency_p999_ns"] = 0
        row["rejected_latency_sample_count"] = 32
        row["accepted_records"] = 0
        row["rejected_records"] = 32
        row["maximum_accepted_per_round"] = 0
        row["accepted_bytes"] = 0
        row["rejected_bytes"] = 32 * 16_384
        row["truncated_records"] = 0
        row["logical_retained_high_water_bytes"] = OCCUPANCY_BYTES["near-full"]
        row["physical_retained_high_water_bytes"] = OCCUPANCY_BYTES["near-full"]
        row["topology_enqueued_records"] = 0
        row["topology_dequeued_records"] = 0
        row["topology_rejected_records"] = 1
        row["topology_contention_rejections"] = 1
        row["records_per_second"] = 0
        row["bytes_per_second"] = 0

        result = self.run_validator(document)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_topology_accounting_identities_and_workload_relationship_are_exact(self):
        mutations = {
            "topology_attempted_records": 1,
            "topology_enqueued_records": 1,
            "topology_dequeued_records": 1,
            "topology_rejected_records": 1,
            "topology_full_rejections": 1,
            "topology_contention_rejections": 1,
            "topology_invalid_rejections": 1,
        }
        for field, increment in mutations.items():
            with self.subTest(field=field):
                document = make_document()
                document["benchmarks"][0][field] += increment
                self.assert_invalid(document, field)

        allowed_rejection = make_document()
        row = next(
            row
            for row in allowed_rejection["benchmarks"]
            if "/producers:32/" in row["name"]
            and "/record_bytes:16384/" in row["name"]
            and "/occupancy:near-full/" in row["name"]
        )
        row["topology_attempted_records"] += 1
        row["topology_rejected_records"] += 1
        row["topology_contention_rejections"] += 1
        self.assertEqual(self.run_validator(allowed_rejection).returncode, 0)

        invalid_rejection = copy.deepcopy(allowed_rejection)
        row = next(
            row
            for row in invalid_rejection["benchmarks"]
            if "/producers:32/" in row["name"]
            and "/record_bytes:16384/" in row["name"]
            and "/occupancy:near-full/" in row["name"]
        )
        row["topology_contention_rejections"] -= 1
        row["topology_invalid_rejections"] += 1
        self.assert_invalid(invalid_rejection, "topology_invalid_rejections")

    def test_topology_and_validation_retained_counters_must_finish_at_zero(self):
        for field in (
            "topology_retained_records",
            "topology_retained_serialized_bytes",
            "topology_retained_charge_bytes",
            "fifo_error_count",
            "sequence_error_count",
            "record_validation_error_count",
        ):
            with self.subTest(field=field):
                document = make_document()
                document["benchmarks"][0][field] = 1
                self.assert_invalid(document, field)

    def test_publication_action_limit_is_candidate_specific_and_observed_is_bounded(self):
        for candidate in CANDIDATES:
            with self.subTest(candidate=candidate):
                document = make_document()
                row = next(
                    row
                    for row in document["benchmarks"]
                    if row["name"].startswith(f"UlogIngressTopology/{candidate}/")
                )
                row["publication_action_limit"] += 1
                self.assert_invalid(document, "publication_action_limit")

                document = make_document()
                row = next(
                    row
                    for row in document["benchmarks"]
                    if row["name"].startswith(f"UlogIngressTopology/{candidate}/")
                )
                row["maximum_publication_actions_observed"] = ACTION_LIMITS[candidate] + 1
                self.assert_invalid(document, "maximum_publication_actions_observed")

    def test_record_footprint_is_canonical_and_identical_across_candidates(self):
        document = make_document()
        document["benchmarks"][0]["metadata_bytes"] += 1
        document["benchmarks"][0]["serialized_bytes"] += 1
        document["benchmarks"][0]["fragmentation_bytes"] -= 1
        self.assert_invalid(document, "canonical Record recipe")

        document = make_document()
        document["benchmarks"][1]["stored_message_bytes"] -= 1
        self.assert_invalid(document, "canonical Record recipe")

        identities = [
            ((candidate, 1, 64, "empty", 0), (64, 64, 160, 192, 352, 0))
            for candidate in CANDIDATES
        ]
        identities[1] = (identities[1][0], (64, 63, 159, 192, 351, 0))
        with self.assertRaisesRegex(
            ingress.BenchmarkResultsError, "same-cell logical Record"
        ):
            ingress.validate_candidate_schedule(identities)

    def test_schedule_is_adjacent_and_uses_all_six_permutations(self):
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

    def test_missing_duplicate_suffix_and_strict_json_are_handled(self):
        missing = make_document()
        missing["benchmarks"].pop()
        self.assert_invalid(missing, "matrix is incomplete")

        duplicate = make_document()
        duplicate["benchmarks"].append(copy.deepcopy(duplicate["benchmarks"][0]))
        self.assert_invalid(duplicate, "duplicate ingress cell")

        suffixed = make_document()
        for row in suffixed["benchmarks"]:
            row["name"] += "/iterations:1/manual_time"
        self.assertEqual(self.run_validator(suffixed).returncode, 0)

        encoded = json.dumps(make_document())
        duplicate_json = encoded.replace(
            '"ulog_mode": "smoke"',
            '"ulog_mode": "smoke", "ulog_mode": "controlled"',
            1,
        )
        self.assert_invalid(duplicate_json, "duplicate JSON key")
        self.assert_invalid(b"\xef\xbb\xbf" + encoded.encode(), "without a BOM")

    def test_numeric_rates_latency_and_required_counters_are_validated(self):
        cases = (
            ("wall_time_ns", 0, "wall_time_ns"),
            ("records_per_second", 1.0, "records_per_second"),
            ("cpu_utilization_percent", 51.0, "cpu_utilization_percent"),
            ("producer_latency_p99_ns", 9.0, "p50 <= p99"),
            ("publication_action_limit", 1.5, "publication_action_limit"),
        )
        for field, value, expected in cases:
            with self.subTest(field=field):
                document = make_document()
                document["benchmarks"][0][field] = value
                self.assert_invalid(document, expected)

        document = make_document()
        del document["benchmarks"][0]["topology_attempted_records"]
        self.assert_invalid(document, "missing required counters")

    def test_schedule_checker_has_a_bounded_listing_and_exact_controlled_order(self):
        expected = schedule.expected_listing()
        self.assertEqual(schedule.LISTING_TIMEOUT_SECONDS, 30)
        self.assertEqual(len(expected), 2_520)
        normalized = [
            schedule.normalize_listing_row(name + "/iterations:1/manual_time", index)
            for index, name in enumerate(expected)
        ]
        self.assertEqual(schedule.validate_listing(normalized), 2_520)

        wrong = normalized.copy()
        wrong[0], wrong[1] = wrong[1], wrong[0]
        with self.assertRaisesRegex(schedule.IngressScheduleError, "row 0"):
            schedule.validate_listing(wrong)

        with mock.patch.object(
            schedule.subprocess,
            "run",
            side_effect=subprocess.TimeoutExpired("ulog-ingress-benchmark", 30),
        ):
            with self.assertRaisesRegex(schedule.IngressScheduleError, "30 seconds"):
                schedule.read_listing(Path("ulog-ingress-benchmark"))


if __name__ == "__main__":
    unittest.main()
