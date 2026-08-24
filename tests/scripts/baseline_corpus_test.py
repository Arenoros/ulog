import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
CORPUS_TOOL = PROJECT_ROOT / "scripts" / "baseline_corpus.py"
CAPABILITY_MANIFEST = PROJECT_ROOT / "docs" / "migration" / "capability-manifest.md"
CORPUS_ROOT = PROJECT_ROOT / "docs" / "migration" / "corpus"
BASELINE_CORPUS = CORPUS_ROOT / "baseline-v1.json"
TEXT_CORPUS = CORPUS_ROOT / "text-v1.json"
JSON_CORPUS = CORPUS_ROOT / "json-v1.json"
TEXT_PROBE_SOURCE = PROJECT_ROOT / "tools" / "baseline_text_probe"
JSON_PROBE_SOURCE = PROJECT_ROOT / "tools" / "baseline_json_probe"
BASELINE_REPOSITORY = "user" + "ver"
BASELINE_REVISION = "72e07f717ae46a17822776df21ebd73dbc4ce728"
PROBE_BASELINE = {
    "repository": BASELINE_REPOSITORY,
    "revision": BASELINE_REVISION,
}
KNOWN_CORPUS = {
    "schema_version": 1,
    "provenance": {
        "baseline_document": "docs/migration/baseline.md",
        "capture_tool": "ulog-baseline-corpus/1",
        "normalization_profile": "ulog-baseline-normalization/1",
        "repository": BASELINE_REPOSITORY,
        "revision": BASELINE_REVISION,
    },
    "cases": [
        {
            "id": "raw-basic",
            "feature_ids": ["FMT-004", "VAL-006"],
            "difference_ids": [],
            "platform": "portable",
            "observed": {
                "kind": "utf8",
                "format": "raw",
                "value": "tskv\tfoo=bar\ttext=test\n",
            },
        }
    ],
    "integrity": {
        "algorithm": "sha256",
        "canonicalization": "ulog-json-v1",
        "value": "0aa95807472b2e2d9571724bf6f71be461a6f7bdc2b5f16a86a360e2cd5bdcba",
    },
}


def make_probe_document(
    *,
    schema_version: object = 1,
    baseline: object = PROBE_BASELINE,
    feature_ids: list[str] | None = None,
) -> dict[str, object]:
    return {
        "probe_schema_version": schema_version,
        "baseline": baseline,
        "cases": [
            {
                "id": "raw-basic",
                "feature_ids": feature_ids or ["FMT-004", "VAL-006"],
                "difference_ids": [],
                "platform": "portable",
                "observed": {
                    "kind": "utf8",
                    "format": "raw",
                    "value": "tskv\tfoo=bar\ttext=test\n",
                },
                "normalization": [],
            }
        ],
    }


def probe_program(document: object) -> list[str]:
    return [
        sys.executable,
        "-c",
        f"print({json.dumps(json.dumps(document))})",
    ]


def counting_probe_program(
    temp_path: Path, documents: list[dict[str, object]]
) -> list[str]:
    probe_path = temp_path / "counting_probe.py"
    counter_path = temp_path / "probe_run_count.txt"
    probe_path.write_text(
        "import json\n"
        "import sys\n"
        "from pathlib import Path\n"
        f"documents = json.loads({json.dumps(json.dumps(documents))})\n"
        "counter = Path(sys.argv[1])\n"
        "index = int(counter.read_text()) if counter.exists() else 0\n"
        "if index >= len(documents):\n"
        "    raise RuntimeError('probe ran too many times')\n"
        "counter.write_text(str(index + 1))\n"
        "print(json.dumps(documents[index], ensure_ascii=False))\n",
        encoding="utf-8",
    )
    return [sys.executable, str(probe_path), str(counter_path)]


def recalculate_integrity(corpus: dict[str, object]) -> None:
    material = json.loads(json.dumps(corpus))
    del material["integrity"]["value"]
    canonical = json.dumps(
        material,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
    corpus["integrity"]["value"] = hashlib.sha256(canonical).hexdigest()


def make_fake_git(
    temp_path: Path,
    revision: str,
    worktree_changes: str = "",
    dirty_marker: Path | None = None,
) -> dict[str, str]:
    driver = temp_path / "fake_git.py"
    driver.write_text(
        """import os
import sys

arguments = sys.argv[1:]
if (
    len(arguments) >= 3
    and arguments[0] == "-C"
    and arguments[2:4] == ["rev-parse", "--verify"]
    and arguments[4] in {"HEAD^{commit}", "HEAD{commit}"}
):
    print(os.environ["FAKE_GIT_REVISION"])
    raise SystemExit(0)
if (
    len(arguments) >= 3
    and arguments[0] == "-C"
    and arguments[2:4] == ["status", "--short"]
    and arguments[4] == "--untracked-files=all"
):
    changes = os.environ["FAKE_GIT_WORKTREE_CHANGES"].splitlines()
    marker = os.environ.get("FAKE_GIT_DIRTY_MARKER")
    if marker and os.path.exists(marker):
        changes.append(" M universal/src/logging/changed_during_probe.cpp")
    print("\\n".join(changes))
    raise SystemExit(0)

print(f"unexpected git arguments: {arguments!r}", file=sys.stderr)
raise SystemExit(9)
""",
        encoding="utf-8",
    )

    if os.name == "nt":
        wrapper = temp_path / "git.cmd"
        wrapper.write_text(
            '@echo off\r\n"%FAKE_GIT_PYTHON%" "%FAKE_GIT_DRIVER%" %*\r\n',
            encoding="utf-8",
        )
    else:
        wrapper = temp_path / "git"
        wrapper.write_text(
            '#!/bin/sh\nexec "$FAKE_GIT_PYTHON" "$FAKE_GIT_DRIVER" "$@"\n',
            encoding="utf-8",
        )
        wrapper.chmod(0o755)

    environment = os.environ.copy()
    environment["PATH"] = f"{temp_path}{os.pathsep}{environment['PATH']}"
    environment["FAKE_GIT_DRIVER"] = str(driver)
    environment["FAKE_GIT_PYTHON"] = sys.executable
    environment["FAKE_GIT_REVISION"] = revision
    environment["FAKE_GIT_WORKTREE_CHANGES"] = worktree_changes
    if dirty_marker is not None:
        environment["FAKE_GIT_DIRTY_MARKER"] = str(dirty_marker)
    if os.name == "nt":
        environment["PATHEXT"] = f".CMD;{environment.get('PATHEXT', '')}"
    return environment


class BaselineCorpusTest(unittest.TestCase):
    def test_text_probe_configuration_reports_how_to_fix_missing_host_input(self):
        cmake = shutil.which("cmake")
        self.assertIsNotNone(cmake, "CMake must be available for repository tests")
        with tempfile.TemporaryDirectory() as temp_directory:
            result = subprocess.run(
                [
                    cmake,
                    "-S",
                    str(TEXT_PROBE_SOURCE),
                    "-B",
                    str(Path(temp_directory) / "build"),
                ],
                capture_output=True,
                check=False,
                text=True,
            )

        self.assertNotEqual(result.returncode, 0)
        output = f"{result.stdout}\n{result.stderr}"
        if os.name == "nt":
            self.assertIn("does not support a native Windows build", output)
            self.assertIn("Linux or macOS", output)
        else:
            self.assertIn(
                f"ULOG_BASELINE_SOURCE must name a clean {BASELINE_REPOSITORY} checkout",
                output,
            )

    def test_json_probe_configuration_reports_how_to_fix_missing_host_input(self):
        cmake = shutil.which("cmake")
        self.assertIsNotNone(cmake, "CMake must be available for repository tests")
        with tempfile.TemporaryDirectory() as temp_directory:
            result = subprocess.run(
                [
                    cmake,
                    "-S",
                    str(JSON_PROBE_SOURCE),
                    "-B",
                    str(Path(temp_directory) / "build"),
                ],
                capture_output=True,
                check=False,
                text=True,
            )

        self.assertNotEqual(result.returncode, 0)
        output = f"{result.stdout}\n{result.stderr}"
        if os.name == "nt":
            self.assertIn("does not support a native Windows build", output)
            self.assertIn("Linux or macOS", output)
        else:
            self.assertIn(
                f"ULOG_BASELINE_SOURCE must name a clean {BASELINE_REPOSITORY} checkout",
                output,
            )

    def test_committed_text_corpus_has_issue_4_matrix(self):
        self.assertTrue(
            BASELINE_CORPUS.is_file(), "baseline-v1.json must remain in the corpus"
        )
        self.assertTrue(
            TEXT_CORPUS.is_file(),
            "Capture and commit docs/migration/corpus/text-v1.json for issue #4",
        )

        validation = self.run_validation_root(CORPUS_ROOT)
        self.assertEqual(validation.returncode, 0, validation.stderr)

        corpus = json.loads(TEXT_CORPUS.read_text(encoding="utf-8"))
        cases_by_id = {case["id"]: case for case in corpus["cases"]}
        self.assertEqual(
            len(cases_by_id),
            len(corpus["cases"]),
            "text-v1.json must not contain duplicate case IDs",
        )

        format_features = {
            "tskv": {"API-010", "FMT-002"},
            "ltsv": {"API-010", "FMT-003"},
            "raw": {"FMT-004"},
        }
        scenario_features = {
            "empty": set(),
            "simple": {"VAL-001"},
            "unicode-controls": {"VAL-001", "VAL-006"},
            "ordered-scalars": {"VAL-001", "VAL-006", "VAL-008"},
            "replacement-frozen-duplicate": {
                "VAL-001",
                "VAL-006",
                "VAL-007",
            },
        }
        scenario_differences = {
            "empty": [],
            "simple": [],
            "unicode-controls": [],
            "ordered-scalars": [],
            "replacement-frozen-duplicate": ["DEF-004"],
        }
        expected_case_ids = {
            f"{format_name}-{scenario_name}"
            for format_name in format_features
            for scenario_name in scenario_features
        }
        missing_case_ids = sorted(expected_case_ids.difference(cases_by_id))
        self.assertFalse(
            missing_case_ids,
            f"text-v1.json is missing issue #4 cases: {', '.join(missing_case_ids)}",
        )

        for format_name, format_ids in format_features.items():
            for scenario_name, scenario_ids in scenario_features.items():
                case_id = f"{format_name}-{scenario_name}"
                with self.subTest(case_id=case_id):
                    case = cases_by_id[case_id]
                    self.assertEqual(
                        case["feature_ids"], sorted(format_ids | scenario_ids)
                    )
                    self.assertEqual(
                        case["difference_ids"], scenario_differences[scenario_name]
                    )
                    self.assertEqual(case["platform"], "portable")
                    self.assertEqual(case["observed"]["kind"], "utf8")
                    self.assertEqual(case["observed"]["format"], format_name)

    def test_committed_json_corpus_has_issue_5_matrix(self):
        self.assertTrue(
            JSON_CORPUS.is_file(),
            "Capture and commit docs/migration/corpus/json-v1.json for issue #5",
        )

        validation = self.run_validation_root(CORPUS_ROOT)
        self.assertEqual(validation.returncode, 0, validation.stderr)

        corpus = json.loads(JSON_CORPUS.read_text(encoding="utf-8"))
        cases_by_id = {case["id"]: case for case in corpus["cases"]}
        self.assertEqual(
            len(cases_by_id),
            len(corpus["cases"]),
            "json-v1.json must not contain duplicate case IDs",
        )

        format_contracts = {
            "json": ("json", "FMT-005"),
            "json-yadeploy": ("json_yadeploy", "FMT-006"),
        }
        scenario_features = {
            "typed-nested-escaping": {
                "API-010",
                "VAL-001",
                "VAL-006",
                "VAL-008",
            },
            "replacement-frozen-duplicate": {
                "API-010",
                "VAL-001",
                "VAL-006",
                "VAL-007",
            },
        }
        expected_case_ids = {
            f"{case_prefix}-{scenario_name}"
            for case_prefix in format_contracts
            for scenario_name in scenario_features
        }
        self.assertEqual(
            set(cases_by_id),
            expected_case_ids,
            "json-v1.json must contain exactly the issue #5 matrix",
        )

        for case_prefix, (format_name, format_id) in format_contracts.items():
            for scenario_name, scenario_ids in scenario_features.items():
                case_id = f"{case_prefix}-{scenario_name}"
                with self.subTest(case_id=case_id):
                    case = cases_by_id[case_id]
                    self.assertEqual(
                        case["feature_ids"], sorted({format_id} | scenario_ids)
                    )
                    expected_differences = (
                        ["DEF-004"]
                        if scenario_name == "replacement-frozen-duplicate"
                        else []
                    )
                    self.assertEqual(case["difference_ids"], expected_differences)
                    self.assertEqual(case["platform"], "portable")
                    self.assertEqual(case["observed"]["kind"], "json-object")
                    self.assertEqual(case["observed"]["format"], format_name)
                    self.assertEqual(case["observed"]["framing"], "compact-lf")

        standard_fields = {
            "json": ("timestamp", "level", "module", "text"),
            "json-yadeploy": ("@timestamp", "levelStr", "module", "message"),
        }
        typed_field_keys = [
            "escaped-key\"\t\x00\\",
            "signed",
            "unsigned",
            "float",
            "double",
            "bool-true",
            "bool-false",
            "null",
            "nested",
        ]
        expected_typed_values = {
            "escaped-key\"\t\x00\\": {
                "kind": "string",
                "value": "Привет🌍\t\r\n\x00\\",
            },
            "signed": {"kind": "number", "value": "-42"},
            "unsigned": {
                "kind": "number",
                "value": "18446744073709551615",
            },
            "float": {"kind": "number", "value": "1.5"},
            "double": {"kind": "number", "value": "-2.25"},
            "bool-true": {"kind": "boolean", "value": True},
            "bool-false": {"kind": "boolean", "value": False},
            "null": {"kind": "null"},
        }
        expected_collision_members = {
            "json": [
                ("timestamp", "2000-01-02T03:04:05.123456Z"),
                ("level", "INFO"),
                ("module", "BaselineJsonProbe ( <source-path>:321 )"),
                ("first", "one"),
                ("replace", "new"),
                ("last", "three"),
                ("frozen", "keep"),
                ("duplicate", "one"),
                ("duplicate", "two"),
                ("timestamp", "shadow-timestamp"),
                ("level", "shadow-level"),
                ("text", "shadow-text"),
                ("text", "collisions"),
            ],
            "json-yadeploy": [
                ("@timestamp", "2000-01-02T03:04:05.123456Z"),
                ("levelStr", "INFO"),
                ("module", "BaselineJsonProbe ( <source-path>:321 )"),
                ("first", "one"),
                ("replace", "new"),
                ("last", "three"),
                ("frozen", "keep"),
                ("@timestamp", "shadow-timestamp"),
                ("levelStr", "shadow-level"),
                ("message", "shadow-message"),
                ("duplicate", "one"),
                ("duplicate", "two"),
                ("message", "collisions"),
            ],
        }
        for case_prefix in format_contracts:
            case = cases_by_id[f"{case_prefix}-typed-nested-escaping"]
            members = case["observed"]["members"]
            prefix_fields = list(standard_fields[case_prefix][:3])
            message_field = standard_fields[case_prefix][3]
            self.assertEqual(
                [member["key"] for member in members],
                prefix_fields + typed_field_keys + [message_field],
            )
            self.assertEqual(
                [member["value"]["value"] for member in members[:3]],
                [
                    "2000-01-02T03:04:05.123456Z",
                    "INFO",
                    "BaselineJsonProbe ( <source-path>:321 )",
                ],
            )
            members_by_key = {member["key"]: member for member in members}
            for key, expected_value in expected_typed_values.items():
                self.assertEqual(members_by_key[key]["value"], expected_value)
            self.assertEqual(
                members_by_key[message_field]["value"],
                {
                    "kind": "string",
                    "value": "message Привет🌍 \"\t\r\n\x00\\",
                },
            )
            nested = members_by_key["nested"]["value"]
            self.assertEqual(nested["kind"], "object")
            self.assertEqual(
                [member["key"] for member in nested["members"]],
                ["a", "obj", "z"],
            )
            self.assertEqual(
                nested["members"][0]["value"]["items"],
                [
                    {"kind": "string", "value": "x"},
                    {"kind": "boolean", "value": False},
                    {"kind": "null"},
                ],
            )
            self.assertEqual(
                [
                    member["key"]
                    for member in nested["members"][1]["value"]["members"]
                ],
                ["a", "b"],
            )

            collision_case = cases_by_id[
                f"{case_prefix}-replacement-frozen-duplicate"
            ]
            actual_collision_members = [
                (member["key"], member["value"]["value"])
                for member in collision_case["observed"]["members"]
            ]
            self.assertEqual(
                actual_collision_members, expected_collision_members[case_prefix]
            )

    def run_validation_root(
        self, corpus_root: Path, manifest: Path = CAPABILITY_MANIFEST
    ) -> subprocess.CompletedProcess[str]:
        environment = os.environ.copy()
        environment["PATH"] = ""
        return subprocess.run(
            [
                sys.executable,
                str(CORPUS_TOOL),
                "validate",
                "--corpus-root",
                str(corpus_root),
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            check=False,
            env=environment,
            text=True,
        )

    def run_validation(
        self, corpus: object, temp_path: Path
    ) -> subprocess.CompletedProcess[str]:
        corpus_root = temp_path / "corpus"
        corpus_root.mkdir()
        (corpus_root / "baseline-v1.json").write_text(
            json.dumps(corpus, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        return self.run_validation_root(corpus_root)

    def run_capture(
        self,
        source_path: Path,
        output_path: Path,
        environment: dict[str, str],
        command: list[str],
        *,
        force: bool = False,
    ) -> subprocess.CompletedProcess[str]:
        arguments = [
            sys.executable,
            str(CORPUS_TOOL),
            "capture",
            "--baseline-source",
            str(source_path),
            "--output",
            str(output_path),
        ]
        if force:
            arguments.append("--force")
        arguments.extend(["--", *command])
        return subprocess.run(
            arguments,
            capture_output=True,
            check=False,
            env=environment,
            text=True,
        )

    def test_validate_accepts_known_corpus_without_external_checkout(self):
        for capture_tool in (
            "ulog-baseline-corpus/1",
            "ulog-baseline-corpus/2",
            "ulog-baseline-corpus/3",
        ):
            with self.subTest(capture_tool=capture_tool):
                corpus = json.loads(json.dumps(KNOWN_CORPUS))
                corpus["provenance"]["capture_tool"] = capture_tool
                recalculate_integrity(corpus)
                with tempfile.TemporaryDirectory() as temp_directory:
                    result = self.run_validation(corpus, Path(temp_directory))

                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(
                    "Validated 1 corpus file(s) with 1 case(s)", result.stdout
                )

    def test_validate_rejects_accidental_payload_edit(self):
        edited_corpus = json.loads(json.dumps(KNOWN_CORPUS))
        edited_corpus["cases"][0]["observed"]["value"] = (
            "tskv\tfoo=bar\ttext=changed\n"
        )
        with tempfile.TemporaryDirectory() as temp_directory:
            result = self.run_validation(edited_corpus, Path(temp_directory))

        self.assertEqual(result.returncode, 1)
        self.assertIn("integrity mismatch", result.stderr)
        self.assertIn("baseline-v1.json", result.stderr)
        self.assertIn("Recapture", result.stderr)

    def test_validate_rejects_empty_corpus_root(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            corpus_root = Path(temp_directory) / "corpus"
            corpus_root.mkdir()
            result = self.run_validation_root(corpus_root)

        self.assertEqual(result.returncode, 1)
        self.assertIn("contains no JSON files", result.stderr)

    def test_validate_rejects_unknown_manifest_id(self):
        corpus = json.loads(json.dumps(KNOWN_CORPUS))
        corpus["cases"][0]["feature_ids"][0] = "FMT-999"
        with tempfile.TemporaryDirectory() as temp_directory:
            result = self.run_validation(corpus, Path(temp_directory))

        self.assertEqual(result.returncode, 1)
        self.assertIn("unknown manifest IDs: FMT-999", result.stderr)

    def test_validate_rejects_unknown_capture_tool_version(self):
        corpus = json.loads(json.dumps(KNOWN_CORPUS))
        corpus["provenance"]["capture_tool"] = "ulog-baseline-corpus/999"
        recalculate_integrity(corpus)
        with tempfile.TemporaryDirectory() as temp_directory:
            result = self.run_validation(corpus, Path(temp_directory))

        self.assertEqual(result.returncode, 1)
        self.assertIn("supported tool versions", result.stderr)
        self.assertIn("Recapture", result.stderr)

    def test_validate_rejects_duplicate_case_ids_across_files(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            corpus_root = Path(temp_directory) / "corpus"
            corpus_root.mkdir()
            for filename in ("first.json", "second.json"):
                (corpus_root / filename).write_text(
                    json.dumps(KNOWN_CORPUS, ensure_ascii=False, indent=2) + "\n",
                    encoding="utf-8",
                )
            result = self.run_validation_root(corpus_root)

        self.assertEqual(result.returncode, 1)
        self.assertIn("duplicate case id 'raw-basic' across corpus files", result.stderr)
        self.assertIn("first.json", result.stderr)
        self.assertIn("second.json", result.stderr)

    def test_validate_rejects_observed_with_an_unknown_member(self):
        corpus = json.loads(json.dumps(KNOWN_CORPUS))
        corpus["cases"][0]["observed"]["encoding"] = "utf-8"
        recalculate_integrity(corpus)
        with tempfile.TemporaryDirectory() as temp_directory:
            result = self.run_validation(corpus, Path(temp_directory))

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "observed must contain exactly kind, format, and value", result.stderr
        )

    def test_validate_rejects_format_without_matching_feature_id(self):
        corpus = json.loads(json.dumps(KNOWN_CORPUS))
        corpus["cases"][0]["observed"]["format"] = "tskv"
        recalculate_integrity(corpus)
        with tempfile.TemporaryDirectory() as temp_directory:
            result = self.run_validation(corpus, Path(temp_directory))

        self.assertEqual(result.returncode, 1)
        self.assertIn("format 'tskv' must reference exactly FMT-002", result.stderr)

    def test_validate_rejects_text_without_one_final_newline(self):
        corpus = json.loads(json.dumps(KNOWN_CORPUS))
        corpus["cases"][0]["observed"]["value"] = "tskv\tfoo=bar\ttext=test"
        recalculate_integrity(corpus)
        with tempfile.TemporaryDirectory() as temp_directory:
            result = self.run_validation(corpus, Path(temp_directory))

        self.assertEqual(result.returncode, 1)
        self.assertIn("one newline-terminated log line", result.stderr)

    def test_validate_accepts_ltsv_text_observation(self):
        corpus = json.loads(json.dumps(KNOWN_CORPUS))
        corpus["cases"][0] = {
            "id": "ltsv-simple",
            "feature_ids": ["FMT-003", "VAL-001"],
            "difference_ids": [],
            "platform": "portable",
            "observed": {
                "kind": "utf8",
                "format": "ltsv",
                "value": "timestamp:2000-01-02T03:04:05.123456\ttext:hello\n",
            },
        }
        recalculate_integrity(corpus)
        with tempfile.TemporaryDirectory() as temp_directory:
            result = self.run_validation(corpus, Path(temp_directory))

        self.assertEqual(result.returncode, 0, result.stderr)

    def test_validate_rejects_invalid_text_kind_and_format(self):
        invalid_members = (
            ("kind", "bytes", "observed kind must be 'utf8'"),
            ("format", "plain", "observed format must be one of"),
        )
        for member, value, diagnostic in invalid_members:
            with self.subTest(member=member):
                corpus = json.loads(json.dumps(KNOWN_CORPUS))
                corpus["cases"][0]["observed"][member] = value
                recalculate_integrity(corpus)
                with tempfile.TemporaryDirectory() as temp_directory:
                    result = self.run_validation(corpus, Path(temp_directory))

                self.assertEqual(result.returncode, 1)
                self.assertIn(diagnostic, result.stderr)

    def test_validate_rejects_non_string_format_without_traceback(self):
        corpus = json.loads(json.dumps(KNOWN_CORPUS))
        corpus["cases"][0]["observed"]["format"] = []
        recalculate_integrity(corpus)
        with tempfile.TemporaryDirectory() as temp_directory:
            result = self.run_validation(corpus, Path(temp_directory))

        self.assertEqual(result.returncode, 1)
        self.assertIn("observed format must be one of", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_validate_rejects_cr_and_multiple_log_lines(self):
        invalid_values = (
            "tskv\ttext=test\r\n",
            "tskv\ttext=first\nsecond\n",
        )
        for value in invalid_values:
            with self.subTest(value=value):
                corpus = json.loads(json.dumps(KNOWN_CORPUS))
                corpus["cases"][0]["observed"]["value"] = value
                recalculate_integrity(corpus)
                with tempfile.TemporaryDirectory() as temp_directory:
                    result = self.run_validation(corpus, Path(temp_directory))

                self.assertEqual(result.returncode, 1)
                self.assertIn("one newline-terminated log line", result.stderr)

    def test_validate_preserves_non_text_observation_schema(self):
        corpus = json.loads(json.dumps(KNOWN_CORPUS))
        corpus["cases"][0] = {
            "id": "json-structured",
            "feature_ids": ["DST-001"],
            "difference_ids": [],
            "platform": "portable",
            "observed": {
                "kind": "json-object",
                "value": {"text": "test", "typed": 42},
            },
        }
        recalculate_integrity(corpus)
        with tempfile.TemporaryDirectory() as temp_directory:
            result = self.run_validation(corpus, Path(temp_directory))

        self.assertEqual(result.returncode, 0, result.stderr)

    def test_validate_rejects_raw_json_log_line_in_committed_corpus(self):
        corpus = json.loads(json.dumps(KNOWN_CORPUS))
        corpus["cases"][0] = {
            "id": "json-raw-observation",
            "feature_ids": ["API-010", "FMT-005"],
            "difference_ids": [],
            "platform": "portable",
            "observed": {
                "kind": "json-line",
                "format": "json",
                "value": '{}\n',
            },
        }
        recalculate_integrity(corpus)
        with tempfile.TemporaryDirectory() as temp_directory:
            result = self.run_validation(corpus, Path(temp_directory))

        self.assertEqual(result.returncode, 1)
        self.assertIn("recapture", result.stderr.lower())

    def test_validate_rejects_json_object_without_standard_fields(self):
        corpus = json.loads(json.dumps(KNOWN_CORPUS))
        corpus["cases"][0] = {
            "id": "json-missing-standard-fields",
            "feature_ids": ["API-010", "FMT-005"],
            "difference_ids": [],
            "platform": "portable",
            "observed": {
                "kind": "json-object",
                "format": "json",
                "framing": "compact-lf",
                "members": [],
            },
        }
        recalculate_integrity(corpus)
        with tempfile.TemporaryDirectory() as temp_directory:
            result = self.run_validation(corpus, Path(temp_directory))

        self.assertEqual(result.returncode, 1)
        self.assertIn("missing standard fields", result.stderr)

    def test_capture_rejects_malformed_json_log_line(self):
        probe = make_probe_document(feature_ids=["API-010", "FMT-005"])
        probe["cases"][0]["id"] = "json-malformed"
        probe["cases"][0]["observed"] = {
            "kind": "json-line",
            "format": "json",
            "value": '{"timestamp":}\n',
        }

        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            source_path = temp_path / "baseline-source"
            source_path.mkdir()
            output_path = temp_path / "corpus.json"
            result = self.run_capture(
                source_path,
                output_path,
                make_fake_git(temp_path, BASELINE_REVISION),
                probe_program(probe),
            )

        self.assertEqual(result.returncode, 1)
        self.assertIn("not valid JSON", result.stderr)

    def test_capture_rejects_non_string_format_without_traceback(self):
        probe = make_probe_document()
        probe["cases"][0]["observed"]["format"] = []

        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            source_path = temp_path / "baseline-source"
            source_path.mkdir()
            result = self.run_capture(
                source_path,
                temp_path / "corpus.json",
                make_fake_git(temp_path, BASELINE_REVISION),
                probe_program(probe),
            )

        self.assertEqual(result.returncode, 1)
        self.assertIn("observed format must be one of", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_capture_normalizes_json_line_into_typed_ordered_members(self):
        timestamp = "2026-08-24T09:30:12.123456Z"
        source_path_value = "/baseline/tools/baseline_json_probe/main.cpp"
        raw_line = (
            f'{{"timestamp":"{timestamp}","level":"INFO",'
            f'"module":"BaselineJsonProbe ( {source_path_value}:321 )",'
            '"escaped-key\\"\\t\\u0000\\\\":"Привет🌍\\t\\r\\n\\u0000\\\\",'
            '"signed":-42,"unsigned":18446744073709551615,"float":1.5,'
            '"double":-2.25,"bool-true":true,"bool-false":false,"null":null,'
            '"nested":{"z":1,"a":["x",false,null],'
            '"obj":{"b":2,"a":true}},"duplicate":"one",'
            '"duplicate":"two","text":"message"}\n'
        )
        probe = make_probe_document(
            feature_ids=["API-010", "FMT-005", "VAL-001", "VAL-006", "VAL-008"]
        )
        probe_case = probe["cases"][0]
        probe_case["id"] = "json-typed-nested-escaping"
        probe_case["difference_ids"] = ["DEF-004"]
        probe_case["observed"] = {
            "kind": "json-line",
            "format": "json",
            "value": raw_line,
        }
        probe_case["normalization"] = [
            {
                "kind": "timestamp_utc",
                "path": "/value",
                "value": timestamp,
                "occurrence": 0,
            },
            {
                "kind": "source_path",
                "path": "/value",
                "value": source_path_value,
                "occurrence": 0,
            },
        ]

        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            source_path = temp_path / "baseline-source"
            source_path.mkdir()
            output_path = temp_path / "corpus.json"
            result = self.run_capture(
                source_path,
                output_path,
                make_fake_git(temp_path, BASELINE_REVISION),
                probe_program(probe),
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            corpus = json.loads(output_path.read_text(encoding="utf-8"))

        self.assertEqual(corpus["provenance"]["capture_tool"], "ulog-baseline-corpus/3")
        observed = corpus["cases"][0]["observed"]
        self.assertEqual(
            observed,
            {
                "kind": "json-object",
                "format": "json",
                "framing": "compact-lf",
                "members": [
                    {
                        "key": "timestamp",
                        "value": {
                            "kind": "string",
                            "value": "2000-01-02T03:04:05.123456Z",
                        },
                    },
                    {"key": "level", "value": {"kind": "string", "value": "INFO"}},
                    {
                        "key": "module",
                        "value": {
                            "kind": "string",
                            "value": "BaselineJsonProbe ( <source-path>:321 )",
                        },
                    },
                    {
                        "key": "escaped-key\"\t\x00\\",
                        "value": {
                            "kind": "string",
                            "value": "Привет🌍\t\r\n\x00\\",
                        },
                    },
                    {"key": "signed", "value": {"kind": "number", "value": "-42"}},
                    {
                        "key": "unsigned",
                        "value": {
                            "kind": "number",
                            "value": "18446744073709551615",
                        },
                    },
                    {"key": "float", "value": {"kind": "number", "value": "1.5"}},
                    {"key": "double", "value": {"kind": "number", "value": "-2.25"}},
                    {"key": "bool-true", "value": {"kind": "boolean", "value": True}},
                    {"key": "bool-false", "value": {"kind": "boolean", "value": False}},
                    {"key": "null", "value": {"kind": "null"}},
                    {
                        "key": "nested",
                        "value": {
                            "kind": "object",
                            "members": [
                                {
                                    "key": "a",
                                    "value": {
                                        "kind": "array",
                                        "items": [
                                            {"kind": "string", "value": "x"},
                                            {"kind": "boolean", "value": False},
                                            {"kind": "null"},
                                        ],
                                    },
                                },
                                {
                                    "key": "obj",
                                    "value": {
                                        "kind": "object",
                                        "members": [
                                            {
                                                "key": "a",
                                                "value": {
                                                    "kind": "boolean",
                                                    "value": True,
                                                },
                                            },
                                            {
                                                "key": "b",
                                                "value": {
                                                    "kind": "number",
                                                    "value": "2",
                                                },
                                            },
                                        ],
                                    },
                                },
                                {
                                    "key": "z",
                                    "value": {"kind": "number", "value": "1"},
                                },
                            ],
                        },
                    },
                    {"key": "duplicate", "value": {"kind": "string", "value": "one"}},
                    {"key": "duplicate", "value": {"kind": "string", "value": "two"}},
                    {"key": "text", "value": {"kind": "string", "value": "message"}},
                ],
            },
        )

    def test_validate_rejects_duplicate_json_keys(self):
        contents = json.dumps(KNOWN_CORPUS, ensure_ascii=False, indent=2)
        contents = contents.replace(
            '"schema_version": 1',
            '"schema_version": 1,\n  "schema_version": 1',
            1,
        )
        with tempfile.TemporaryDirectory() as temp_directory:
            corpus_root = Path(temp_directory) / "corpus"
            corpus_root.mkdir()
            (corpus_root / "duplicate.json").write_text(
                contents + "\n", encoding="utf-8"
            )
            result = self.run_validation_root(corpus_root)

        self.assertEqual(result.returncode, 1)
        self.assertIn("duplicate JSON key 'schema_version'", result.stderr)

    def test_validate_rejects_boolean_schema_version(self):
        corpus = json.loads(json.dumps(KNOWN_CORPUS))
        corpus["schema_version"] = True
        recalculate_integrity(corpus)
        with tempfile.TemporaryDirectory() as temp_directory:
            result = self.run_validation(corpus, Path(temp_directory))

        self.assertEqual(result.returncode, 1)
        self.assertIn("unsupported schema_version True", result.stderr)

    def test_validate_rejects_mixed_case_json_suffix(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            corpus_root = Path(temp_directory) / "corpus"
            corpus_root.mkdir()
            (corpus_root / "baseline-v1.json").write_text(
                json.dumps(KNOWN_CORPUS) + "\n", encoding="utf-8"
            )
            (corpus_root / "ignored.JSON").write_text("{}\n", encoding="utf-8")
            result = self.run_validation_root(corpus_root)

        self.assertEqual(result.returncode, 1)
        self.assertIn("must use the lowercase .json suffix", result.stderr)
        self.assertIn("ignored.JSON", result.stderr)

    def test_validate_reports_invalid_utf8_manifest_without_traceback(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            corpus_root = temp_path / "corpus"
            corpus_root.mkdir()
            (corpus_root / "baseline-v1.json").write_text(
                json.dumps(KNOWN_CORPUS) + "\n", encoding="utf-8"
            )
            manifest = temp_path / "manifest.md"
            manifest.write_bytes(b"\xff")
            result = self.run_validation_root(corpus_root, manifest)

        self.assertEqual(result.returncode, 1)
        self.assertIn("not valid UTF-8", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_capture_refuses_to_overwrite_corpus_without_force(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            source_path = temp_path / "baseline-source"
            source_path.mkdir()
            output_path = temp_path / "corpus.json"
            output_path.write_text("existing evidence\n", encoding="utf-8")
            environment = make_fake_git(
                temp_path, "72e07f717ae46a17822776df21ebd73dbc4ce728"
            )

            result = self.run_capture(
                source_path,
                output_path,
                environment,
                [sys.executable, "-c", "raise SystemExit(0)"],
            )

            self.assertEqual(
                output_path.read_text(encoding="utf-8"), "existing evidence\n"
            )

        self.assertEqual(result.returncode, 1)
        self.assertIn("already exists", result.stderr)
        self.assertIn("--force", result.stderr)

    def test_capture_rejects_checkout_at_another_revision(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            source_path = temp_path / "baseline-source"
            source_path.mkdir()
            output_path = temp_path / "corpus.json"
            environment = make_fake_git(temp_path, "0" * 40)

            result = self.run_capture(
                source_path,
                output_path,
                environment,
                [sys.executable, "-c", "raise SystemExit(0)"],
            )

            self.assertFalse(output_path.exists())

        self.assertEqual(result.returncode, 1)
        self.assertIn("0" * 40, result.stderr)
        self.assertIn("72e07f717ae46a17822776df21ebd73dbc4ce728", result.stderr)
        self.assertIn("git checkout", result.stderr)

    def test_capture_rejects_tracked_changes_at_pinned_revision(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            source_path = temp_path / "baseline-source"
            source_path.mkdir()
            output_path = temp_path / "corpus.json"
            environment = make_fake_git(
                temp_path,
                "72e07f717ae46a17822776df21ebd73dbc4ce728",
                " M universal/src/logging/level.cpp",
            )

            result = self.run_capture(
                source_path,
                output_path,
                environment,
                [sys.executable, "-c", "raise SystemExit(0)"],
            )

            self.assertFalse(output_path.exists())

        self.assertEqual(result.returncode, 1)
        self.assertIn("tracked changes", result.stderr)
        self.assertIn("universal/src/logging/level.cpp", result.stderr)
        self.assertIn("commit, stash, or revert", result.stderr)

    def test_capture_rejects_untracked_source_at_pinned_revision(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            source_path = temp_path / "baseline-source"
            source_path.mkdir()
            output_path = temp_path / "corpus.json"
            environment = make_fake_git(
                temp_path,
                "72e07f717ae46a17822776df21ebd73dbc4ce728",
                "?? universal/src/logging/local_override.cpp",
            )

            result = self.run_capture(
                source_path,
                output_path,
                environment,
                [sys.executable, "-c", "raise SystemExit(0)"],
            )

            self.assertFalse(output_path.exists())

        self.assertEqual(result.returncode, 1)
        self.assertIn("untracked files", result.stderr)
        self.assertIn("local_override.cpp", result.stderr)

    def test_capture_rejects_boolean_probe_schema_version(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            source_path = temp_path / "baseline-source"
            source_path.mkdir()
            output_path = temp_path / "corpus.json"
            environment = make_fake_git(temp_path, BASELINE_REVISION)
            result = self.run_capture(
                source_path,
                output_path,
                environment,
                probe_program(make_probe_document(schema_version=True)),
            )

            self.assertFalse(output_path.exists())

        self.assertEqual(result.returncode, 1)
        self.assertIn("Unsupported probe_schema_version True", result.stderr)

    def test_capture_accepts_explicitly_deterministic_case(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            source_path = temp_path / "baseline-source"
            source_path.mkdir()
            output_path = temp_path / "corpus.json"
            environment = make_fake_git(temp_path, BASELINE_REVISION)
            result = self.run_capture(
                source_path,
                output_path,
                environment,
                probe_program(make_probe_document()),
            )

            self.assertTrue(output_path.is_file())

        self.assertEqual(result.returncode, 0, result.stderr)

    def test_capture_accepts_two_runs_that_match_after_normalization(self):
        timestamps = [
            "2026-08-24T09:30:12.123456",
            "2026-08-24T09:30:13.654321",
        ]
        documents = []
        for timestamp in timestamps:
            document = make_probe_document(feature_ids=["API-010", "FMT-002"])
            case = document["cases"][0]
            case["id"] = "tskv-repeatable"
            case["observed"] = {
                "kind": "utf8",
                "format": "tskv",
                "value": f"tskv\ttimestamp={timestamp}\ttext=test\n",
            }
            case["normalization"] = [
                {
                    "kind": "timestamp_local",
                    "path": "/value",
                    "value": timestamp,
                    "occurrence": 0,
                }
            ]
            documents.append(document)

        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            source_path = temp_path / "baseline-source"
            source_path.mkdir()
            output_path = temp_path / "corpus.json"
            environment = make_fake_git(temp_path, BASELINE_REVISION)
            result = self.run_capture(
                source_path,
                output_path,
                environment,
                counting_probe_program(temp_path, documents),
            )
            corpus = json.loads(output_path.read_text(encoding="utf-8"))

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(corpus["provenance"]["capture_tool"], "ulog-baseline-corpus/3")
        self.assertIn(
            "timestamp=2000-01-02T03:04:05.123456",
            corpus["cases"][0]["observed"]["value"],
        )

    def test_independent_captures_are_byte_identical(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            source_path = temp_path / "baseline-source"
            source_path.mkdir()
            environment = make_fake_git(temp_path, BASELINE_REVISION)
            outputs = []
            for name in ("first", "second"):
                output_parent = temp_path / name
                output_parent.mkdir()
                output_path = output_parent / "corpus.json"
                result = self.run_capture(
                    source_path,
                    output_path,
                    environment,
                    probe_program(make_probe_document()),
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                outputs.append(output_path.read_bytes())

        self.assertEqual(outputs[0], outputs[1])

    def test_capture_rejects_two_normalized_runs_that_differ(self):
        documents = []
        for run_number in (1, 2):
            document = make_probe_document()
            document["cases"][0]["observed"]["value"] = (
                f"tskv\ttext=test\tnonce={run_number}\n"
            )
            documents.append(document)

        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            source_path = temp_path / "baseline-source"
            source_path.mkdir()
            output_path = temp_path / "corpus.json"
            environment = make_fake_git(temp_path, BASELINE_REVISION)
            result = self.run_capture(
                source_path,
                output_path,
                environment,
                counting_probe_program(temp_path, documents),
            )

            self.assertFalse(output_path.exists())

        self.assertEqual(result.returncode, 1)
        self.assertIn("changed between consecutive probe runs", result.stderr)
        self.assertIn("/cases/0/observed/value", result.stderr)
        self.assertIn("Declare every volatile occurrence", result.stderr)

    def test_capture_mismatch_preserves_existing_forced_destination(self):
        documents = []
        for run_number in (1, 2):
            document = make_probe_document()
            document["cases"][0]["observed"]["value"] = (
                f"tskv\ttext=test\tnonce={run_number}\n"
            )
            documents.append(document)

        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            source_path = temp_path / "baseline-source"
            source_path.mkdir()
            output_path = temp_path / "corpus.json"
            output_path.write_text("existing evidence\n", encoding="utf-8")
            environment = make_fake_git(temp_path, BASELINE_REVISION)
            result = self.run_capture(
                source_path,
                output_path,
                environment,
                counting_probe_program(temp_path, documents),
                force=True,
            )

            self.assertEqual(
                output_path.read_text(encoding="utf-8"), "existing evidence\n"
            )

        self.assertEqual(result.returncode, 1)
        self.assertIn("changed between consecutive probe runs", result.stderr)

    def test_capture_rejects_stale_probe_baseline(self):
        stale_baseline = {
            "repository": BASELINE_REPOSITORY,
            "revision": "0" * 40,
        }
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            source_path = temp_path / "baseline-source"
            source_path.mkdir()
            output_path = temp_path / "corpus.json"
            environment = make_fake_git(temp_path, BASELINE_REVISION)
            result = self.run_capture(
                source_path,
                output_path,
                environment,
                probe_program(make_probe_document(baseline=stale_baseline)),
            )

            self.assertFalse(output_path.exists())

        self.assertEqual(result.returncode, 1)
        self.assertIn("probe baseline", result.stderr)
        self.assertIn("0" * 40, result.stderr)
        self.assertIn(BASELINE_REVISION, result.stderr)

    def test_capture_rechecks_checkout_after_probe(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            source_path = temp_path / "baseline-source"
            source_path.mkdir()
            output_path = temp_path / "corpus.json"
            dirty_marker = temp_path / "dirty"
            environment = make_fake_git(
                temp_path, BASELINE_REVISION, dirty_marker=dirty_marker
            )
            command = [
                sys.executable,
                "-c",
                (
                    "from pathlib import Path; "
                    f"Path({str(dirty_marker)!r}).write_text('dirty'); "
                    f"print({json.dumps(json.dumps(make_probe_document()))})"
                ),
            ]
            result = self.run_capture(
                source_path, output_path, environment, command
            )

            self.assertFalse(output_path.exists())

        self.assertEqual(result.returncode, 1)
        self.assertIn("changed while the probe was running", result.stderr)
        self.assertIn("changed_during_probe.cpp", result.stderr)

    def test_capture_rejects_noncanonical_json_pointer(self):
        probe = make_probe_document()
        probe["cases"][0]["observed"] = {"items": ["AA BB"]}
        probe["cases"][0]["normalization"] = [
            {
                "kind": "platform",
                "path": "/items/0",
                "value": "AA",
                "occurrence": 0,
            },
            {
                "kind": "process_id",
                "path": "/items/00",
                "value": "BB",
                "occurrence": 0,
            },
        ]
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            source_path = temp_path / "baseline-source"
            source_path.mkdir()
            output_path = temp_path / "corpus.json"
            environment = make_fake_git(temp_path, BASELINE_REVISION)
            result = self.run_capture(
                source_path, output_path, environment, probe_program(probe)
            )

        self.assertEqual(result.returncode, 1)
        self.assertIn("non-canonical list index '00'", result.stderr)

    def test_capture_rejects_invalid_json_pointer_escape(self):
        probe = make_probe_document()
        probe["cases"][0]["normalization"] = [
            {
                "kind": "process_id",
                "path": "/value~2",
                "value": "42",
                "occurrence": 0,
            }
        ]
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            source_path = temp_path / "baseline-source"
            source_path.mkdir()
            output_path = temp_path / "corpus.json"
            environment = make_fake_git(temp_path, BASELINE_REVISION)
            result = self.run_capture(
                source_path, output_path, environment, probe_program(probe)
            )

        self.assertEqual(result.returncode, 1)
        self.assertIn("invalid JSON Pointer escape", result.stderr)

    def test_capture_does_not_overwrite_output_created_while_probe_runs(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            source_path = temp_path / "baseline-source"
            source_path.mkdir()
            output_path = temp_path / "corpus.json"
            environment = make_fake_git(temp_path, BASELINE_REVISION)
            command = [
                sys.executable,
                "-c",
                (
                    "from pathlib import Path; "
                    f"Path({str(output_path)!r}).write_text('concurrent evidence\\n'); "
                    f"print({json.dumps(json.dumps(make_probe_document()))})"
                ),
            ]
            result = self.run_capture(
                source_path, output_path, environment, command
            )

            self.assertEqual(
                output_path.read_text(encoding="utf-8"), "concurrent evidence\n"
            )

        self.assertEqual(result.returncode, 1)
        self.assertIn("appeared while capture was running", result.stderr)
        self.assertIn("--force", result.stderr)

    def test_capture_rejects_unknown_manifest_id_before_write(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            source_path = temp_path / "baseline-source"
            source_path.mkdir()
            output_path = temp_path / "corpus.json"
            environment = make_fake_git(temp_path, BASELINE_REVISION)
            result = self.run_capture(
                source_path,
                output_path,
                environment,
                probe_program(make_probe_document(feature_ids=["FMT-999"])),
            )

            self.assertFalse(output_path.exists())

        self.assertEqual(result.returncode, 1)
        self.assertIn("unknown manifest IDs: FMT-999", result.stderr)

    def test_capture_rejects_case_id_colliding_with_sibling_corpus(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            source_path = temp_path / "baseline-source"
            source_path.mkdir()
            sibling_path = temp_path / "existing.json"
            sibling_path.write_text(
                json.dumps(KNOWN_CORPUS, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
            output_path = temp_path / "new.json"
            environment = make_fake_git(temp_path, BASELINE_REVISION)
            result = self.run_capture(
                source_path,
                output_path,
                environment,
                probe_program(make_probe_document()),
            )

            self.assertFalse(output_path.exists())

        self.assertEqual(result.returncode, 1)
        self.assertIn("case id 'raw-basic' collides with sibling corpus", result.stderr)
        self.assertIn("existing.json", result.stderr)

    def test_capture_reports_output_failure_without_traceback(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            source_path = temp_path / "baseline-source"
            source_path.mkdir()
            output_path = temp_path / "corpus.json"
            output_path.mkdir()
            environment = make_fake_git(temp_path, BASELINE_REVISION)
            result = self.run_capture(
                source_path,
                output_path,
                environment,
                probe_program(make_probe_document()),
                force=True,
            )

        self.assertEqual(result.returncode, 1)
        self.assertIn("Unable to write corpus", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_capture_normalizes_probe_output_and_records_integrity(self):
        raw_timestamp = "2026-08-24T09:30:12.123456"
        raw_source_path = "C:\\checkout\\probe.cpp"
        raw_process_id = "4242"
        raw_thread_id = "77"
        raw_platform = "Windows"
        raw_capture = {
            "probe_schema_version": 1,
            "baseline": PROBE_BASELINE,
            "cases": [
                {
                    "id": "tskv-basic",
                    "feature_ids": ["FMT-002", "API-010"],
                    "difference_ids": [],
                    "platform": "portable",
                    "observed": {
                        "kind": "utf8",
                        "format": "tskv",
                        "value": (
                            f"tskv\ttimestamp={raw_timestamp}"
                            f"\tmodule={raw_source_path}"
                            f"\tprocess={raw_process_id}"
                            f"\tthread={raw_thread_id}"
                            f"\tplatform={raw_platform}"
                            f"\ttext={raw_platform} {raw_process_id}\n"
                        )
                    },
                    "normalization": [
                        {
                            "kind": "timestamp_local",
                            "path": "/value",
                            "value": raw_timestamp,
                            "occurrence": 0,
                        },
                        {
                            "kind": "source_path",
                            "path": "/value",
                            "value": raw_source_path,
                            "occurrence": 0,
                        },
                        {
                            "kind": "process_id",
                            "path": "/value",
                            "value": raw_process_id,
                            "occurrence": 0,
                        },
                        {
                            "kind": "thread_id",
                            "path": "/value",
                            "value": raw_thread_id,
                            "occurrence": 0,
                        },
                        {
                            "kind": "platform",
                            "path": "/value",
                            "value": raw_platform,
                            "occurrence": 0,
                        },
                    ],
                }
            ],
        }

        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            source_path = temp_path / "baseline-source"
            source_path.mkdir()
            output_path = temp_path / "corpus.json"
            probe_path = temp_path / "probe.py"
            probe_path.write_text(
                "import json\n"
                f"print({json.dumps(json.dumps(raw_capture))})\n",
                encoding="utf-8",
            )
            environment = make_fake_git(
                temp_path, "72e07f717ae46a17822776df21ebd73dbc4ce728"
            )

            result = self.run_capture(
                source_path,
                output_path,
                environment,
                [sys.executable, str(probe_path)],
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            corpus = json.loads(output_path.read_text(encoding="utf-8"))

        self.assertEqual(corpus["schema_version"], 1)
        self.assertEqual(
            corpus["provenance"],
            {
                "capture_tool": "ulog-baseline-corpus/3",
                "baseline_document": "docs/migration/baseline.md",
                "normalization_profile": "ulog-baseline-normalization/1",
                "repository": "user" + "ver",
                "revision": "72e07f717ae46a17822776df21ebd73dbc4ce728",
            },
        )
        self.assertEqual(
            corpus["cases"],
            [
                {
                    "id": "tskv-basic",
                    "feature_ids": ["API-010", "FMT-002"],
                    "difference_ids": [],
                    "platform": "portable",
                    "observed": {
                        "kind": "utf8",
                        "format": "tskv",
                        "value": (
                            "tskv\ttimestamp=2000-01-02T03:04:05.123456"
                            "\tmodule=<source-path>"
                            "\tprocess=<process-id>"
                            "\tthread=<thread-id>"
                            "\tplatform=<platform>\ttext=Windows 4242\n"
                        )
                    },
                }
            ],
        )
        self.assertEqual(corpus["integrity"]["algorithm"], "sha256")
        self.assertEqual(
            corpus["integrity"]["canonicalization"], "ulog-json-v1"
        )
        self.assertRegex(corpus["integrity"]["value"], r"^[0-9a-f]{64}$")


if __name__ == "__main__":
    unittest.main()
