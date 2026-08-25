#!/usr/bin/env python3

import argparse
import re
import subprocess
import sys
from pathlib import Path


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
OCCUPANCIES = ("empty", "partial", "near-full", "saturated")
CONTROLLED_REPETITIONS = 7
LISTING_TIMEOUT_SECONDS = 30
RECORD_STORAGE_IDENTITY = re.compile(
    r"^(?P<identity>UlogRecordStorage/"
    r"(?:contiguous-record|chunked-record|hybrid-record)/"
    r"producers:(?:1|2|4|8|16|32)/"
    r"record_bytes:(?:64|256|1024|4096|16384)/"
    r"occupancy:(?:empty|partial|near-full|saturated)/"
    r"repetition:[0-6])(?:/.*)?$"
)


class RecordStorageScheduleError(RuntimeError):
    pass


def expected_listing() -> list[str]:
    return [
        f"UlogRecordStorage/{candidate}/producers:{producers}/"
        f"record_bytes:{record_size}/occupancy:{occupancy}/"
        f"repetition:{repetition}"
        for repetition in range(CONTROLLED_REPETITIONS)
        for producers in PRODUCER_COUNTS
        for record_size in RECORD_SIZES
        for occupancy in OCCUPANCIES
        for candidate in CANDIDATE_ORDERS[repetition % len(CANDIDATE_ORDERS)]
    ]


def normalize_listing_row(row: str, row_index: int) -> str:
    stripped = row.strip()
    match = RECORD_STORAGE_IDENTITY.fullmatch(stripped)
    if not match:
        raise RecordStorageScheduleError(
            f"Record-storage benchmark listing row {row_index} has unrecognized "
            f"identity {stripped!r}. Keep UlogRecordStorage names stable and retry."
        )
    return match.group("identity")


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
        raise RecordStorageScheduleError(
            f"Record-storage benchmark listing exceeded {LISTING_TIMEOUT_SECONDS} "
            "seconds. Check benchmark initialization and retry."
        ) from error
    except OSError as error:
        raise RecordStorageScheduleError(
            f"Unable to execute Record-storage benchmark '{executable}': {error}. "
            "Build the Record-storage benchmark executable and retry."
        ) from error
    if result.returncode != 0:
        details = result.stderr.strip() or result.stdout.strip() or "no diagnostic output"
        raise RecordStorageScheduleError(
            f"Record-storage benchmark listing exited with {result.returncode}: {details}"
        )

    listing = []
    for row_index, line in enumerate(result.stdout.splitlines()):
        if line.strip():
            listing.append(normalize_listing_row(line, row_index))
    return listing


def validate_listing(actual: list[str]) -> int:
    expected = expected_listing()
    if len(actual) != len(expected):
        raise RecordStorageScheduleError(
            f"Record-storage benchmark listing has {len(actual)} row(s); expected "
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
        raise RecordStorageScheduleError(
            f"Record-storage benchmark listing row {mismatch_index} is "
            f"{actual[mismatch_index]!r}; expected {expected[mismatch_index]!r}. "
            "Register adjacent same-cell candidate triples in the six-permutation cycle."
        )
    return len(actual)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Validate Ulog's controlled structured Record storage registration schedule."
        )
    )
    parser.add_argument("executable", type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        row_count = validate_listing(read_listing(arguments.executable))
    except RecordStorageScheduleError as error:
        print(f"record-storage schedule validation failed: {error}", file=sys.stderr)
        return 1
    print(
        f"Validated six-permutation Record-storage schedule for {row_count} "
        "benchmark row(s)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
