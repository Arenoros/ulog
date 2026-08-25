import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "scripts"))

from conan_cache_key import hash_profile
from benchmark_results_test import make_document
from record_storage_results_test import make_document as make_record_storage_document


def make_fake_conan_environment(temp_path: Path, profile: object) -> dict[str, str]:
    driver = temp_path / "fake_conan.py"
    driver.write_text(
        """import os
import sys

expected_arguments = [
    "profile",
    "show",
    "-pr:h=conan/profiles/cpp20",
    "-pr:b=default",
    f"-s:h=build_type={os.environ['FAKE_CONAN_BUILD_TYPE']}",
    "--format=json",
]
if sys.argv[1:] != expected_arguments:
    print(f"unexpected arguments: {sys.argv[1:]!r}", file=sys.stderr)
    raise SystemExit(9)
if os.environ.get("FAKE_CONAN_FAIL"):
    print("fake Conan profile failure", file=sys.stderr)
    raise SystemExit(7)
print(os.environ["FAKE_CONAN_PROFILE"])
""",
        encoding="utf-8",
    )

    if os.name == "nt":
        wrapper = temp_path / "conan.cmd"
        wrapper.write_text(
            '@echo off\r\n"%FAKE_CONAN_PYTHON%" "%FAKE_CONAN_DRIVER%" %*\r\n',
            encoding="utf-8",
        )
    else:
        wrapper = temp_path / "conan"
        wrapper.write_text(
            '#!/bin/sh\nexec "$FAKE_CONAN_PYTHON" "$FAKE_CONAN_DRIVER" "$@"\n',
            encoding="utf-8",
        )
        wrapper.chmod(0o755)

    environment = os.environ.copy()
    environment["PATH"] = f"{temp_path}{os.pathsep}{environment['PATH']}"
    environment["FAKE_CONAN_BUILD_TYPE"] = "Release"
    environment["FAKE_CONAN_DRIVER"] = str(driver)
    environment["FAKE_CONAN_PROFILE"] = json.dumps(profile)
    environment["FAKE_CONAN_PYTHON"] = sys.executable
    if os.name == "nt":
        environment["PATHEXT"] = f".CMD;{environment.get('PATHEXT', '')}"
    return environment


class ScriptToolsTest(unittest.TestCase):
    def test_conan_cache_key_tracks_profile_identity(self):
        profile = {
            "host_profile": {
                "settings": {
                    "compiler": "gcc",
                    "compiler.version": "13",
                    "os": "Linux",
                }
            }
        }
        reordered_profile = {
            "host_profile": {
                "settings": {
                    "os": "Linux",
                    "compiler.version": "13",
                    "compiler": "gcc",
                }
            }
        }
        newer_compiler_profile = {
            "host_profile": {
                "settings": {
                    "compiler": "gcc",
                    "compiler.version": "14",
                    "os": "Linux",
                }
            }
        }

        self.assertEqual(hash_profile(profile), hash_profile(reordered_profile))
        self.assertNotEqual(hash_profile(profile), hash_profile(newer_compiler_profile))

    def test_conan_cache_key_cli_writes_github_output(self):
        profile = {
            "host": {
                "settings": {
                    "compiler": "gcc",
                    "compiler.version": "13",
                    "os": "Linux",
                }
            }
        }
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            github_output = temp_path / "github-output.txt"
            environment = make_fake_conan_environment(temp_path, profile)
            environment["GITHUB_OUTPUT"] = str(github_output)
            result = subprocess.run(
                [
                    sys.executable,
                    str(PROJECT_ROOT / "scripts" / "conan_cache_key.py"),
                    "Release",
                ],
                capture_output=True,
                check=False,
                env=environment,
                text=True,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), hash_profile(profile))
            self.assertEqual(
                github_output.read_text(encoding="utf-8").strip(),
                f"hash={hash_profile(profile)}",
            )

    def test_conan_cache_key_cli_explains_profile_failure(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            environment = make_fake_conan_environment(Path(temp_directory), {})
            environment["FAKE_CONAN_FAIL"] = "1"
            result = subprocess.run(
                [
                    sys.executable,
                    str(PROJECT_ROOT / "scripts" / "conan_cache_key.py"),
                    "Release",
                ],
                capture_output=True,
                check=False,
                env=environment,
                text=True,
            )

        self.assertEqual(result.returncode, 7)
        self.assertIn("fake Conan profile failure", result.stderr)
        self.assertIn("conan profile detect --force", result.stderr)

    def test_graph_guard_rejects_reference_containing_forbidden_name(self):
        forbidden_reference = "adapter-" + "user" + "ver" + "-bridge/1.0.0"
        graph = {
            "graph": {
                "nodes": {
                    "0": {"ref": "ulog/0.1.0"},
                    "1": {"ref": forbidden_reference},
                }
            }
        }

        with tempfile.TemporaryDirectory() as temp_directory:
            graph_path = Path(temp_directory) / "graph.json"
            graph_path.write_text(json.dumps(graph), encoding="utf-8")
            result = subprocess.run(
                [
                    sys.executable,
                    str(PROJECT_ROOT / "scripts" / "check_conan_graph.py"),
                    str(graph_path),
                ],
                capture_output=True,
                check=False,
                text=True,
            )

        self.assertEqual(result.returncode, 1)
        self.assertIn(forbidden_reference, result.stderr)

    def test_benchmark_collector_selects_newest_result(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            conan_home = temp_path / "conan-home"
            older_result = conan_home / "p" / "b" / "z-cache" / "benchmark-results.json"
            newer_result = conan_home / "p" / "b" / "a-cache" / "benchmark-results.json"
            destination = temp_path / "collected.json"
            older_result.parent.mkdir(parents=True)
            newer_result.parent.mkdir(parents=True)
            older_document = make_document()
            older_document["context"]["generation"] = "older"
            newer_document = make_document()
            newer_document["context"]["generation"] = "newer"
            older_result.write_text(json.dumps(older_document), encoding="utf-8")
            newer_result.write_text(json.dumps(newer_document), encoding="utf-8")
            os.utime(older_result, ns=(1_000_000_000, 1_000_000_000))
            os.utime(newer_result, ns=(2_000_000_000, 2_000_000_000))

            environment = os.environ.copy()
            environment["CONAN_HOME"] = str(conan_home)
            result = subprocess.run(
                [
                    sys.executable,
                    str(PROJECT_ROOT / "scripts" / "collect_benchmark_results.py"),
                    str(destination),
                ],
                capture_output=True,
                check=False,
                env=environment,
                text=True,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(
                json.loads(destination.read_text(encoding="utf-8"))["context"][
                    "generation"
                ],
                "newer",
            )

    def test_benchmark_collector_rejects_invalid_newest_result_before_copy(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            conan_home = temp_path / "conan-home"
            older_result = conan_home / "p" / "b" / "older" / "benchmark-results.json"
            newer_result = conan_home / "p" / "b" / "newer" / "benchmark-results.json"
            destination = temp_path / "collected.json"
            older_result.parent.mkdir(parents=True)
            newer_result.parent.mkdir(parents=True)
            older_result.write_text(json.dumps(make_document()), encoding="utf-8")
            newer_result.write_text('{"context": {}}', encoding="utf-8")
            os.utime(older_result, ns=(1_000_000_000, 1_000_000_000))
            os.utime(newer_result, ns=(2_000_000_000, 2_000_000_000))
            destination.write_text("preserve-me", encoding="utf-8")

            environment = os.environ.copy()
            environment["CONAN_HOME"] = str(conan_home)
            result = subprocess.run(
                [
                    sys.executable,
                    str(PROJECT_ROOT / "scripts" / "collect_benchmark_results.py"),
                    str(destination),
                ],
                capture_output=True,
                check=False,
                env=environment,
                text=True,
            )

            self.assertEqual(result.returncode, 1)
            self.assertIn(str(newer_result), result.stderr)
            self.assertIn("ulog_result_protocol", result.stderr)
            self.assertIn("retry", result.stderr)
            self.assertNotIn("Traceback", result.stderr)
            self.assertEqual(destination.read_text(encoding="utf-8"), "preserve-me")

    def test_benchmark_collector_publishes_both_result_protocols(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            temp_path = Path(temp_directory)
            conan_home = temp_path / "conan-home"
            result_root = conan_home / "p" / "b" / "latest"
            reservation_source = result_root / "benchmark-results.json"
            record_storage_source = result_root / "record-storage-results.json"
            reservation_destination = temp_path / "reservation.json"
            record_storage_destination = temp_path / "record-storage.json"
            result_root.mkdir(parents=True)
            reservation_source.write_text(
                json.dumps(make_document()), encoding="utf-8"
            )
            record_storage_source.write_text(
                json.dumps(make_record_storage_document()), encoding="utf-8"
            )

            environment = os.environ.copy()
            environment["CONAN_HOME"] = str(conan_home)
            result = subprocess.run(
                [
                    sys.executable,
                    str(PROJECT_ROOT / "scripts" / "collect_benchmark_results.py"),
                    str(reservation_destination),
                    str(record_storage_destination),
                ],
                capture_output=True,
                check=False,
                env=environment,
                text=True,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(
                json.loads(reservation_destination.read_text(encoding="utf-8"))[
                    "context"
                ]["ulog_result_protocol"],
                "ulog-workload-results/4",
            )
            self.assertEqual(
                json.loads(record_storage_destination.read_text(encoding="utf-8"))[
                    "context"
                ]["ulog_result_protocol"],
                "ulog-record-storage-results/2",
            )


if __name__ == "__main__":
    unittest.main()
