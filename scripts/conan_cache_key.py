#!/usr/bin/env python3

import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


def hash_profile(profile: object) -> str:
    canonical_profile = json.dumps(
        profile, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    )
    return hashlib.sha256(canonical_profile.encode("utf-8")).hexdigest()[:16]


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: conan_cache_key.py <build-type>", file=sys.stderr)
        return 2

    conan = shutil.which("conan")
    if not conan:
        print(
            "Conan is not available on PATH. Install the pinned CI Conan version "
            "before computing the cache key.",
            file=sys.stderr,
        )
        return 1

    command = [
        conan,
        "profile",
        "show",
        "-pr:h=conan/profiles/cpp20",
        "-pr:b=default",
        f"-s:h=build_type={sys.argv[1]}",
        "--format=json",
    ]
    result = subprocess.run(command, capture_output=True, check=False, text=True)
    if result.returncode != 0:
        print(result.stderr, file=sys.stderr, end="")
        print(
            "Unable to resolve the Conan profiles for the cache key. "
            "Run 'conan profile detect --force' and retry.",
            file=sys.stderr,
        )
        return result.returncode

    try:
        profile = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        print(f"Conan returned an invalid JSON profile: {error}", file=sys.stderr)
        return 1

    profile_hash = hash_profile(profile)
    github_output = os.environ.get("GITHUB_OUTPUT")
    if github_output:
        with Path(github_output).open("a", encoding="utf-8") as output:
            print(f"hash={profile_hash}", file=output)
    print(profile_hash)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
