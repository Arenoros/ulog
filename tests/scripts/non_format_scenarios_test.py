import ast
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = PROJECT_ROOT / "scripts" / "non_format_scenarios.py"
MIGRATION_MANIFEST = PROJECT_ROOT / "scripts" / "migration_manifest.py"
CATALOG = (
    PROJECT_ROOT / "docs" / "migration" / "non-format-parity-scenarios.md"
)
MANIFEST = PROJECT_ROOT / "docs" / "migration" / "capability-manifest.md"
BASELINE = PROJECT_ROOT / "docs" / "migration" / "baseline.md"

FAKE_REPOSITORY = "reference"
FAKE_REVISION = "0123456789abcdef0123456789abcdef01234567"
ANCHOR_IDS = {
    "compile-time-filtering": ("API-005",),
    "default-logger": ("DFL-001",),
    "dynamic-debug": ("DYN-001", "DYN-002"),
    "flush": ("LFC-002",),
    "limited-logging": ("LIM-001", "LIM-002"),
    "reopen": ("LFC-004",),
    "runtime-filtering": ("API-001", "LVL-001"),
    "startup-forwarding": ("BST-001", "BST-002"),
}
MANIFEST_IDS = tuple(sorted({item for ids in ANCHOR_IDS.values() for item in ids}))


def make_scenario(category: str) -> dict[str, object]:
    return {
        "id": f"{category}-case",
        "category": category,
        "feature_ids": list(ANCHOR_IDS[category]),
        "difference_ids": [],
        "inputs": {"fixed_value": 1},
        "action": ["Invoke the capability with the fixed input."],
        "observable_result": {
            "baseline_and_ulog": ["The exact state snapshot equals the oracle."]
        },
        "parity": "same",
        "determinism": {
            "methods": ["state"],
            "controls": ["Read the state snapshot without elapsed-time waits."],
        },
    }


def scenario_block(value: object) -> str:
    return (
        "```json ulog-non-format-scenario\n"
        f"{json.dumps(value, ensure_ascii=False, indent=2)}\n"
        "```\n"
    )


def complete_scenarios() -> list[dict[str, object]]:
    return [make_scenario(category) for category in sorted(ANCHOR_IDS)]


def catalog(
    scenarios: list[object],
    *,
    schema_version: int = 1,
    repository: str = FAKE_REPOSITORY,
    revision: str = FAKE_REVISION,
) -> str:
    blocks = "\n".join(scenario_block(value) for value in scenarios)
    return f"""# Non-format parity scenarios

<!-- ulog-non-format-scenarios-schema: {schema_version} -->

```text ulog-non-format-baseline
repository: {repository}
commit: {revision}
```

{blocks}"""


def write_validation_fixture(
    root: Path,
    catalog_contents: str | bytes,
    manifest_ids: tuple[str, ...],
    baseline_repository: str,
    baseline_revision: str,
) -> tuple[Path, Path, Path]:
    catalog_path = root / "scenarios.md"
    manifest_path = root / "manifest.md"
    baseline_path = root / "baseline.md"
    if isinstance(catalog_contents, bytes):
        catalog_path.write_bytes(catalog_contents)
    else:
        catalog_path.write_bytes(catalog_contents.encode("utf-8"))
    manifest_path.write_bytes(
        (
            "\n".join(
                f"| `{item}` | source | behavior | owner | test |"
                for item in manifest_ids
            )
            + "\n"
        ).encode("utf-8")
    )
    baseline_path.write_bytes(
        (
            "# Baseline\n\n```text\n"
            f"repository: {baseline_repository}\n"
            f"commit: {baseline_revision}\n"
            "```\n"
        ).encode("utf-8")
    )
    return catalog_path, manifest_path, baseline_path


class NonFormatScenariosTest(unittest.TestCase):
    def run_validator(
        self,
        catalog_contents: str | bytes,
        *,
        manifest_ids: tuple[str, ...] = MANIFEST_IDS,
        baseline_repository: str = FAKE_REPOSITORY,
        baseline_revision: str = FAKE_REVISION,
        path_environment: str | None = None,
    ) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            catalog_path, manifest_path, baseline_path = write_validation_fixture(
                temp_path,
                catalog_contents,
                manifest_ids,
                baseline_repository,
                baseline_revision,
            )
            environment = os.environ.copy()
            if path_environment is not None:
                environment["PATH"] = path_environment
            return subprocess.run(
                [
                    sys.executable,
                    str(VALIDATOR),
                    "validate",
                    "--catalog",
                    str(catalog_path),
                    "--manifest",
                    str(manifest_path),
                    "--baseline",
                    str(baseline_path),
                ],
                capture_output=True,
                check=False,
                cwd=temp_path,
                env=environment,
                text=True,
            )

    def test_complete_catalog_is_valid_without_external_tools(self):
        with tempfile.TemporaryDirectory() as empty_path:
            result = self.run_validator(
                catalog(complete_scenarios()), path_environment=empty_path
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout.strip(),
            "Validated 8 non-format scenario(s) covering 8 categories",
        )

    def test_validator_is_confined_to_explicit_offline_inputs(self):
        allowed_imports = {
            "argparse",
            "json",
            "migration_manifest",
            "pathlib",
            "re",
            "sys",
        }
        for source_path in (VALIDATOR, MIGRATION_MANIFEST):
            tree = ast.parse(source_path.read_text(encoding="utf-8"))
            imports = {
                alias.name.split(".", 1)[0]
                for node in ast.walk(tree)
                if isinstance(node, ast.Import)
                for alias in node.names
            }
            imports.update(
                node.module.split(".", 1)[0]
                for node in ast.walk(tree)
                if isinstance(node, ast.ImportFrom) and node.module
            )
            self.assertLessEqual(imports, allowed_imports, source_path)

        audit_program = r"""
import os
import sys
from pathlib import Path

module_path, catalog_path, manifest_path, baseline_path = sys.argv[1:]
sys.path.insert(0, module_path)
import non_format_scenarios

sys.argv = [
    str(Path(module_path) / "non_format_scenarios.py"),
    "validate",
    "--catalog",
    catalog_path,
    "--manifest",
    manifest_path,
    "--baseline",
    baseline_path,
]
arguments = non_format_scenarios.parse_arguments()

allowed_paths = {
    Path(catalog_path).resolve(),
    Path(manifest_path).resolve(),
    Path(baseline_path).resolve(),
}

def audit(event, arguments):
    if event == "open":
        raw_path = arguments[0]
        if isinstance(raw_path, int):
            return
        path = Path(raw_path).resolve()
        if path not in allowed_paths:
            raise RuntimeError(f"unexpected filesystem access: {path}")
    if event.startswith(("socket.", "subprocess.")) or event in {
        "os.exec",
        "os.posix_spawn",
        "os.spawn",
        "os.system",
    }:
        raise RuntimeError(f"forbidden offline operation: {event}")

sys.addaudithook(audit)
raise SystemExit(non_format_scenarios.validate(arguments))
"""
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            paths = write_validation_fixture(
                temp_path,
                catalog(complete_scenarios()),
                MANIFEST_IDS,
                FAKE_REPOSITORY,
                FAKE_REVISION,
            )
            environment = os.environ.copy()
            environment["PYTHONDONTWRITEBYTECODE"] = "1"
            result = subprocess.run(
                [
                    sys.executable,
                    "-c",
                    audit_program,
                    str(VALIDATOR.parent),
                    *(str(path) for path in paths),
                ],
                capture_output=True,
                check=False,
                cwd=temp_path,
                env=environment,
                text=True,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("covering 8 categories", result.stdout)

    def test_repository_catalog_is_valid(self):
        result = subprocess.run(
            [
                sys.executable,
                str(VALIDATOR),
                "validate",
                "--catalog",
                str(CATALOG),
                "--manifest",
                str(MANIFEST),
                "--baseline",
                str(BASELINE),
            ],
            capture_output=True,
            check=False,
            text=True,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("covering 8 categories", result.stdout)

        contents = CATALOG.read_text(encoding="utf-8")
        block_pattern = re.compile(
            r"^```json ulog-non-format-scenario\n(.*?)^```$",
            re.MULTILINE | re.DOTALL,
        )
        matches = list(block_pattern.finditer(contents))
        scenarios = [json.loads(match.group(1)) for match in matches]
        expected_matrix = {
            "compile-filter-cutoff-matrix": (
                ["API-005", "DYN-001", "LIM-001"],
                ["DEF-011", "DIFF-001"],
            ),
            "default-logger-exchange": (
                ["API-001", "DFL-001", "DFL-002"],
                ["DEF-008", "DIFF-003"],
            ),
            "default-logger-initial-null": (
                ["API-008", "DFL-001", "DST-001"],
                [],
            ),
            "dynamic-debug-force-matrix": (
                ["DYN-002", "LVL-001"],
                ["DEF-012"],
            ),
            "dynamic-debug-location-selection": (
                ["API-006", "DYN-001", "DYN-002"],
                [],
            ),
            "flush-threshold-and-periodic-wakeup": (
                ["LFC-003", "LVL-003"],
                [],
            ),
            "flush-watermark": (["LFC-002"], ["DIFF-006"]),
            "limited-filtered-attempts": (
                ["DYN-002", "LIM-002"],
                ["DEF-003"],
            ),
            "limited-power-of-two-drops": (["LIM-001", "LIM-002"], []),
            "reopen-watermark-partial-failure": (
                ["DST-008", "LFC-004"],
                ["DIFF-006"],
            ),
            "runtime-filter-threshold-matrix": (
                ["API-001", "API-007", "LVL-001"],
                ["DEF-012"],
            ),
            "startup-explicit-handoff": (
                ["BST-001", "BST-002", "DFL-001"],
                ["DEF-009", "DIFF-004"],
            ),
            "startup-overflow-accounting": (
                ["BST-001", "DST-008"],
                ["DEF-001", "DIFF-004", "DIFF-005"],
            ),
        }
        actual_matrix = {
            scenario["id"]: (scenario["feature_ids"], scenario["difference_ids"])
            for scenario in scenarios
        }
        self.assertEqual(actual_matrix, expected_matrix)
        for index, (match, scenario) in enumerate(zip(matches, scenarios)):
            evidence_end = (
                matches[index + 1].start() if index + 1 < len(matches) else len(contents)
            )
            self.assertIn(
                "/blob/72e07f717ae46a17822776df21ebd73dbc4ce728/",
                contents[match.end() : evidence_end],
                scenario["id"],
            )

    def test_missing_required_category_is_rejected(self):
        scenarios = complete_scenarios()[:-1]
        result = self.run_validator(catalog(scenarios))

        self.assertEqual(result.returncode, 1)
        self.assertIn("missing required categories: startup-forwarding", result.stderr)

    def test_category_requires_anchor_feature_ids(self):
        scenarios = complete_scenarios()
        scenarios[0]["feature_ids"] = ["API-001"]
        result = self.run_validator(catalog(scenarios))

        self.assertEqual(result.returncode, 1)
        self.assertIn("missing anchor feature IDs: API-005", result.stderr)

    def test_unknown_manifest_id_is_rejected(self):
        scenarios = complete_scenarios()
        scenarios[0]["feature_ids"] = sorted(
            [*scenarios[0]["feature_ids"], "API-999"]
        )
        result = self.run_validator(catalog(scenarios))

        self.assertEqual(result.returncode, 1)
        self.assertIn("unknown manifest IDs: API-999", result.stderr)

    def test_duplicate_and_unsorted_scenario_ids_are_rejected(self):
        scenarios = complete_scenarios()
        scenarios[-1]["id"] = scenarios[0]["id"]
        duplicate = self.run_validator(catalog(scenarios))

        scenarios = complete_scenarios()
        scenarios[0], scenarios[1] = scenarios[1], scenarios[0]
        unsorted = self.run_validator(catalog(scenarios))

        self.assertEqual(duplicate.returncode, 1)
        self.assertIn("Scenario IDs must be unique and sorted", duplicate.stderr)
        self.assertEqual(unsorted.returncode, 1)
        self.assertIn("Scenario IDs must be unique and sorted", unsorted.stderr)

    def test_missing_and_extra_keys_are_rejected(self):
        missing_scenarios = complete_scenarios()
        del missing_scenarios[0]["inputs"]
        missing = self.run_validator(catalog(missing_scenarios))

        extra_scenarios = complete_scenarios()
        extra_scenarios[0]["notes"] = []
        extra = self.run_validator(catalog(extra_scenarios))

        self.assertEqual(missing.returncode, 1)
        self.assertIn("missing keys: inputs", missing.stderr)
        self.assertEqual(extra.returncode, 1)
        self.assertIn("unexpected keys: notes", extra.stderr)

    def test_sleep_based_action_is_rejected(self):
        scenarios = complete_scenarios()
        scenarios[0]["action"] = ["Call std::this_thread::sleep_for(1s)."]
        result = self.run_validator(catalog(scenarios))

        self.assertEqual(result.returncode, 1)
        self.assertIn("uses forbidden elapsed-time synchronization", result.stderr)

    def test_parity_shapes_and_difference_ids_are_consistent(self):
        same_scenarios = complete_scenarios()
        same_scenarios[0]["difference_ids"] = ["DEF-001"]
        same = self.run_validator(
            catalog(same_scenarios), manifest_ids=(*MANIFEST_IDS, "DEF-001")
        )

        intentional_scenarios = complete_scenarios()
        intentional_scenarios[0]["parity"] = "intentional"
        intentional_scenarios[0]["observable_result"] = {
            "baseline": ["first"],
            "ulog": ["second"],
        }
        intentional = self.run_validator(catalog(intentional_scenarios))

        decision_scenarios = complete_scenarios()
        decision_scenarios[0]["parity"] = "decision-point"
        decision_scenarios[0]["difference_ids"] = ["DEF-001"]
        decision_scenarios[0]["observable_result"] = {
            "baseline": ["ambiguous"],
            "decision": [],
        }
        decision = self.run_validator(
            catalog(decision_scenarios), manifest_ids=(*MANIFEST_IDS, "DEF-001")
        )

        self.assertEqual(same.returncode, 1)
        self.assertIn("parity 'same' requires no difference IDs", same.stderr)
        self.assertEqual(intentional.returncode, 1)
        self.assertIn("parity 'intentional' requires difference IDs", intentional.stderr)
        self.assertEqual(decision.returncode, 1)
        self.assertIn("decision must be a non-empty string list", decision.stderr)

    def test_feature_and_difference_id_namespaces_are_separate(self):
        feature_scenarios = complete_scenarios()
        feature_scenarios[0]["feature_ids"] = ["DEF-001"]
        feature = self.run_validator(
            catalog(feature_scenarios), manifest_ids=(*MANIFEST_IDS, "DEF-001")
        )

        difference_scenarios = complete_scenarios()
        difference_scenarios[0]["difference_ids"] = ["API-001"]
        difference_scenarios[0]["parity"] = "intentional"
        difference_scenarios[0]["observable_result"] = {
            "baseline": ["first"],
            "ulog": ["second"],
        }
        difference = self.run_validator(catalog(difference_scenarios))

        self.assertEqual(feature.returncode, 1)
        self.assertIn("feature_ids cannot contain DEF-/DIFF- IDs", feature.stderr)
        self.assertEqual(difference.returncode, 1)
        self.assertIn("difference_ids must contain only DEF-/DIFF- IDs", difference.stderr)

    def test_strict_json_rejects_duplicate_keys_and_nan(self):
        valid_block = scenario_block(complete_scenarios()[0])
        duplicate_block = valid_block.replace(
            '  "id": "compile-time-filtering-case",',
            '  "id": "compile-time-filtering-case",\n  "id": "duplicate",',
        )
        duplicate_document = catalog(complete_scenarios()).replace(
            valid_block, duplicate_block, 1
        )
        duplicate = self.run_validator(duplicate_document)

        nan_block = valid_block.replace('"fixed_value": 1', '"fixed_value": NaN')
        nan_document = catalog(complete_scenarios()).replace(valid_block, nan_block, 1)
        nan = self.run_validator(nan_document)

        self.assertEqual(duplicate.returncode, 1)
        self.assertIn("duplicate JSON key", duplicate.stderr)
        self.assertEqual(nan.returncode, 1)
        self.assertIn("non-standard JSON value", nan.stderr)

    def test_marker_fence_and_framing_are_strict(self):
        missing_marker = self.run_validator(
            catalog(complete_scenarios()).replace(
                "<!-- ulog-non-format-scenarios-schema: 1 -->\n", ""
            )
        )
        unknown_version = self.run_validator(
            catalog(complete_scenarios(), schema_version=2)
        )
        unknown_fence = self.run_validator(
            catalog(complete_scenarios()).replace(
                "```json ulog-non-format-scenario",
                "```json ulog-non-format-scenario-v2",
                1,
            )
        )
        crlf = self.run_validator(
            catalog(complete_scenarios()).replace("\n", "\r\n")
        )
        no_final_lf = self.run_validator(catalog(complete_scenarios()).rstrip("\n"))

        self.assertEqual(missing_marker.returncode, 1)
        self.assertIn("exactly one schema marker", missing_marker.stderr)
        self.assertEqual(unknown_version.returncode, 1)
        self.assertIn("unsupported schema version 2", unknown_version.stderr)
        self.assertEqual(unknown_fence.returncode, 1)
        self.assertIn("unknown scenario fence", unknown_fence.stderr)
        self.assertEqual(crlf.returncode, 1)
        self.assertIn("must use LF line endings", crlf.stderr)
        self.assertEqual(no_final_lf.returncode, 1)
        self.assertIn("must end with LF", no_final_lf.stderr)

    def test_baseline_revision_mismatch_is_rejected(self):
        result = self.run_validator(
            catalog(complete_scenarios()),
            baseline_revision="fedcba9876543210fedcba9876543210fedcba98",
        )

        self.assertEqual(result.returncode, 1)
        self.assertIn("does not match migration baseline", result.stderr)

    def test_baseline_repository_mismatch_is_rejected(self):
        result = self.run_validator(
            catalog(complete_scenarios()), baseline_repository="another-repository"
        )

        self.assertEqual(result.returncode, 1)
        self.assertIn("does not match migration baseline", result.stderr)

    def test_missing_catalog_baseline_declaration_is_rejected(self):
        document = catalog(complete_scenarios())
        declaration = (
            "```text ulog-non-format-baseline\n"
            f"repository: {FAKE_REPOSITORY}\n"
            f"commit: {FAKE_REVISION}\n"
            "```\n\n"
        )
        result = self.run_validator(document.replace(declaration, "", 1))

        self.assertEqual(result.returncode, 1)
        self.assertIn("exactly one baseline declaration", result.stderr)

    def test_invalid_utf8_and_bom_are_reported_without_traceback(self):
        invalid_utf8 = self.run_validator(b"\xff")
        bom = self.run_validator(
            b"\xef\xbb\xbf" + catalog(complete_scenarios()).encode("utf-8")
        )

        self.assertEqual(invalid_utf8.returncode, 1)
        self.assertIn("is not valid UTF-8", invalid_utf8.stderr)
        self.assertNotIn("Traceback", invalid_utf8.stderr)
        self.assertEqual(bom.returncode, 1)
        self.assertIn("must not contain a UTF-8 BOM", bom.stderr)


if __name__ == "__main__":
    unittest.main()
