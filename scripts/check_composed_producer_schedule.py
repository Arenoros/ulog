#!/usr/bin/env python3

import argparse
import re
import subprocess
import sys
from pathlib import Path


PRODUCER_COUNTS = (1, 2, 4, 8, 16, 32)
RECORD_SIZES = (64, 256, 1024, 4096, 16384)
OCCUPANCIES = ("empty", "partial", "near-full", "saturated")
CONTROLLED_REPETITIONS = 7
LISTING_TIMEOUT_SECONDS = 30
IDENTITY = re.compile(
    r"^(?P<identity>UlogComposedProducer/composed-producer/"
    r"producers:(?:1|2|4|8|16|32)/"
    r"record_bytes:(?:64|256|1024|4096|16384)/"
    r"occupancy:(?:empty|partial|near-full|saturated)/"
    r"repetition:[0-6])(?:/.*)?$"
)


class ScheduleError(RuntimeError):
    pass


def expected_listing() -> list[str]:
    return [
        f"UlogComposedProducer/composed-producer/producers:{producers}/"
        f"record_bytes:{record_size}/occupancy:{occupancy}/repetition:{repetition}"
        for repetition in range(CONTROLLED_REPETITIONS)
        for producers in PRODUCER_COUNTS
        for record_size in RECORD_SIZES
        for occupancy in OCCUPANCIES
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
        raise ScheduleError(
            f"Composed-producer listing exceeded {LISTING_TIMEOUT_SECONDS} seconds. "
            "Check that list mode only registers workloads and retry."
        ) from error
    except OSError as error:
        raise ScheduleError(
            f"Unable to execute composed-producer benchmark {executable!s}: {error}."
        ) from error
    if result.returncode != 0:
        details = result.stderr.strip() or result.stdout.strip() or "no diagnostic output"
        raise ScheduleError(
            f"Composed-producer listing exited with {result.returncode}: {details}"
        )
    listing = []
    for row_index, line in enumerate(result.stdout.splitlines()):
        if not line.strip():
            continue
        match = IDENTITY.fullmatch(line.strip())
        if not match:
            raise ScheduleError(
                f"Composed-producer listing row {row_index} has unrecognized identity "
                f"{line.strip()!r}."
            )
        listing.append(match.group("identity"))
    return listing


def validate_listing(actual: list[str]) -> int:
    expected = expected_listing()
    if actual != expected:
        if len(actual) != len(expected):
            detail = f"found {len(actual)} row(s), expected {len(expected)}"
        else:
            mismatch = next(
                index
                for index, (actual_name, expected_name) in enumerate(
                    zip(actual, expected, strict=True)
                )
                if actual_name != expected_name
            )
            detail = (
                f"row {mismatch} is {actual[mismatch]!r}, expected "
                f"{expected[mismatch]!r}"
            )
        raise ScheduleError(
            f"Composed-producer schedule differs from the common workload matrix: {detail}."
        )
    return len(actual)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate composed-producer common workload registration."
    )
    parser.add_argument("executable", type=Path)
    arguments = parser.parse_args()
    try:
        rows = validate_listing(read_listing(arguments.executable))
    except ScheduleError as error:
        print(f"composed-producer schedule validation failed: {error}", file=sys.stderr)
        return 1
    print(f"Validated single-candidate composed-producer schedule for {rows} row(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
