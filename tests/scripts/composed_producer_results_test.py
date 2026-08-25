import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_DIRECTORY = PROJECT_ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS_DIRECTORY))

import check_composed_producer_schedule as schedule  # noqa: E402
import composed_producer_results as results  # noqa: E402


def footprint(record_size: int) -> tuple[int, int]:
    stored = min(record_size, results.MAXIMUM_STORED_MESSAGE_BYTES)
    serialized = (
        stored + results.CANONICAL_FIXED_PAYLOAD_BYTES + results.CANONICAL_METADATA_BYTES
    )
    return serialized, results.round_up(serialized, results.CONTIGUOUS_QUANTUM_BYTES)


def make_row(
    producers: int, record_size: int, occupancy: str, repetition: int
) -> dict[str, object]:
    serialized, charge = footprint(record_size)
    stored = min(record_size, results.MAXIMUM_STORED_MESSAGE_BYTES)
    owned_payload = stored + results.CANONICAL_FIXED_PAYLOAD_BYTES
    truncated = int(stored < record_size)
    initial = results.OCCUPANCY_BYTES[occupancy]
    accepted_per_round = min(
        producers, (results.CAPACITY_BYTES - initial) // charge
    )
    attempted = producers
    accepted = accepted_per_round
    rejected = attempted - accepted
    row = {
        "name": (
            "UlogComposedProducer/composed-producer/"
            f"producers:{producers}/record_bytes:{record_size}/"
            f"occupancy:{occupancy}/repetition:{repetition}"
        ),
        "run_type": "iteration",
        "iterations": 1,
        "producer_count": producers,
        "record_size_bytes": record_size,
        "workload_repetition_index": repetition,
        "warmup_rounds": 1,
        "measured_rounds": 1,
        "sample_count": attempted,
        "accepted_latency_sample_count": accepted,
        "rejected_latency_sample_count": rejected,
        "attempted_records": attempted,
        "accepted_records": accepted,
        "rejected_records": rejected,
        "maximum_accepted_per_round": accepted_per_round,
        "accepted_bytes": accepted * record_size,
        "rejected_bytes": rejected * record_size,
        "allocation_count": 0,
        "allocation_failure_count": 0,
        "requested_message_bytes": record_size,
        "stored_message_bytes": stored,
        "owned_payload_bytes": owned_payload,
        "metadata_bytes": results.CANONICAL_METADATA_BYTES,
        "serialized_bytes": serialized,
        "fragmentation_bytes": charge - serialized,
        "accounting_charge_bytes": charge,
        "minimum_accounting_charge_bytes": results.CONTIGUOUS_QUANTUM_BYTES,
        "record_truncated": truncated,
        "truncated_records": accepted * truncated,
        "logical_retained_initial_bytes": initial,
        "logical_retained_high_water_bytes": initial + accepted * serialized,
        "logical_retained_final_bytes": initial,
        "logical_retained_limit_bytes": results.CAPACITY_BYTES,
        "physical_retained_initial_bytes": initial + accepted * charge,
        "physical_retained_high_water_bytes": initial + accepted * charge,
        "physical_retained_final_bytes": initial + accepted * charge,
        "physical_retained_limit_bytes": results.CAPACITY_BYTES,
        "accounting_error_count": 0,
        "retained_bound_error_count": 0,
        "message_callback_count": accepted,
        "context_callback_count": accepted,
        "fifo_error_count": 0,
        "record_validation_error_count": 0,
        "publication_error_count": 0,
        "lifecycle_error_count": 0,
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
    }
    wall_time_ns = attempted * 1_000_000
    row.update(
        {
            "wall_time_ns": wall_time_ns,
            "process_cpu_time_ns": wall_time_ns / 2,
            "cpu_utilization_percent": 50.0,
            "producer_latency_p50_ns": 10.0,
            "producer_latency_p99_ns": 20.0,
            "producer_latency_p999_ns": 30.0,
            "accepted_latency_p50_ns": 10.0 if accepted else 0.0,
            "accepted_latency_p99_ns": 20.0 if accepted else 0.0,
            "accepted_latency_p999_ns": 30.0 if accepted else 0.0,
            "rejected_latency_p50_ns": 10.0 if rejected else 0.0,
            "rejected_latency_p99_ns": 20.0 if rejected else 0.0,
            "rejected_latency_p999_ns": 30.0 if rejected else 0.0,
            "attempts_per_second": 1_000.0,
            "records_per_second": accepted * 1_000.0 / attempted,
            "bytes_per_second": accepted * record_size * 1_000.0 / attempted,
        }
    )
    return row


def make_document() -> dict[str, object]:
    return {
        "context": {
            "ulog_result_protocol": results.RESULT_PROTOCOL,
            "ulog_candidates": results.CANDIDATE,
            "ulog_candidate_schedule": "single-candidate",
            "ulog_timing_policy": "advisory",
            "ulog_mode": "smoke",
            "ulog_repetitions": "1",
        },
        "benchmarks": [
            make_row(producers, record_size, occupancy, 0)
            for producers in results.PRODUCER_COUNTS
            for record_size in results.RECORD_SIZES
            for occupancy in results.OCCUPANCIES
        ],
    }


class ComposedProducerResultsTest(unittest.TestCase):
    def validate(self, document: dict[str, object]) -> tuple[int, int, int]:
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "composed-producer-results.json"
            path.write_text(json.dumps(document), encoding="utf-8")
            return results.validate_result_file(path)

    def test_valid_smoke_matrix(self):
        self.assertEqual(self.validate(make_document()), (120, 1, 1))

    def test_rejects_callback_count_that_exceeds_admission(self):
        document = copy.deepcopy(make_document())
        document["benchmarks"][0]["message_callback_count"] += 1
        with self.assertRaisesRegex(
            results.BenchmarkResultsError, "message_callback_count"
        ):
            self.validate(document)

    def test_rejects_aggregated_benchmark_row(self):
        document = copy.deepcopy(make_document())
        document["benchmarks"][0]["run_type"] = "aggregate"
        with self.assertRaisesRegex(
            results.BenchmarkResultsError, "single benchmark iteration"
        ):
            self.validate(document)

    def test_rejects_boolean_iteration_count(self):
        document = copy.deepcopy(make_document())
        document["benchmarks"][0]["iterations"] = True
        with self.assertRaisesRegex(
            results.BenchmarkResultsError, "single benchmark iteration"
        ):
            self.validate(document)

    def test_requires_timing_protocol_counters(self):
        document = copy.deepcopy(make_document())
        del document["benchmarks"][0]["wall_time_ns"]
        with self.assertRaisesRegex(results.BenchmarkResultsError, "wall_time_ns"):
            self.validate(document)

    def test_rejects_zero_wall_time(self):
        document = copy.deepcopy(make_document())
        document["benchmarks"][0]["wall_time_ns"] = 0
        with self.assertRaisesRegex(results.BenchmarkResultsError, "must be positive"):
            self.validate(document)

    def test_rejects_rate_inconsistent_with_wall_time(self):
        document = copy.deepcopy(make_document())
        document["benchmarks"][0]["records_per_second"] += 1
        with self.assertRaisesRegex(results.BenchmarkResultsError, "records_per_second"):
            self.validate(document)

    def test_rejects_nonzero_zero_sample_latency(self):
        document = copy.deepcopy(make_document())
        saturated = next(
            row
            for row in document["benchmarks"]
            if "/occupancy:saturated/" in row["name"]
        )
        saturated["accepted_latency_p50_ns"] = 1.0
        saturated["accepted_latency_p99_ns"] = 1.0
        saturated["accepted_latency_p999_ns"] = 1.0
        with self.assertRaisesRegex(
            results.BenchmarkResultsError, "zero-sample accepted latency"
        ):
            self.validate(document)

    def test_rejects_self_consistent_undercharged_footprint(self):
        document = copy.deepcopy(make_document())
        document["benchmarks"][0]["accounting_charge_bytes"] = 128
        with self.assertRaisesRegex(
            results.BenchmarkResultsError, "contiguous Record policy"
        ):
            self.validate(document)

    def test_rejects_zero_accounting_charge_as_protocol_error(self):
        document = copy.deepcopy(make_document())
        document["benchmarks"][0]["accounting_charge_bytes"] = 0
        with self.assertRaisesRegex(
            results.BenchmarkResultsError, "contiguous Record policy"
        ):
            self.validate(document)

    def test_schedule_requires_every_controlled_cell(self):
        expected = schedule.expected_listing()
        self.assertEqual(schedule.validate_listing(expected), 840)
        with self.assertRaisesRegex(schedule.ScheduleError, "found 839"):
            schedule.validate_listing(expected[:-1])


if __name__ == "__main__":
    unittest.main()
