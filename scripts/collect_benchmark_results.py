#!/usr/bin/env python3

import os
import shutil
import sys
from pathlib import Path
from typing import Callable

from benchmark_results import BenchmarkResultsError, validate_result_file
from record_storage_results import validate_result_file as validate_record_storage_result_file


Validator = Callable[[Path], tuple[int, int, int]]


def collect_newest_result(
    conan_home: Path,
    filename: str,
    destination: Path,
    validator: Validator,
    protocol: str,
) -> bool:
    candidates = sorted(conan_home.glob(f"p/b/**/{filename}"))
    if not candidates:
        print(f"no {filename} found below {conan_home}", file=sys.stderr)
        return False

    newest_result = max(candidates, key=lambda path: path.stat().st_mtime_ns)
    try:
        validator(newest_result)
    except BenchmarkResultsError as error:
        print(
            f"newest benchmark result '{newest_result}' is invalid: {error} "
            f"Fix the {protocol} output and retry.",
            file=sys.stderr,
        )
        return False

    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(newest_result, destination)
    print(f"copied {newest_result} to {destination}")
    return True


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print(
            "usage: collect_benchmark_results.py <reservation-destination.json> "
            "[record-storage-destination.json]",
            file=sys.stderr,
        )
        return 2

    conan_home = Path(os.environ.get("CONAN_HOME", Path.home() / ".conan2"))
    if not collect_newest_result(
        conan_home,
        "benchmark-results.json",
        Path(sys.argv[1]),
        validate_result_file,
        "ulog-workload-results/3",
    ):
        return 1

    if len(sys.argv) == 3 and not collect_newest_result(
        conan_home,
        "record-storage-results.json",
        Path(sys.argv[2]),
        validate_record_storage_result_file,
        "ulog-record-storage-results/1",
    ):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
