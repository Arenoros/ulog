#!/usr/bin/env python3

import os
import shutil
import sys
from pathlib import Path

from benchmark_results import BenchmarkResultsError, validate_result_file


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: collect_benchmark_results.py <destination.json>", file=sys.stderr)
        return 2

    conan_home = Path(os.environ.get("CONAN_HOME", Path.home() / ".conan2"))
    candidates = sorted(conan_home.glob("p/b/**/benchmark-results.json"))
    if not candidates:
        print(f"no benchmark-results.json found below {conan_home}", file=sys.stderr)
        return 1

    newest_result = max(candidates, key=lambda path: path.stat().st_mtime_ns)
    try:
        validate_result_file(newest_result)
    except BenchmarkResultsError as error:
        print(
            f"newest benchmark result '{newest_result}' is invalid: {error} "
            "Fix the ulog-workload-results/3 output and retry.",
            file=sys.stderr,
        )
        return 1

    destination = Path(sys.argv[1])
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(newest_result, destination)
    print(f"copied {newest_result} to {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
