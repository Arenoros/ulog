#!/usr/bin/env python3

import argparse
import subprocess
import sys
from pathlib import Path

from frontend_results import MODE_REPETITIONS, ROW_PATTERN, expected_identities


LISTING_TIMEOUT_SECONDS = 10


class FrontendScheduleError(RuntimeError):
    pass


def expected_listing() -> list[str]:
    return expected_identities(MODE_REPETITIONS["controlled"])


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
        raise FrontendScheduleError(
            f"Frontend listing exceeded {LISTING_TIMEOUT_SECONDS} seconds. "
            "Check that list mode only registers workloads and retry."
        ) from error
    except OSError as error:
        raise FrontendScheduleError(
            f"Unable to execute frontend benchmark {executable!s}: {error}."
        ) from error
    if result.returncode != 0:
        details = result.stderr.strip() or result.stdout.strip() or "no diagnostic output"
        raise FrontendScheduleError(
            f"Frontend listing exited with {result.returncode}: {details}"
        )

    listing = []
    for row_index, line in enumerate(result.stdout.splitlines()):
        if not line.strip():
            continue
        match = ROW_PATTERN.fullmatch(line.strip())
        if not match:
            raise FrontendScheduleError(
                f"Frontend listing row {row_index} has unrecognized identity "
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
        raise FrontendScheduleError(
            f"Frontend schedule must be the bounded repetition-major matrix: {detail}."
        )
    return len(actual)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate the controlled frontend benchmark registration schedule."
    )
    parser.add_argument("executable", type=Path)
    arguments = parser.parse_args()
    try:
        rows = validate_listing(read_listing(arguments.executable))
    except FrontendScheduleError as error:
        print(f"frontend schedule validation failed: {error}", file=sys.stderr)
        return 1
    print(f"Validated bounded repetition-major frontend schedule for {rows} row(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
