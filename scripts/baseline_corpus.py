#!/usr/bin/env python3

import argparse
import copy
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
BASELINE_METADATA_PATH = PROJECT_ROOT / "docs" / "migration" / "baseline.md"
CORPUS_SCHEMA_VERSION = 1
PROBE_SCHEMA_VERSION = 1
CAPTURE_TOOL_VERSION = "ulog-baseline-corpus/1"
NORMALIZATION_VERSION = "ulog-baseline-normalization/1"
CANONICALIZATION = "ulog-json-v1"
CASE_ID_PATTERN = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
FEATURE_ID_PATTERN = re.compile(r"^[A-Z]{3,5}-[0-9]{3}[A-Z]?$")
NORMALIZATION_REPLACEMENTS = {
    "timestamp_local": "2000-01-02T03:04:05.123456",
    "timestamp_utc": "2000-01-02T03:04:05.123456Z",
    "source_path": "<source-path>",
    "process_id": "<process-id>",
    "thread_id": "<thread-id>",
    "platform": "<platform>",
}
CASE_PLATFORMS = {"portable", "windows", "linux", "macos"}
LOCAL_TIMESTAMP_PATTERN = re.compile(
    r"^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{6}$"
)
UTC_TIMESTAMP_PATTERN = re.compile(
    r"^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{6}Z$"
)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Capture and validate the committed Ulog baseline corpus."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    capture_parser = subparsers.add_parser(
        "capture", help="Run an offline probe against the pinned baseline checkout."
    )
    capture_parser.add_argument(
        "--baseline-source",
        required=True,
        type=Path,
        help="Path to the pinned baseline source checkout.",
    )
    capture_parser.add_argument(
        "--output", required=True, type=Path, help="Destination corpus JSON file."
    )
    capture_parser.add_argument(
        "--force",
        action="store_true",
        help="Atomically replace an existing corpus after all checks pass.",
    )
    capture_parser.add_argument(
        "probe_command",
        nargs=argparse.REMAINDER,
        help="Probe command to run after '--'.",
    )

    validate_parser = subparsers.add_parser(
        "validate", help="Validate committed corpus schema, provenance, and integrity."
    )
    validate_parser.add_argument(
        "--corpus-root",
        required=True,
        type=Path,
        help="Directory containing only committed corpus JSON files.",
    )
    validate_parser.add_argument(
        "--manifest",
        required=True,
        type=Path,
        help="Capability manifest supplying the allowed stable IDs.",
    )

    return parser.parse_args()


def run_git(source_path: Path, arguments: list[str]) -> subprocess.CompletedProcess[str]:
    git = shutil.which("git")
    if not git:
        raise RuntimeError("Git is not available on PATH. Install git and retry.")
    try:
        return subprocess.run(
            [git, "-C", str(source_path), *arguments],
            capture_output=True,
            check=False,
            text=True,
        )
    except OSError as error:
        raise RuntimeError(
            f"Unable to inspect the baseline checkout with git: {error}. "
            "Install git and retry."
        ) from error


def read_checkout_revision(source_path: Path) -> str:
    result = run_git(source_path, ["rev-parse", "--verify", "HEAD^{commit}"])

    if result.returncode != 0:
        detail = result.stderr.strip() or "git did not recognize the checkout"
        raise RuntimeError(
            f"Unable to read the baseline revision at '{source_path}': {detail}. "
            "Pass --baseline-source with a valid checkout and retry."
        )
    return result.stdout.strip().lower()


def read_worktree_changes(source_path: Path) -> str:
    result = run_git(source_path, ["status", "--short", "--untracked-files=all"])
    if result.returncode != 0:
        detail = result.stderr.strip() or "git status failed"
        raise RuntimeError(
            f"Unable to check the baseline worktree at '{source_path}': {detail}. "
            "Fix the checkout and retry."
        )
    return result.stdout.strip()


def reject_nonstandard_json(value: str) -> None:
    raise ValueError(f"non-standard JSON value '{value}' is not allowed")


def reject_floating_number(value: str) -> None:
    raise ValueError(
        f"floating JSON number '{value}' is not allowed; store exact fixture "
        "numbers as strings"
    )


def reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r} is not allowed")
        result[key] = value
    return result


def load_probe_output(output: str) -> dict[str, object]:
    try:
        document = json.loads(
            output,
            object_pairs_hook=reject_duplicate_keys,
            parse_constant=reject_nonstandard_json,
            parse_float=reject_floating_number,
        )
    except (json.JSONDecodeError, ValueError) as error:
        raise RuntimeError(
            f"Probe output is not valid strict JSON: {error}. "
            "Emit one JSON object matching probe schema version 1."
        ) from error
    if not isinstance(document, dict):
        raise RuntimeError("Probe output must be one JSON object, not another JSON type.")
    return document


def read_baseline_metadata() -> dict[str, str]:
    try:
        contents = BASELINE_METADATA_PATH.read_text(encoding="utf-8")
    except OSError as error:
        raise RuntimeError(
            f"Unable to read migration baseline metadata at {BASELINE_METADATA_PATH}: "
            f"{error}. Restore the file from the repository and retry."
        ) from error

    repositories = re.findall(r"^repository:\s*(\S+)\s*$", contents, re.MULTILINE)
    revisions = re.findall(r"^commit:\s*([0-9a-f]{40})\s*$", contents, re.MULTILINE)
    if len(repositories) != 1 or len(revisions) != 1:
        raise RuntimeError(
            f"{BASELINE_METADATA_PATH} must contain exactly one repository and one "
            "40-character lowercase commit entry. Restore the documented metadata "
            "and retry."
        )
    return {"repository": repositories[0], "revision": revisions[0]}


def load_json_file(path: Path) -> dict[str, object]:
    try:
        raw_contents = path.read_bytes()
    except OSError as error:
        raise RuntimeError(f"Unable to read corpus file '{path}': {error}.") from error
    if raw_contents.startswith(b"\xef\xbb\xbf"):
        raise RuntimeError(f"Corpus file '{path}' must not contain a UTF-8 BOM.")
    try:
        contents = raw_contents.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise RuntimeError(
            f"Corpus file '{path}' is not valid UTF-8 at byte {error.start}."
        ) from error
    try:
        document = json.loads(
            contents,
            object_pairs_hook=reject_duplicate_keys,
            parse_constant=reject_nonstandard_json,
            parse_float=reject_floating_number,
        )
    except (json.JSONDecodeError, ValueError) as error:
        raise RuntimeError(f"Corpus file '{path}' is not strict JSON: {error}.") from error
    if not isinstance(document, dict):
        raise RuntimeError(f"Corpus file '{path}' must contain one JSON object.")
    return document


def canonical_json(value: object) -> bytes:
    try:
        return json.dumps(
            value,
            ensure_ascii=False,
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
    except (TypeError, ValueError, UnicodeEncodeError) as error:
        raise RuntimeError(f"Corpus cannot be canonicalized as UTF-8 JSON: {error}.") from error


def calculate_integrity(document: dict[str, object]) -> str:
    material = copy.deepcopy(document)
    integrity = material.get("integrity")
    if not isinstance(integrity, dict) or "value" not in integrity:
        raise RuntimeError("Corpus integrity object must contain a value.")
    del integrity["value"]
    return hashlib.sha256(canonical_json(material)).hexdigest()


def load_manifest_ids(path: Path) -> set[str]:
    try:
        contents = path.read_text(encoding="utf-8")
    except OSError as error:
        raise RuntimeError(
            f"Unable to read capability manifest '{path}': {error}. "
            "Restore the manifest and retry."
        ) from error
    ids = set(
        re.findall(
            r"^\| `([A-Z]{3,5}-[0-9]{3}[A-Z]?)` \|",
            contents,
            re.MULTILINE,
        )
    )
    if not ids:
        raise RuntimeError(
            f"Capability manifest '{path}' contains no stable IDs. Restore the "
            "manifest and retry."
        )
    return ids


def validate_id_list(
    case_id: str,
    name: str,
    value: object,
    manifest_ids: set[str],
    allow_empty: bool,
) -> None:
    if not isinstance(value, list) or (not allow_empty and not value):
        qualifier = "a list" if allow_empty else "a non-empty list"
        raise RuntimeError(f"Corpus case '{case_id}' {name} must be {qualifier}.")
    if any(not isinstance(item, str) for item in value):
        raise RuntimeError(f"Corpus case '{case_id}' {name} must contain strings.")
    if value != sorted(set(value)):
        raise RuntimeError(
            f"Corpus case '{case_id}' {name} must be unique and lexicographically sorted."
        )
    unknown = sorted(set(value) - manifest_ids)
    if unknown:
        raise RuntimeError(
            f"Corpus case '{case_id}' {name} references unknown manifest IDs: "
            f"{', '.join(unknown)}. Add or correct the manifest entry first."
        )


def validate_committed_case(raw_case: object, manifest_ids: set[str]) -> str:
    if not isinstance(raw_case, dict):
        raise RuntimeError("Every committed corpus case must be a JSON object.")
    expected_keys = {
        "id",
        "feature_ids",
        "difference_ids",
        "platform",
        "observed",
    }
    if set(raw_case) != expected_keys:
        raise RuntimeError(
            "Every committed corpus case must contain exactly id, feature_ids, "
            "difference_ids, platform, and observed."
        )
    case_id = raw_case["id"]
    if not isinstance(case_id, str) or not CASE_ID_PATTERN.fullmatch(case_id):
        raise RuntimeError(f"Committed corpus case has invalid id {case_id!r}.")
    validate_id_list(
        case_id, "feature_ids", raw_case["feature_ids"], manifest_ids, False
    )
    validate_id_list(
        case_id, "difference_ids", raw_case["difference_ids"], manifest_ids, True
    )
    if any(
        feature_id.startswith(("DEF-", "DIFF-"))
        for feature_id in raw_case["feature_ids"]
    ):
        raise RuntimeError(
            f"Corpus case '{case_id}' must put DEF/DIFF IDs in difference_ids."
        )
    if any(
        not difference_id.startswith(("DEF-", "DIFF-"))
        for difference_id in raw_case["difference_ids"]
    ):
        raise RuntimeError(
            f"Corpus case '{case_id}' difference_ids may contain only DEF/DIFF IDs."
        )
    platform = raw_case["platform"]
    if not isinstance(platform, str) or platform not in CASE_PLATFORMS:
        supported = ", ".join(sorted(CASE_PLATFORMS))
        raise RuntimeError(
            f"Corpus case '{case_id}' platform must be one of: {supported}."
        )
    observed = raw_case["observed"]
    if not isinstance(observed, dict) or not observed:
        raise RuntimeError(f"Corpus case '{case_id}' observed must be a JSON object.")
    return case_id


def validate_corpus_document(
    path: Path,
    document: dict[str, object],
    baseline: dict[str, str],
    manifest_ids: set[str],
) -> int:
    if set(document) != {"schema_version", "provenance", "cases", "integrity"}:
        raise RuntimeError(
            f"Corpus file '{path}' must contain exactly schema_version, provenance, "
            "cases, and integrity."
        )
    if document["schema_version"] != CORPUS_SCHEMA_VERSION:
        raise RuntimeError(
            f"Corpus file '{path}' uses unsupported schema_version "
            f"{document['schema_version']!r}; expected {CORPUS_SCHEMA_VERSION}."
        )

    expected_provenance = {
        "baseline_document": "docs/migration/baseline.md",
        "capture_tool": CAPTURE_TOOL_VERSION,
        "normalization_profile": NORMALIZATION_VERSION,
        "repository": baseline["repository"],
        "revision": baseline["revision"],
    }
    if document["provenance"] != expected_provenance:
        raise RuntimeError(
            f"Corpus file '{path}' provenance does not match the migration baseline "
            "and tool versions. Recapture it with the documented workflow."
        )

    integrity = document["integrity"]
    if not isinstance(integrity, dict) or set(integrity) != {
        "algorithm",
        "canonicalization",
        "value",
    }:
        raise RuntimeError(
            f"Corpus file '{path}' integrity must contain exactly algorithm, "
            "canonicalization, and value."
        )
    if integrity["algorithm"] != "sha256" or integrity["canonicalization"] != CANONICALIZATION:
        raise RuntimeError(
            f"Corpus file '{path}' must use sha256 with {CANONICALIZATION}."
        )
    stored_integrity = integrity["value"]
    if not isinstance(stored_integrity, str) or not re.fullmatch(
        r"[0-9a-f]{64}", stored_integrity
    ):
        raise RuntimeError(
            f"Corpus file '{path}' integrity value must be 64 lowercase hex digits."
        )

    cases = document["cases"]
    if not isinstance(cases, list) or not cases:
        raise RuntimeError(f"Corpus file '{path}' must contain at least one case.")
    case_ids = [validate_committed_case(case, manifest_ids) for case in cases]
    if case_ids != sorted(set(case_ids)):
        raise RuntimeError(
            f"Corpus file '{path}' case ids must be unique and lexicographically sorted."
        )

    calculated_integrity = calculate_integrity(document)
    if stored_integrity != calculated_integrity:
        raise RuntimeError(
            f"Corpus file '{path}' integrity mismatch: stored {stored_integrity}, "
            f"calculated {calculated_integrity}. Recapture the corpus from the pinned "
            "baseline or restore the accidental edit."
        )
    return len(cases)


def validate(args: argparse.Namespace) -> int:
    if not args.corpus_root.is_dir():
        raise RuntimeError(
            f"Corpus root '{args.corpus_root}' is not a directory. Restore the "
            "committed corpus and retry."
        )
    unexpected_files = sorted(
        path
        for path in args.corpus_root.rglob("*")
        if path.is_file() and path.suffix.lower() != ".json"
    )
    if unexpected_files:
        raise RuntimeError(
            f"Corpus root '{args.corpus_root}' contains unexpected file "
            f"'{unexpected_files[0]}'. Keep documentation outside the corpus directory."
        )
    corpus_files = sorted(args.corpus_root.rglob("*.json"))
    if not corpus_files:
        raise RuntimeError(
            f"Corpus root '{args.corpus_root}' contains no JSON files. Restore at "
            "least one committed fixture."
        )

    baseline = read_baseline_metadata()
    manifest_ids = load_manifest_ids(args.manifest)
    case_count = sum(
        validate_corpus_document(
            path, load_json_file(path), baseline, manifest_ids
        )
        for path in corpus_files
    )
    print(
        f"Validated {len(corpus_files)} corpus file(s) with {case_count} case(s)"
    )
    return 0


def decode_json_pointer(path: str, case_id: str) -> list[str]:
    if not path.startswith("/"):
        raise RuntimeError(
            f"Probe case '{case_id}' normalization path {path!r} must be a non-root "
            "JSON Pointer beginning with '/'."
        )
    return [segment.replace("~1", "/").replace("~0", "~") for segment in path[1:].split("/")]


def resolve_json_pointer(value: object, path: list[str], case_id: str) -> str:
    current = value
    for segment in path:
        if isinstance(current, dict):
            if segment not in current:
                raise RuntimeError(
                    f"Probe case '{case_id}' normalization path has no {segment!r} member."
                )
            current = current[segment]
        elif isinstance(current, list):
            if not segment.isdecimal() or int(segment) >= len(current):
                raise RuntimeError(
                    f"Probe case '{case_id}' normalization path has invalid list "
                    f"index {segment!r}."
                )
            current = current[int(segment)]
        else:
            raise RuntimeError(
                f"Probe case '{case_id}' normalization path traverses a scalar value."
            )
    if not isinstance(current, str):
        raise RuntimeError(
            f"Probe case '{case_id}' normalization path must select a string value."
        )
    return current


def set_json_pointer(value: object, path: list[str], replacement: str) -> None:
    current = value
    for segment in path[:-1]:
        current = current[int(segment)] if isinstance(current, list) else current[segment]
    final_segment = path[-1]
    if isinstance(current, list):
        current[int(final_segment)] = replacement
    else:
        current[final_segment] = replacement


def find_occurrences(text: str, value: str) -> list[int]:
    positions = []
    offset = 0
    while True:
        position = text.find(value, offset)
        if position < 0:
            return positions
        positions.append(position)
        offset = position + len(value)


def validate_normalization_value(kind: str, value: str, case_id: str) -> None:
    if kind == "timestamp_local" and not LOCAL_TIMESTAMP_PATTERN.fullmatch(value):
        raise RuntimeError(
            f"Probe case '{case_id}' timestamp_local must have six fractional digits."
        )
    if kind == "timestamp_utc" and not UTC_TIMESTAMP_PATTERN.fullmatch(value):
        raise RuntimeError(
            f"Probe case '{case_id}' timestamp_utc must have six fractional digits and Z."
        )


def normalize_observed(
    observed: dict[str, object], rules: object, case_id: str
) -> dict[str, object]:
    if not isinstance(rules, list):
        raise RuntimeError(f"Probe case '{case_id}' normalization must be a list.")

    edits_by_path: dict[str, list[tuple[int, int, str]]] = {}
    decoded_paths: dict[str, list[str]] = {}
    for rule in rules:
        if not isinstance(rule, dict) or set(rule) != {
            "kind",
            "path",
            "value",
            "occurrence",
        }:
            raise RuntimeError(
                f"Probe case '{case_id}' normalization entries must contain exactly "
                "kind, path, value, and occurrence."
            )
        kind = rule["kind"]
        path = rule["path"]
        source = rule["value"]
        occurrence = rule["occurrence"]
        if not isinstance(kind, str) or kind not in NORMALIZATION_REPLACEMENTS:
            supported = ", ".join(NORMALIZATION_REPLACEMENTS)
            raise RuntimeError(
                f"Probe case '{case_id}' uses unknown normalization kind {kind!r}; "
                f"use one of: {supported}."
            )
        if not isinstance(path, str):
            raise RuntimeError(f"Probe case '{case_id}' normalization path must be a string.")
        if not isinstance(source, str) or not source:
            raise RuntimeError(
                f"Probe case '{case_id}' normalization value must be a non-empty string."
            )
        if isinstance(occurrence, bool) or not isinstance(occurrence, int) or occurrence < 0:
            raise RuntimeError(
                f"Probe case '{case_id}' normalization occurrence must be a "
                "non-negative integer."
            )
        validate_normalization_value(kind, source, case_id)
        decoded_path = decode_json_pointer(path, case_id)
        target = resolve_json_pointer(observed, decoded_path, case_id)
        positions = find_occurrences(target, source)
        if occurrence >= len(positions):
            raise RuntimeError(
                f"Probe case '{case_id}' normalization {path} selects occurrence "
                f"{occurrence}, but {source!r} occurs {len(positions)} time(s)."
            )
        start = positions[occurrence]
        edits_by_path.setdefault(path, []).append(
            (start, start + len(source), NORMALIZATION_REPLACEMENTS[kind])
        )
        decoded_paths[path] = decoded_path

    normalized = copy.deepcopy(observed)
    for path, edits in edits_by_path.items():
        edits.sort(key=lambda edit: edit[0])
        for previous, current in zip(edits, edits[1:]):
            if previous[1] > current[0]:
                raise RuntimeError(
                    f"Probe case '{case_id}' normalization rules overlap at {path}."
                )
        target = resolve_json_pointer(observed, decoded_paths[path], case_id)
        for start, end, replacement in reversed(edits):
            target = f"{target[:start]}{replacement}{target[end:]}"
        set_json_pointer(normalized, decoded_paths[path], target)
    return normalized


def normalize_probe_case(raw_case: object) -> dict[str, object]:
    if not isinstance(raw_case, dict):
        raise RuntimeError("Each probe case must be a JSON object.")
    expected_keys = {
        "id",
        "feature_ids",
        "difference_ids",
        "platform",
        "observed",
        "normalization",
    }
    if set(raw_case) != expected_keys:
        raise RuntimeError(
            "Each probe case must contain exactly id, feature_ids, difference_ids, "
            "platform, observed, and normalization."
        )

    case_id = raw_case["id"]
    if not isinstance(case_id, str) or not CASE_ID_PATTERN.fullmatch(case_id):
        raise RuntimeError(
            "Probe case id must use lowercase words separated by hyphens, for "
            f"example 'tskv-basic'; found {case_id!r}."
        )

    feature_ids = raw_case["feature_ids"]
    if not isinstance(feature_ids, list) or not feature_ids:
        raise RuntimeError(f"Probe case '{case_id}' must name at least one feature ID.")
    if any(
        not isinstance(feature_id, str)
        or not FEATURE_ID_PATTERN.fullmatch(feature_id)
        for feature_id in feature_ids
    ):
        raise RuntimeError(
            f"Probe case '{case_id}' has an invalid feature ID; use manifest IDs "
            "such as FMT-002."
        )
    if len(set(feature_ids)) != len(feature_ids):
        raise RuntimeError(f"Probe case '{case_id}' repeats a feature ID.")
    if any(feature_id.startswith(("DEF-", "DIFF-")) for feature_id in feature_ids):
        raise RuntimeError(
            f"Probe case '{case_id}' must put DEF/DIFF IDs in difference_ids."
        )

    difference_ids = raw_case["difference_ids"]
    if not isinstance(difference_ids, list):
        raise RuntimeError(f"Probe case '{case_id}' difference_ids must be a list.")
    if any(
        not isinstance(difference_id, str)
        or not FEATURE_ID_PATTERN.fullmatch(difference_id)
        or not difference_id.startswith(("DEF-", "DIFF-"))
        for difference_id in difference_ids
    ):
        raise RuntimeError(
            f"Probe case '{case_id}' difference_ids may contain only DEF/DIFF "
            "manifest IDs."
        )
    if len(set(difference_ids)) != len(difference_ids):
        raise RuntimeError(f"Probe case '{case_id}' repeats a difference ID.")

    platform = raw_case["platform"]
    if not isinstance(platform, str) or platform not in CASE_PLATFORMS:
        supported = ", ".join(sorted(CASE_PLATFORMS))
        raise RuntimeError(
            f"Probe case '{case_id}' platform must be one of: {supported}."
        )

    observed = raw_case["observed"]
    if not isinstance(observed, dict) or not observed:
        raise RuntimeError(f"Probe case '{case_id}' must contain observed values.")

    normalized_observed = normalize_observed(
        observed, raw_case["normalization"], case_id
    )

    return {
        "id": case_id,
        "feature_ids": sorted(feature_ids),
        "difference_ids": sorted(difference_ids),
        "platform": platform,
        "observed": normalized_observed,
    }


def build_corpus(
    probe_document: dict[str, object], baseline: dict[str, str]
) -> dict[str, object]:
    if set(probe_document) != {"probe_schema_version", "cases"}:
        raise RuntimeError(
            "Probe output must contain exactly probe_schema_version and cases."
        )
    if probe_document["probe_schema_version"] != PROBE_SCHEMA_VERSION:
        raise RuntimeError(
            f"Unsupported probe_schema_version {probe_document['probe_schema_version']!r}; "
            f"emit version {PROBE_SCHEMA_VERSION}."
        )
    raw_cases = probe_document["cases"]
    if not isinstance(raw_cases, list) or not raw_cases:
        raise RuntimeError("Probe output must contain at least one case.")

    cases = [normalize_probe_case(raw_case) for raw_case in raw_cases]
    case_ids = [case["id"] for case in cases]
    if len(set(case_ids)) != len(case_ids):
        raise RuntimeError("Probe output contains duplicate case ids.")
    cases.sort(key=lambda case: str(case["id"]))

    payload = {
        "schema_version": CORPUS_SCHEMA_VERSION,
        "provenance": {
            "baseline_document": "docs/migration/baseline.md",
            "capture_tool": CAPTURE_TOOL_VERSION,
            "normalization_profile": NORMALIZATION_VERSION,
            "repository": baseline["repository"],
            "revision": baseline["revision"],
        },
        "cases": cases,
    }
    payload["integrity"] = {
        "algorithm": "sha256",
        "canonicalization": CANONICALIZATION,
        "value": "",
    }
    payload["integrity"]["value"] = calculate_integrity(payload)
    return payload


def run_probe(source_path: Path, command: list[str]) -> str:
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        raise RuntimeError(
            "A probe command is required after '--'. Build the external probe and "
            "pass its executable plus arguments."
        )
    try:
        environment = os.environ.copy()
        environment.update({"LC_ALL": "C", "LANG": "C", "TZ": "UTC"})
        result = subprocess.run(
            command,
            cwd=source_path,
            capture_output=True,
            check=False,
            env=environment,
        )
    except OSError as error:
        raise RuntimeError(
            f"Unable to start probe command {command[0]!r}: {error}. "
            "Build the probe for a supported host and retry."
        ) from error
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        detail = detail or "probe produced no diagnostic"
        raise RuntimeError(
            f"Probe command failed with exit code {result.returncode}: {detail}"
        )
    try:
        return result.stdout.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise RuntimeError(
            f"Probe stdout is not valid UTF-8 at byte {error.start}. Configure the "
            "probe for UTF-8 output and retry."
        ) from error


def write_corpus(path: Path, corpus: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    contents = json.dumps(
        corpus, ensure_ascii=False, allow_nan=False, indent=2, sort_keys=True
    )
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            "w",
            delete=False,
            dir=path.parent,
            encoding="utf-8",
            newline="\n",
        ) as output:
            output.write(contents)
            output.write("\n")
            temporary_path = Path(output.name)
        temporary_path.replace(path)
    finally:
        if temporary_path and temporary_path.exists():
            temporary_path.unlink()


def capture(args: argparse.Namespace) -> int:
    if args.output.exists() and not args.force:
        raise RuntimeError(
            f"Output corpus '{args.output}' already exists. Pass --force only after "
            "reviewing the intended evidence update."
        )
    baseline = read_baseline_metadata()
    revision = read_checkout_revision(args.baseline_source)
    if revision != baseline["revision"]:
        raise RuntimeError(
            f"Baseline checkout revision is {revision}, but "
            f"{baseline['revision']} is required. Run 'git checkout "
            f"{baseline['revision']}' in '{args.baseline_source}' and retry."
        )
    worktree_changes = read_worktree_changes(args.baseline_source)
    if worktree_changes:
        raise RuntimeError(
            "Baseline checkout has tracked changes or untracked files:\n"
            f"{worktree_changes}\n"
            "Please commit, stash, or revert tracked changes and remove untracked "
            "source files so the checkout exactly matches the pinned revision, "
            "then retry."
        )
    probe_output = run_probe(args.baseline_source, args.probe_command)
    corpus = build_corpus(load_probe_output(probe_output), baseline)
    write_corpus(args.output, corpus)
    print(f"Captured {len(corpus['cases'])} case(s) in {args.output}")
    return 0


def main() -> int:
    args = parse_arguments()
    try:
        if args.command == "capture":
            return capture(args)
        if args.command == "validate":
            return validate(args)
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
