#!/usr/bin/env python3

import argparse
import json
import re
import sys
from pathlib import Path

from migration_manifest import (
    FEATURE_ID_PATTERN,
    load_baseline_metadata,
    load_manifest_ids,
    read_utf8,
)


SCHEMA_VERSION = 1
SCHEMA_MARKER_PATTERN = re.compile(
    r"^<!-- ulog-non-format-scenarios-schema: ([0-9]+) -->$", re.MULTILINE
)
SCENARIO_FENCE = "```json ulog-non-format-scenario"
SCENARIO_FENCE_PATTERN = re.compile(
    r"^```json ulog-non-format-scenario\n(.*?)^```$", re.MULTILINE | re.DOTALL
)
BASELINE_FENCE = "```text ulog-non-format-baseline"
BASELINE_FENCE_PATTERN = re.compile(
    r"^```text ulog-non-format-baseline\n"
    r"repository: (\S+)\n"
    r"commit: ([0-9a-f]{40})\n"
    r"```$",
    re.MULTILINE,
)
SCENARIO_ID_PATTERN = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
INPUT_NAME_PATTERN = re.compile(r"^[a-z][a-z0-9]*(?:_[a-z0-9]+)*$")
SCENARIO_KEYS = {
    "id",
    "category",
    "feature_ids",
    "difference_ids",
    "inputs",
    "action",
    "observable_result",
    "parity",
    "determinism",
}
CATEGORY_ANCHOR_IDS = {
    "compile-time-filtering": {"API-005"},
    "runtime-filtering": {"API-001", "LVL-001"},
    "default-logger": {"DFL-001"},
    "limited-logging": {"LIM-001", "LIM-002"},
    "dynamic-debug": {"DYN-001", "DYN-002"},
    "reopen": {"LFC-004"},
    "flush": {"LFC-002"},
    "startup-forwarding": {"BST-001", "BST-002"},
}
DETERMINISM_METHODS = {"barrier", "compile-probe", "injected-clock", "state"}
FORBIDDEN_SYNCHRONIZATION = re.compile(
    r"(?:\b(?:sleep|sleep_for|sleep_until)\b|time\.sleep|std::this_thread::sleep_)",
    re.IGNORECASE,
)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate the committed non-format parity scenario catalog."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    validate_parser = subparsers.add_parser(
        "validate", help="Validate the catalog using committed metadata only."
    )
    validate_parser.add_argument("--catalog", required=True, type=Path)
    validate_parser.add_argument("--manifest", required=True, type=Path)
    validate_parser.add_argument("--baseline", required=True, type=Path)
    return parser.parse_args()


def reject_nonstandard_json(value: str) -> None:
    raise ValueError(f"non-standard JSON value '{value}' is not allowed")


def reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r} is not allowed")
        result[key] = value
    return result


def parse_strict_json(contents: str, block_index: int) -> object:
    try:
        return json.loads(
            contents,
            object_pairs_hook=reject_duplicate_keys,
            parse_constant=reject_nonstandard_json,
        )
    except (json.JSONDecodeError, ValueError) as error:
        raise RuntimeError(
            f"Scenario block {block_index} is not strict JSON: {error}. "
            "Fix the fenced object and retry."
        ) from error


def read_catalog(path: Path) -> str:
    contents = read_utf8(path, "scenario catalog")
    if "\r" in contents:
        raise RuntimeError(
            f"Scenario catalog '{path}' must use LF line endings. Convert CRLF "
            "or CR to LF and retry."
        )
    if not contents.endswith("\n"):
        raise RuntimeError(
            f"Scenario catalog '{path}' must end with LF. Add the final "
            "newline and retry."
        )
    return contents


def extract_scenario_blocks(contents: str) -> list[str]:
    marker_versions = SCHEMA_MARKER_PATTERN.findall(contents)
    if len(marker_versions) != 1:
        raise RuntimeError(
            "Scenario catalog must contain exactly one schema marker "
            f"'<!-- ulog-non-format-scenarios-schema: {SCHEMA_VERSION} -->'."
        )
    version = int(marker_versions[0])
    if version != SCHEMA_VERSION:
        raise RuntimeError(
            f"Scenario catalog uses unsupported schema version {version}; "
            f"change the marker to version {SCHEMA_VERSION} and retry."
        )

    for line in contents.splitlines():
        if line.strip().startswith(SCENARIO_FENCE) and line != SCENARIO_FENCE:
            raise RuntimeError(
                f"Scenario catalog contains unknown scenario fence '{line}'. "
                f"Use exactly '{SCENARIO_FENCE}'."
            )

    blocks = [match.group(1) for match in SCENARIO_FENCE_PATTERN.finditer(contents)]
    opener_count = sum(line == SCENARIO_FENCE for line in contents.splitlines())
    if opener_count != len(blocks):
        raise RuntimeError(
            "Scenario catalog contains an unclosed or malformed scenario fence. "
            f"Close every '{SCENARIO_FENCE}' block with '```'."
        )
    if not blocks:
        raise RuntimeError(
            f"Scenario catalog contains no '{SCENARIO_FENCE}' blocks. Add at "
            "least one scenario and retry."
        )
    return blocks


def load_catalog_baseline(contents: str) -> dict[str, str]:
    for line in contents.splitlines():
        if line.strip().startswith(BASELINE_FENCE) and line != BASELINE_FENCE:
            raise RuntimeError(
                f"Scenario catalog contains unknown baseline fence '{line}'. "
                f"Use exactly '{BASELINE_FENCE}'."
            )
    matches = BASELINE_FENCE_PATTERN.findall(contents)
    opener_count = sum(line == BASELINE_FENCE for line in contents.splitlines())
    if len(matches) != 1 or opener_count != 1:
        raise RuntimeError(
            "Scenario catalog must contain exactly one baseline declaration with "
            f"'{BASELINE_FENCE}', repository, commit, and a closing '```'."
        )
    repository, revision = matches[0]
    return {"repository": repository, "revision": revision}


def require_exact_keys(
    scenario_id: str, description: str, value: object, expected: set[str]
) -> dict[str, object]:
    if not isinstance(value, dict):
        raise RuntimeError(
            f"Scenario '{scenario_id}' {description} must be an object."
        )
    missing = sorted(expected - value.keys())
    unexpected = sorted(value.keys() - expected)
    if missing:
        raise RuntimeError(
            f"Scenario '{scenario_id}' {description} is missing keys: "
            f"{', '.join(missing)}."
        )
    if unexpected:
        raise RuntimeError(
            f"Scenario '{scenario_id}' {description} has unexpected keys: "
            f"{', '.join(unexpected)}."
        )
    return value


def require_string_list(
    scenario_id: str,
    name: str,
    value: object,
    *,
    allow_empty: bool = False,
    sorted_unique: bool = False,
) -> list[str]:
    if not isinstance(value, list) or (not allow_empty and not value):
        qualifier = "a string list" if allow_empty else "a non-empty string list"
        raise RuntimeError(f"Scenario '{scenario_id}' {name} must be {qualifier}.")
    if any(not isinstance(item, str) or not item.strip() for item in value):
        raise RuntimeError(
            f"Scenario '{scenario_id}' {name} must contain non-empty strings."
        )
    if sorted_unique and value != sorted(set(value)):
        raise RuntimeError(
            f"Scenario '{scenario_id}' {name} must be unique and sorted."
        )
    return value


def validate_ids(
    scenario_id: str,
    feature_ids_raw: object,
    difference_ids_raw: object,
    manifest_ids: set[str],
) -> tuple[list[str], list[str]]:
    feature_ids = require_string_list(
        scenario_id, "feature_ids", feature_ids_raw, sorted_unique=True
    )
    difference_ids = require_string_list(
        scenario_id,
        "difference_ids",
        difference_ids_raw,
        allow_empty=True,
        sorted_unique=True,
    )
    invalid = sorted(
        item
        for item in [*feature_ids, *difference_ids]
        if not FEATURE_ID_PATTERN.fullmatch(item)
    )
    if invalid:
        raise RuntimeError(
            f"Scenario '{scenario_id}' has malformed manifest IDs: {', '.join(invalid)}."
        )
    if any(item.startswith(("DEF-", "DIFF-")) for item in feature_ids):
        raise RuntimeError(
            f"Scenario '{scenario_id}' feature_ids cannot contain DEF-/DIFF- IDs."
        )
    if any(not item.startswith(("DEF-", "DIFF-")) for item in difference_ids):
        raise RuntimeError(
            f"Scenario '{scenario_id}' difference_ids must contain only "
            "DEF-/DIFF- IDs."
        )
    unknown = sorted((set(feature_ids) | set(difference_ids)) - manifest_ids)
    if unknown:
        raise RuntimeError(
            f"Scenario '{scenario_id}' references unknown manifest IDs: "
            f"{', '.join(unknown)}."
        )
    return feature_ids, difference_ids


def validate_observable_result(
    scenario_id: str,
    parity: object,
    difference_ids: list[str],
    raw_result: object,
) -> None:
    if parity == "same":
        if difference_ids:
            raise RuntimeError(
                f"Scenario '{scenario_id}' parity 'same' requires no difference IDs."
            )
        result = require_exact_keys(
            scenario_id, "observable_result", raw_result, {"baseline_and_ulog"}
        )
        require_string_list(
            scenario_id,
            "observable_result.baseline_and_ulog",
            result["baseline_and_ulog"],
        )
        return
    if parity == "intentional":
        if not difference_ids:
            raise RuntimeError(
                f"Scenario '{scenario_id}' parity 'intentional' requires difference IDs."
            )
        result = require_exact_keys(
            scenario_id, "observable_result", raw_result, {"baseline", "ulog"}
        )
        baseline = require_string_list(
            scenario_id, "observable_result.baseline", result["baseline"]
        )
        ulog = require_string_list(
            scenario_id, "observable_result.ulog", result["ulog"]
        )
        if baseline == ulog:
            raise RuntimeError(
                f"Scenario '{scenario_id}' intentional baseline and Ulog results "
                "must differ."
            )
        return
    if parity == "decision-point":
        if not difference_ids:
            raise RuntimeError(
                f"Scenario '{scenario_id}' parity 'decision-point' requires "
                "difference IDs."
            )
        result = require_exact_keys(
            scenario_id, "observable_result", raw_result, {"baseline", "decision"}
        )
        require_string_list(
            scenario_id, "observable_result.baseline", result["baseline"]
        )
        require_string_list(
            scenario_id, "observable_result.decision", result["decision"]
        )
        return
    raise RuntimeError(
        f"Scenario '{scenario_id}' parity must be 'same', 'intentional', or "
        "'decision-point'."
    )


def validate_scenario(
    raw_scenario: object, block_index: int, manifest_ids: set[str]
) -> tuple[str, str, set[str]]:
    provisional_id = f"block-{block_index}"
    scenario = require_exact_keys(
        provisional_id, "object", raw_scenario, SCENARIO_KEYS
    )
    scenario_id = scenario["id"]
    if not isinstance(scenario_id, str) or not SCENARIO_ID_PATTERN.fullmatch(
        scenario_id
    ):
        raise RuntimeError(
            f"Scenario block {block_index} id must use lowercase hyphenated words."
        )
    category = scenario["category"]
    if not isinstance(category, str) or category not in CATEGORY_ANCHOR_IDS:
        allowed = ", ".join(sorted(CATEGORY_ANCHOR_IDS))
        raise RuntimeError(
            f"Scenario '{scenario_id}' category must be one of: {allowed}."
        )
    feature_ids, difference_ids = validate_ids(
        scenario_id,
        scenario["feature_ids"],
        scenario["difference_ids"],
        manifest_ids,
    )

    inputs = scenario["inputs"]
    if not isinstance(inputs, dict) or not inputs:
        raise RuntimeError(
            f"Scenario '{scenario_id}' inputs must be a non-empty object."
        )
    invalid_input_names = sorted(
        key
        for key in inputs
        if not isinstance(key, str) or not INPUT_NAME_PATTERN.fullmatch(key)
    )
    if invalid_input_names:
        raise RuntimeError(
            f"Scenario '{scenario_id}' has invalid input names: "
            f"{', '.join(map(str, invalid_input_names))}. Use lower_snake_case."
        )
    require_string_list(scenario_id, "action", scenario["action"])
    validate_observable_result(
        scenario_id, scenario["parity"], difference_ids, scenario["observable_result"]
    )

    determinism = require_exact_keys(
        scenario_id,
        "determinism",
        scenario["determinism"],
        {"methods", "controls"},
    )
    methods = require_string_list(
        scenario_id,
        "determinism.methods",
        determinism["methods"],
        sorted_unique=True,
    )
    unknown_methods = sorted(set(methods) - DETERMINISM_METHODS)
    if unknown_methods:
        raise RuntimeError(
            f"Scenario '{scenario_id}' has unknown determinism methods: "
            f"{', '.join(unknown_methods)}."
        )
    require_string_list(
        scenario_id, "determinism.controls", determinism["controls"]
    )

    serialized = json.dumps(scenario, ensure_ascii=False, sort_keys=True)
    if FORBIDDEN_SYNCHRONIZATION.search(serialized):
        raise RuntimeError(
            f"Scenario '{scenario_id}' uses forbidden elapsed-time "
            "synchronization. Replace it with a barrier, injected clock, or "
            "state observation."
        )
    return scenario_id, category, set(feature_ids)


def validate(args: argparse.Namespace) -> int:
    contents = read_catalog(args.catalog)
    baseline = load_baseline_metadata(args.baseline)
    catalog_baseline = load_catalog_baseline(contents)
    if catalog_baseline != baseline:
        raise RuntimeError(
            f"Scenario catalog baseline {catalog_baseline!r} does not match "
            f"migration baseline {baseline!r}. Copy the exact repository and "
            "commit declaration from the migration baseline and retry."
        )
    manifest_ids = load_manifest_ids(args.manifest)
    raw_blocks = extract_scenario_blocks(contents)

    scenarios = [
        validate_scenario(
            parse_strict_json(block, index), index, manifest_ids
        )
        for index, block in enumerate(raw_blocks, start=1)
    ]
    scenario_ids = [scenario_id for scenario_id, _, _ in scenarios]
    if scenario_ids != sorted(set(scenario_ids)):
        raise RuntimeError("Scenario IDs must be unique and sorted.")

    covered_categories = {category for _, category, _ in scenarios}
    missing_categories = sorted(CATEGORY_ANCHOR_IDS.keys() - covered_categories)
    if missing_categories:
        raise RuntimeError(
            f"Scenario catalog is missing required categories: "
            f"{', '.join(missing_categories)}."
        )
    for category, anchor_ids in CATEGORY_ANCHOR_IDS.items():
        covered_ids = set().union(
            *(
                feature_ids
                for _, item_category, feature_ids in scenarios
                if item_category == category
            )
        )
        missing_anchor_ids = sorted(anchor_ids - covered_ids)
        if missing_anchor_ids:
            raise RuntimeError(
                f"Scenario category '{category}' is missing anchor feature IDs: "
                f"{', '.join(missing_anchor_ids)}."
            )

    print(
        f"Validated {len(scenarios)} non-format scenario(s) covering "
        f"{len(covered_categories)} categories"
    )
    return 0


def main() -> int:
    args = parse_arguments()
    try:
        if args.command == "validate":
            return validate(args)
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
