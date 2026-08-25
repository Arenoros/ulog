#!/usr/bin/env python3

import argparse
import re
import subprocess
import sys
from pathlib import Path


CANDIDATES = ("central-reservation", "producer-credit-reservation")
PRODUCER_COUNTS = (1, 2, 4, 8, 16, 32)
RECORD_SIZES = (64, 256, 1024, 4096, 16384)
OCCUPANCIES = ("empty", "partial", "near-full", "saturated")
CONTROLLED_REPETITIONS = 7
LISTING_TIMEOUT_SECONDS = 30
WORKLOAD_IDENTITY = re.compile(
    r"^(?P<identity>UlogWorkload/[^/]+/producers:\d+/record_bytes:\d+/"
    r"occupancy:[^/]+/repetition:\d+)(?:/.*)?$"
)


class BenchmarkScheduleError(RuntimeError):
    pass


def expected_listing() -> list[str]:
    return [
        f"UlogWorkload/{candidate}/producers:{producers}/"
        f"record_bytes:{record_size}/occupancy:{occupancy}/"
        f"repetition:{repetition}"
        for repetition in range(CONTROLLED_REPETITIONS)
        for producers in PRODUCER_COUNTS
        for record_size in RECORD_SIZES
        for occupancy in OCCUPANCIES
        for candidate in (
            CANDIDATES if repetition % 2 == 0 else tuple(reversed(CANDIDATES))
        )
    ]


def read_listing(executable: Path) -> list[str]:
    try:
        result = subprocess.run(
            [
                str(executable),
                "--ulog_mode=controlled",
                "--benchmark_list_tests=true",
            ],
            capture_output=True,
            check=False,
            text=True,
            timeout=LISTING_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as error:
        raise BenchmarkScheduleError(
            f"Workload benchmark listing exceeded {LISTING_TIMEOUT_SECONDS} seconds. "
            "Check benchmark initialization and retry."
        ) from error
    except OSError as error:
        raise BenchmarkScheduleError(
            f"Unable to execute workload benchmark '{executable}': {error}. "
            "Build ulog-workload-benchmarks and retry."
        ) from error
    if result.returncode != 0:
        details = result.stderr.strip() or result.stdout.strip() or "no diagnostic output"
        raise BenchmarkScheduleError(
            f"Workload benchmark listing exited with {result.returncode}: {details}"
        )
    listing = []
    for row_index, line in enumerate(result.stdout.splitlines()):
        stripped = line.strip()
        if not stripped:
            continue
        match = WORKLOAD_IDENTITY.fullmatch(stripped)
        if not match:
            raise BenchmarkScheduleError(
                f"Workload benchmark listing row {row_index} has unrecognized "
                f"identity {stripped!r}. Keep Ulog workload names stable and retry."
            )
        listing.append(match.group("identity"))
    return listing


def validate_listing(actual: list[str]) -> int:
    expected = expected_listing()
    if len(actual) != len(expected):
        raise BenchmarkScheduleError(
            f"Workload benchmark listing has {len(actual)} row(s); expected "
            f"{len(expected)}. Register the complete controlled matrix and retry."
        )
    if actual != expected:
        mismatch_index = next(
            index
            for index, (actual_name, expected_name) in enumerate(
                zip(actual, expected, strict=True)
            )
            if actual_name != expected_name
        )
        raise BenchmarkScheduleError(
            f"Workload benchmark listing row {mismatch_index} is "
            f"{actual[mismatch_index]!r}; expected {expected[mismatch_index]!r}. "
            "Register adjacent candidate pairs and alternate the first candidate "
            "by repetition."
        )
    return len(actual)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate Ulog's controlled benchmark registration schedule."
    )
    parser.add_argument("executable", type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        row_count = validate_listing(read_listing(arguments.executable))
    except BenchmarkScheduleError as error:
        print(f"benchmark schedule validation failed: {error}", file=sys.stderr)
        return 1
    print(f"Validated paired-alternating schedule for {row_count} benchmark row(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
