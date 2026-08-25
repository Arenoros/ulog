import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = PROJECT_ROOT / "scripts" / "benchmark_results.py"

PRODUCER_COUNTS = (1, 2, 4, 8, 16, 32)
RECORD_SIZES = (64, 256, 1024, 4096, 16384)
OCCUPANCY_BYTES = {
    "empty": 0,
    "partial": 524_288,
    "near-full": 1_032_192,
    "saturated": 1_048_576,
}
LOGICAL_LIMIT = 1_048_576
EXPECTED_CANDIDATES = (
    "central-reservation",
    "producer-credit-reservation",
)
EXPECTED_CANDIDATE_DECLARATION = "central-reservation,producer-credit-reservation"
CANDIDATE_SCHEDULE = "paired-alternating"
MODE_REPETITIONS = {"controlled": 7, "smoke": 1}
MODE_WARMUP_ROUNDS = {"controlled": 64, "smoke": 1}
SMOKE_MEASURED_ROUNDS = 1
CONTROLLED_MINIMUM_MEASURED_ROUNDS = 64
CONTROLLED_MINIMUM_SAMPLES_PER_CELL = 100_000


def make_row(
    candidate: str,
    producers: int,
    record_size: int,
    occupancy: str,
    repetition: int,
    mode: str,
) -> dict[str, object]:
    measured_rounds = (
        SMOKE_MEASURED_ROUNDS
        if mode == "smoke"
        else max(
            CONTROLLED_MINIMUM_MEASURED_ROUNDS,
            (CONTROLLED_MINIMUM_SAMPLES_PER_CELL + producers - 1) // producers,
        )
    )
    initial_bytes = OCCUPANCY_BYTES[occupancy]
    accepted_per_round = min(
        producers, (LOGICAL_LIMIT - initial_bytes) // record_size
    )
    attempted_records = producers * measured_rounds
    accepted_records = accepted_per_round * measured_rounds
    rejected_records = attempted_records - accepted_records
    wall_time_ns = attempted_records * 1_000_000
    logical_high_water = initial_bytes + accepted_per_round * record_size
    if candidate == "central-reservation":
        physical_initial = initial_bytes
        physical_high_water = logical_high_water
        physical_limit = LOGICAL_LIMIT
    else:
        physical_initial = initial_bytes + 128
        physical_high_water = physical_initial + accepted_per_round * record_size
        physical_limit = LOGICAL_LIMIT * 2

    return {
        "name": (
            f"UlogWorkload/{candidate}/producers:{producers}/"
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
        "measured_rounds": measured_rounds,
        "sample_count": attempted_records,
        "wall_time_ns": wall_time_ns,
        "process_cpu_time_ns": wall_time_ns / 2,
        "cpu_utilization_percent": 50.0,
        "producer_latency_p50_ns": 10.0,
        "producer_latency_p99_ns": 20.0,
        "producer_latency_p999_ns": 30.0,
        "attempts_per_second": 1_000.0,
        "records_per_second": accepted_records * 1_000.0 / attempted_records,
        "bytes_per_second": (
            accepted_records * record_size * 1_000.0 / attempted_records
        ),
        "attempted_records": attempted_records,
        "accepted_records": accepted_records,
        "rejected_records": rejected_records,
        "accepted_bytes": accepted_records * record_size,
        "rejected_bytes": rejected_records * record_size,
        "allocation_count": 0,
        "allocation_failure_count": 0,
        "logical_retained_initial_bytes": initial_bytes,
        "logical_retained_high_water_bytes": logical_high_water,
        "logical_retained_final_bytes": initial_bytes,
        "logical_retained_limit_bytes": LOGICAL_LIMIT,
        "physical_retained_initial_bytes": physical_initial,
        "physical_retained_high_water_bytes": physical_high_water,
        "physical_retained_final_bytes": physical_initial,
        "physical_retained_limit_bytes": physical_limit,
        "accounting_error_count": 0,
        "retained_bound_error_count": 0,
    }


def make_document(
    *, candidates: tuple[str, ...] = EXPECTED_CANDIDATES, mode: str = "smoke"
) -> dict[str, object]:
    repetitions = MODE_REPETITIONS[mode]
    rows = [
        make_row(candidate, producers, record_size, occupancy, repetition, mode)
        for repetition in range(repetitions)
        for producers in PRODUCER_COUNTS
        for record_size in RECORD_SIZES
        for occupancy in OCCUPANCY_BYTES
        for candidate in (
            candidates if repetition % 2 == 0 else tuple(reversed(candidates))
        )
    ]
    return {
        "context": {
            "date": "2026-08-24T00:00:00+00:00",
            "host_name": "fixture",
            "ulog_result_protocol": "ulog-workload-results/4",
            "ulog_candidates": EXPECTED_CANDIDATE_DECLARATION,
            "ulog_candidate_schedule": CANDIDATE_SCHEDULE,
            "ulog_mode": mode,
            "ulog_timing_policy": "advisory",
            "ulog_repetitions": str(repetitions),
        },
        "benchmarks": rows,
    }


class BenchmarkResultsTest(unittest.TestCase):
    def run_validator(
        self, document: object | str | bytes
    ) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temp_directory:
            result_path = Path(temp_directory) / "benchmark-results.json"
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

    def test_complete_matrix_is_valid(self):
        result = self.run_validator(make_document())

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout.strip(),
            "Validated 240 workload row(s) for 2 candidate(s) and 1 repetition(s)",
        )

    def test_protocol_context_is_required(self):
        document = make_document()
        document["context"]["ulog_result_protocol"] = "ulog-workload-results/1"

        result = self.run_validator(document)

        self.assertEqual(result.returncode, 1)
        self.assertIn("ulog_result_protocol", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_multiple_candidates_and_controlled_repetitions_are_valid(self):
        result = self.run_validator(make_document(mode="controlled"))

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("1680 workload row(s)", result.stdout)
        self.assertIn("2 candidate(s) and 7 repetition(s)", result.stdout)

    def test_candidate_schedule_is_paired_and_alternates_first_candidate(self):
        nonadjacent = make_document()
        nonadjacent["benchmarks"][1], nonadjacent["benchmarks"][3] = (
            nonadjacent["benchmarks"][3],
            nonadjacent["benchmarks"][1],
        )

        wrong_odd_order = make_document(mode="controlled")
        odd_pair_start = next(
            index
            for index, row in enumerate(wrong_odd_order["benchmarks"])
            if row["workload_repetition_index"] == 1
        )
        wrong_odd_order["benchmarks"][odd_pair_start : odd_pair_start + 2] = reversed(
            wrong_odd_order["benchmarks"][odd_pair_start : odd_pair_start + 2]
        )

        wrong_context = make_document()
        wrong_context["context"]["ulog_candidate_schedule"] = "candidate-blocks"

        cases = (
            (nonadjacent, "same matrix cell"),
            (wrong_odd_order, "candidate order"),
            (wrong_context, "ulog_candidate_schedule"),
        )
        for document, expected_message in cases:
            with self.subTest(expected_message=expected_message):
                result = self.run_validator(document)
                self.assertEqual(result.returncode, 1)
                self.assertIn(expected_message, result.stderr)
                self.assertNotIn("Traceback", result.stderr)

    def test_declared_candidate_inventory_is_exact(self):
        missing_candidate = make_document(candidates=(EXPECTED_CANDIDATES[0],))
        undeclared_candidate = make_document(
            candidates=EXPECTED_CANDIDATES + ("undeclared-reservation",)
        )

        missing = self.run_validator(missing_candidate)
        undeclared = self.run_validator(undeclared_candidate)

        self.assertEqual(missing.returncode, 1)
        self.assertIn("producer-credit-reservation", missing.stderr)
        self.assertIn("ulog_candidates", missing.stderr)
        self.assertEqual(undeclared.returncode, 1)
        self.assertIn("undeclared-reservation", undeclared.stderr)
        self.assertIn("ulog_candidates", undeclared.stderr)

    def test_candidate_declaration_is_canonical_and_required(self):
        malformed_values = (
            None,
            "producer-credit-reservation,central-reservation",
            "central-reservation,producer-credit-reservation,central-reservation",
            "central-reservation, producer-credit-reservation",
            list(EXPECTED_CANDIDATES),
        )
        for value in malformed_values:
            with self.subTest(value=value):
                document = make_document()
                if value is None:
                    del document["context"]["ulog_candidates"]
                else:
                    document["context"]["ulog_candidates"] = value

                result = self.run_validator(document)

                self.assertEqual(result.returncode, 1)
                self.assertIn("ulog_candidates", result.stderr)
                self.assertNotIn("Traceback", result.stderr)

    def test_mode_repetitions_and_rounds_are_exact(self):
        invalid_documents = []

        wrong_repetitions = make_document()
        wrong_repetitions["context"]["ulog_repetitions"] = "7"
        invalid_documents.append(("ulog_repetitions", wrong_repetitions))

        wrong_smoke_warmup = make_document()
        wrong_smoke_warmup["benchmarks"][0]["warmup_rounds"] = 64
        invalid_documents.append(("warmup_rounds", wrong_smoke_warmup))

        wrong_smoke_measurement = make_document()
        wrong_smoke_measurement["benchmarks"][0]["measured_rounds"] = 63
        invalid_documents.append(("measured_rounds", wrong_smoke_measurement))

        wrong_controlled_warmup = make_document(mode="controlled")
        wrong_controlled_warmup["benchmarks"][0]["warmup_rounds"] = 8
        invalid_documents.append(("warmup_rounds", wrong_controlled_warmup))

        wrong_controlled_measurement = make_document(mode="controlled")
        wrong_controlled_measurement["benchmarks"][0]["measured_rounds"] -= 1
        invalid_documents.append(("measured_rounds", wrong_controlled_measurement))

        for field, document in invalid_documents:
            with self.subTest(field=field):
                result = self.run_validator(document)
                self.assertEqual(result.returncode, 1)
                self.assertIn(field, result.stderr)
                self.assertNotIn("Traceback", result.stderr)

    def test_context_mode_policy_and_repetition_decimal_are_strict(self):
        invalid_documents = []
        for field, value in (
            ("ulog_mode", "fast"),
            ("ulog_mode", []),
            ("ulog_timing_policy", "gate"),
            ("ulog_repetitions", 1),
            ("ulog_repetitions", "01"),
            ("ulog_repetitions", "0"),
            ("ulog_repetitions", "9" * 5_000),
        ):
            document = make_document()
            document["context"][field] = value
            invalid_documents.append((field, value, document))

        for field, value, document in invalid_documents:
            with self.subTest(field=field, value=value):
                result = self.run_validator(document)
                self.assertEqual(result.returncode, 1)
                self.assertIn(field, result.stderr)
                self.assertNotIn("Traceback", result.stderr)

    def test_strict_json_rejects_duplicate_keys_and_nonfinite_numbers(self):
        encoded = json.dumps(make_document())
        duplicate = encoded.replace(
            '"ulog_mode": "smoke"',
            '"ulog_mode": "smoke", "ulog_mode": "controlled"',
            1,
        )
        nonfinite_document = make_document()
        nonfinite_document["benchmarks"][0]["wall_time_ns"] = float("nan")

        duplicate_result = self.run_validator(duplicate)
        nonfinite_result = self.run_validator(nonfinite_document)

        self.assertEqual(duplicate_result.returncode, 1)
        self.assertIn("duplicate JSON key", duplicate_result.stderr)
        self.assertEqual(nonfinite_result.returncode, 1)
        self.assertIn("non-standard JSON number", nonfinite_result.stderr)

    def test_invalid_utf8_and_bom_are_actionable_without_traceback(self):
        invalid_utf8 = self.run_validator(b"\xff")
        bom = self.run_validator(
            b"\xef\xbb\xbf" + json.dumps(make_document()).encode("utf-8")
        )

        self.assertEqual(invalid_utf8.returncode, 1)
        self.assertIn("not valid UTF-8", invalid_utf8.stderr)
        self.assertNotIn("Traceback", invalid_utf8.stderr)
        self.assertEqual(bom.returncode, 1)
        self.assertIn("without a BOM", bom.stderr)

    def test_missing_duplicate_and_out_of_range_matrix_cells_are_rejected(self):
        missing_document = make_document()
        missing_document["benchmarks"].pop()

        duplicate_document = make_document()
        duplicate_document["benchmarks"].append(
            copy.deepcopy(duplicate_document["benchmarks"][0])
        )

        range_document = make_document()
        row = range_document["benchmarks"][0]
        row["name"] = row["name"].replace("repetition:0", "repetition:1")
        row["workload_repetition_index"] = 1

        missing = self.run_validator(missing_document)
        duplicate = self.run_validator(duplicate_document)
        out_of_range = self.run_validator(range_document)

        self.assertEqual(missing.returncode, 1)
        self.assertIn("matrix is incomplete", missing.stderr)
        self.assertIn("first missing cell", missing.stderr)
        self.assertEqual(duplicate.returncode, 1)
        self.assertIn("duplicate workload cell", duplicate.stderr)
        self.assertEqual(out_of_range.returncode, 1)
        self.assertIn("outside context range", out_of_range.stderr)

    def test_row_name_and_required_counters_are_strict(self):
        invalid_name_document = make_document()
        invalid_name_document["benchmarks"][0]["name"] = "VersionQuery"

        oversized_repetition_document = make_document()
        oversized_row = oversized_repetition_document["benchmarks"][0]
        oversized_row["name"] = oversized_row["name"].replace(
            "repetition:0", "repetition:" + "9" * 5_000
        )

        missing_counter_document = make_document()
        del missing_counter_document["benchmarks"][0]["allocation_count"]

        invalid_name = self.run_validator(invalid_name_document)
        oversized_repetition = self.run_validator(oversized_repetition_document)
        missing_counter = self.run_validator(missing_counter_document)

        self.assertEqual(invalid_name.returncode, 1)
        self.assertIn("does not match the required", invalid_name.stderr)
        self.assertEqual(oversized_repetition.returncode, 1)
        self.assertIn("does not match the required", oversized_repetition.stderr)
        self.assertNotIn("Traceback", oversized_repetition.stderr)
        self.assertEqual(missing_counter.returncode, 1)
        self.assertIn("missing required counters: allocation_count", missing_counter.stderr)

    def test_google_benchmark_envelope_must_be_a_successful_single_iteration(self):
        mutations = {
            "error_occurred": True,
            "iterations": 99,
            "run_type": "aggregate",
        }
        for field, value in mutations.items():
            with self.subTest(field=field):
                document = make_document()
                document["benchmarks"][0][field] = value
                result = self.run_validator(document)
                self.assertEqual(result.returncode, 1)
                self.assertIn(field, result.stderr)
                self.assertNotIn("Traceback", result.stderr)

    def test_optional_iterations_suffix_is_accepted(self):
        document = make_document()
        for row in document["benchmarks"]:
            row["name"] += "/iterations:1/manual_time"

        result = self.run_validator(document)

        self.assertEqual(result.returncode, 0, result.stderr)

    def test_dimension_counters_must_match_the_workload_name(self):
        mutations = {
            "producer_count": 2,
            "record_size_bytes": 256,
            "workload_repetition_index": 1,
        }
        for field, value in mutations.items():
            with self.subTest(field=field):
                document = make_document()
                document["benchmarks"][0][field] = value
                result = self.run_validator(document)
                self.assertEqual(result.returncode, 1)
                self.assertIn(field, result.stderr)

    def test_numeric_counters_must_be_finite_nonnegative_and_integral_when_counted(self):
        mutations = {
            "sample_count": 1.5,
            "allocation_count": -1,
            "wall_time_ns": "1000",
            "process_cpu_time_ns": 10**1000,
            "records_per_second": -1.0,
        }
        for field, value in mutations.items():
            with self.subTest(field=field):
                document = make_document()
                document["benchmarks"][0][field] = value
                result = self.run_validator(document)
                self.assertEqual(result.returncode, 1)
                self.assertIn(field, result.stderr)
                self.assertNotIn("Traceback", result.stderr)

    def test_attempt_accept_reject_and_byte_accounting_is_exact(self):
        for field in (
            "sample_count",
            "attempted_records",
            "accepted_records",
            "rejected_records",
            "accepted_bytes",
            "rejected_bytes",
        ):
            with self.subTest(field=field):
                document = make_document()
                document["benchmarks"][0][field] += 1
                result = self.run_validator(document)
                self.assertEqual(result.returncode, 1)
                self.assertIn(field, result.stderr)
                self.assertIn("expected", result.stderr)

    def test_saturated_and_near_full_acceptance_are_derived_from_capacity(self):
        document = make_document()
        near_full = next(
            row
            for row in document["benchmarks"]
            if "/producers:32/" in row["name"]
            and "/record_bytes:16384/" in row["name"]
            and "/occupancy:near-full/" in row["name"]
        )
        saturated = next(
            row
            for row in document["benchmarks"]
            if "/producers:32/" in row["name"]
            and "/record_bytes:16384/" in row["name"]
            and "/occupancy:saturated/" in row["name"]
        )

        self.assertEqual(near_full["accepted_records"], 1)
        self.assertEqual(near_full["rejected_records"], 31)
        self.assertEqual(saturated["accepted_records"], 0)
        self.assertEqual(saturated["rejected_records"], 32)
        self.assertEqual(self.run_validator(document).returncode, 0)

    def test_allocation_and_deterministic_error_counters_must_be_zero(self):
        for field in (
            "allocation_failure_count",
            "accounting_error_count",
            "retained_bound_error_count",
        ):
            with self.subTest(field=field):
                document = make_document()
                document["benchmarks"][0][field] = 1
                result = self.run_validator(document)
                self.assertEqual(result.returncode, 1)
                self.assertIn(field, result.stderr)
                self.assertIn("expected 0", result.stderr)

    def test_logical_and_physical_retained_occupancy_and_bounds_are_enforced(self):
        mutations = {
            "logical_retained_final_bytes": 1,
            "logical_retained_high_water_bytes": LOGICAL_LIMIT + 1,
            "logical_retained_limit_bytes": LOGICAL_LIMIT - 1,
            "physical_retained_final_bytes": 1,
            "physical_retained_high_water_bytes": LOGICAL_LIMIT * 2 + 1,
        }
        for field, value in mutations.items():
            with self.subTest(field=field):
                document = make_document()
                document["benchmarks"][0][field] = value
                result = self.run_validator(document)
                self.assertEqual(result.returncode, 1)
                self.assertIn("retained", result.stderr)

    def test_physical_retained_bytes_must_cover_logical_retained_bytes(self):
        saturated_document = make_document()
        saturated_row = next(
            row
            for row in saturated_document["benchmarks"]
            if "/occupancy:saturated/" in row["name"]
        )
        saturated_row["physical_retained_initial_bytes"] = LOGICAL_LIMIT - 1
        saturated_row["physical_retained_final_bytes"] = LOGICAL_LIMIT - 1

        final_document = make_document()
        final_row = next(
            row
            for row in final_document["benchmarks"]
            if "/occupancy:partial/" in row["name"]
        )
        final_row["physical_retained_final_bytes"] = (
            final_row["logical_retained_final_bytes"] - 1
        )

        high_water_document = make_document()
        high_water_row = next(
            row
            for row in high_water_document["benchmarks"]
            if "/producers:32/" in row["name"]
            and "/record_bytes:16384/" in row["name"]
            and "/occupancy:empty/" in row["name"]
        )
        high_water_row["physical_retained_high_water_bytes"] = (
            high_water_row["logical_retained_high_water_bytes"] - 1
        )

        limit_document = make_document()
        limit_row = next(
            row
            for row in limit_document["benchmarks"]
            if "/producers:1/" in row["name"]
            and "/record_bytes:64/" in row["name"]
            and "/occupancy:empty/" in row["name"]
        )
        limit_row["physical_retained_limit_bytes"] = LOGICAL_LIMIT - 1

        for stage, document in (
            ("initial", saturated_document),
            ("final", final_document),
            ("high-water", high_water_document),
            ("limit", limit_document),
        ):
            with self.subTest(stage=stage):
                result = self.run_validator(document)
                self.assertEqual(result.returncode, 1)
                self.assertIn("physical retained", result.stderr)
                self.assertIn("logical retained", result.stderr)
                self.assertIn(stage, result.stderr)

    def test_central_candidate_physical_retained_bytes_equal_logical(self):
        document = make_document()
        row = next(
            row
            for row in document["benchmarks"]
            if row["name"].startswith("UlogWorkload/central-reservation/")
            and "/occupancy:empty/" in row["name"]
        )
        row["physical_retained_high_water_bytes"] = (
            row["logical_retained_high_water_bytes"] + 64
        )

        result = self.run_validator(document)

        self.assertEqual(result.returncode, 1)
        self.assertIn("central candidate", result.stderr)
        self.assertIn("physical retained high-water", result.stderr)

    def test_percentiles_are_monotonic_but_have_no_timing_threshold(self):
        invalid_document = make_document()
        invalid_document["benchmarks"][0]["producer_latency_p99_ns"] = 9.0

        advisory_document = make_document()
        for row in advisory_document["benchmarks"]:
            row["producer_latency_p50_ns"] = 1e12
            row["producer_latency_p99_ns"] = 1e13
            row["producer_latency_p999_ns"] = 1e14

        invalid = self.run_validator(invalid_document)
        advisory = self.run_validator(advisory_document)

        self.assertEqual(invalid.returncode, 1)
        self.assertIn("p50 <= p99 <= p99.9", invalid.stderr)
        self.assertEqual(advisory.returncode, 0, advisory.stderr)

    def test_rates_and_cpu_utilization_follow_exact_counts_and_wall_time(self):
        for field in (
            "attempts_per_second",
            "records_per_second",
            "bytes_per_second",
            "cpu_utilization_percent",
        ):
            with self.subTest(field=field):
                document = make_document()
                document["benchmarks"][0][field] += 10.0
                result = self.run_validator(document)
                self.assertEqual(result.returncode, 1)
                self.assertIn(field, result.stderr)
                self.assertIn("wall time", result.stderr)


if __name__ == "__main__":
    unittest.main()
