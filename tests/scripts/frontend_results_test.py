import json
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_DIRECTORY = PROJECT_ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS_DIRECTORY))

import frontend_results as results  # noqa: E402
import check_frontend_schedule as schedule  # noqa: E402


def make_row(path: str, repetition: int = 0) -> dict[str, object]:
    attempts = results.SMOKE_MEASURED_ATTEMPTS
    accepted = attempts if path == "ordinary-accepted" else 0
    rejected = attempts - accepted
    wall_time_ns = attempts * 1_000
    row = {
        "name": f"UlogFrontend/producer-frontend/path:{path}/repetition:{repetition}",
        "run_type": "iteration",
        "iterations": 1,
        "workload_repetition_index": repetition,
        "warmup_attempts": results.SMOKE_WARMUP_ATTEMPTS,
        "measured_attempts": attempts,
        "sample_count": attempts,
        "accepted_latency_sample_count": accepted,
        "rejected_latency_sample_count": rejected,
        "attempted_records": attempts,
        "accepted_records": accepted,
        "rejected_records": rejected,
        "compile_erased_records": attempts if path == "compile-erased" else 0,
        "runtime_filtered_records": attempts if path == "runtime-filtered" else 0,
        "null_logger_records": attempts if path == "null-logger" else 0,
        "admission_rejected_records": attempts if path == "admission-rejected" else 0,
        "message_evaluation_count": accepted,
        "default_logger_load_count": 0 if path == "compile-erased" else attempts,
        "accepted_bytes": accepted * results.MESSAGE_BYTES,
        "allocation_count": 0,
        "allocation_failure_count": 0,
        "logical_retained_final_bytes": 0,
        "physical_retained_final_bytes": 0,
        "accounting_error_count": 0,
        "retained_bound_error_count": 0,
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
        "attempts_per_second": 1_000_000.0,
        "records_per_second": accepted * 1_000_000.0 / attempts,
        "bytes_per_second": accepted * results.MESSAGE_BYTES * 1_000_000.0 / attempts,
    }
    return row


def make_document() -> dict[str, object]:
    return {
        "context": {
            "ulog_result_protocol": results.RESULT_PROTOCOL,
            "ulog_candidates": results.CANDIDATE,
            "ulog_candidate_schedule": "repetition-major",
            "ulog_frontend_paths": ",".join(results.PATHS),
            "ulog_timing_policy": "advisory",
            "ulog_mode": "smoke",
            "ulog_repetitions": "1",
        },
        "benchmarks": [make_row(path) for path in results.PATHS],
    }


class FrontendResultsTest(unittest.TestCase):
    def validate(self, document: dict[str, object]) -> tuple[int, int, int]:
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "frontend-results.json"
            path.write_text(json.dumps(document), encoding="utf-8")
            return results.validate_result_file(path)

    def test_valid_smoke_paths(self):
        self.assertEqual(self.validate(make_document()), (5, 1, 1))

    def test_rejects_frontend_allocation(self):
        document = make_document()
        document["benchmarks"][0]["allocation_count"] = 1
        with self.assertRaisesRegex(results.BenchmarkResultsError, "allocation_count"):
            self.validate(document)

    def test_rejects_disabled_message_evaluation(self):
        document = make_document()
        document["benchmarks"][1]["message_evaluation_count"] = 1
        with self.assertRaisesRegex(
            results.BenchmarkResultsError, "message_evaluation_count"
        ):
            self.validate(document)

    def test_rejects_more_than_one_default_logger_load_per_active_call(self):
        document = make_document()
        document["benchmarks"][-1]["default_logger_load_count"] += 1
        with self.assertRaisesRegex(
            results.BenchmarkResultsError, "default_logger_load_count"
        ):
            self.validate(document)

    def test_rejects_accounting_failure(self):
        document = make_document()
        document["benchmarks"][-1]["accounting_error_count"] = 1
        with self.assertRaisesRegex(results.BenchmarkResultsError, "accounting_error_count"):
            self.validate(document)

    def test_rejects_missing_latency_counter(self):
        document = make_document()
        del document["benchmarks"][0]["accepted_latency_p999_ns"]
        with self.assertRaisesRegex(results.BenchmarkResultsError, "accepted_latency_p999_ns"):
            self.validate(document)

    def test_rejects_nonzero_zero_sample_latency(self):
        document = make_document()
        document["benchmarks"][0]["accepted_latency_p50_ns"] = 1.0
        document["benchmarks"][0]["accepted_latency_p99_ns"] = 1.0
        document["benchmarks"][0]["accepted_latency_p999_ns"] = 1.0
        with self.assertRaisesRegex(results.BenchmarkResultsError, "zero-sample accepted"):
            self.validate(document)

    def test_rejects_rate_inconsistent_with_wall_time(self):
        document = make_document()
        document["benchmarks"][-1]["records_per_second"] += 100
        with self.assertRaisesRegex(results.BenchmarkResultsError, "records_per_second"):
            self.validate(document)

    def test_requires_every_frontend_path_in_order(self):
        document = make_document()
        document["benchmarks"].pop()
        with self.assertRaisesRegex(results.BenchmarkResultsError, "missing, duplicated"):
            self.validate(document)

    def test_rejects_google_benchmark_error_rows(self):
        document = make_document()
        document["benchmarks"][0]["error_occurred"] = True
        with self.assertRaisesRegex(results.BenchmarkResultsError, "benchmark error"):
            self.validate(document)

    def test_controlled_schedule_is_bounded_and_repetition_major(self):
        expected = schedule.expected_listing()
        self.assertEqual(len(expected), 35)
        self.assertEqual(schedule.validate_listing(expected), 35)
        with self.assertRaisesRegex(schedule.FrontendScheduleError, "row 0"):
            schedule.validate_listing(list(reversed(expected)))


if __name__ == "__main__":
    unittest.main()
