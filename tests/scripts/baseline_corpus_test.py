import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
CORPUS_TOOL = PROJECT_ROOT / "scripts" / "baseline_corpus.py"
CAPABILITY_MANIFEST = PROJECT_ROOT / "docs" / "migration" / "capability-manifest.md"
KNOWN_CORPUS = {
    "schema_version": 1,
    "provenance": {
        "baseline_document": "docs/migration/baseline.md",
        "capture_tool": "ulog-baseline-corpus/1",
        "normalization_profile": "ulog-baseline-normalization/1",
        "repository": "user" + "ver",
        "revision": "72e07f717ae46a17822776df21ebd73dbc4ce728",
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


def make_fake_git(
    temp_path: Path, revision: str, worktree_changes: str = ""
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
    and arguments[4] in {"--untracked-files=no", "--untracked-files=all"}
):
    changes = os.environ["FAKE_GIT_WORKTREE_CHANGES"].splitlines()
    if arguments[4] == "--untracked-files=no":
        changes = [change for change in changes if not change.startswith("??")]
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
    if os.name == "nt":
        environment["PATHEXT"] = f".CMD;{environment.get('PATHEXT', '')}"
    return environment


class BaselineCorpusTest(unittest.TestCase):
    def run_validation_root(
        self, corpus_root: Path
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
                str(CAPABILITY_MANIFEST),
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

    def test_validate_accepts_known_corpus_without_external_checkout(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            result = self.run_validation(KNOWN_CORPUS, Path(temp_directory))

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Validated 1 corpus file(s) with 1 case(s)", result.stdout)

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

            result = subprocess.run(
                [
                    sys.executable,
                    str(CORPUS_TOOL),
                    "capture",
                    "--baseline-source",
                    str(source_path),
                    "--output",
                    str(output_path),
                    "--",
                    sys.executable,
                    "-c",
                    "raise SystemExit(0)",
                ],
                capture_output=True,
                check=False,
                env=environment,
                text=True,
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

            result = subprocess.run(
                [
                    sys.executable,
                    str(CORPUS_TOOL),
                    "capture",
                    "--baseline-source",
                    str(source_path),
                    "--output",
                    str(output_path),
                    "--",
                    sys.executable,
                    "-c",
                    "raise SystemExit(0)",
                ],
                capture_output=True,
                check=False,
                env=environment,
                text=True,
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

            result = subprocess.run(
                [
                    sys.executable,
                    str(CORPUS_TOOL),
                    "capture",
                    "--baseline-source",
                    str(source_path),
                    "--output",
                    str(output_path),
                    "--",
                    sys.executable,
                    "-c",
                    "raise SystemExit(0)",
                ],
                capture_output=True,
                check=False,
                env=environment,
                text=True,
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

            result = subprocess.run(
                [
                    sys.executable,
                    str(CORPUS_TOOL),
                    "capture",
                    "--baseline-source",
                    str(source_path),
                    "--output",
                    str(output_path),
                    "--",
                    sys.executable,
                    "-c",
                    "raise SystemExit(0)",
                ],
                capture_output=True,
                check=False,
                env=environment,
                text=True,
            )

            self.assertFalse(output_path.exists())

        self.assertEqual(result.returncode, 1)
        self.assertIn("untracked files", result.stderr)
        self.assertIn("local_override.cpp", result.stderr)

    def test_capture_normalizes_probe_output_and_records_integrity(self):
        raw_timestamp = "2026-08-24T09:30:12.123456"
        raw_source_path = "C:\\checkout\\probe.cpp"
        raw_process_id = "4242"
        raw_thread_id = "77"
        raw_platform = "Windows"
        raw_capture = {
            "probe_schema_version": 1,
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

            result = subprocess.run(
                [
                    sys.executable,
                    str(CORPUS_TOOL),
                    "capture",
                    "--baseline-source",
                    str(source_path),
                    "--output",
                    str(output_path),
                    "--",
                    sys.executable,
                    str(probe_path),
                ],
                capture_output=True,
                check=False,
                env=environment,
                text=True,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            corpus = json.loads(output_path.read_text(encoding="utf-8"))

        self.assertEqual(corpus["schema_version"], 1)
        self.assertEqual(
            corpus["provenance"],
            {
                "capture_tool": "ulog-baseline-corpus/1",
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
