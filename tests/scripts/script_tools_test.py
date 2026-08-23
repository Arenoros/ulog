import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]


class ScriptToolsTest(unittest.TestCase):
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
            older_result.write_text('{"generation": "older"}', encoding="utf-8")
            newer_result.write_text('{"generation": "newer"}', encoding="utf-8")
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
                json.loads(destination.read_text(encoding="utf-8")),
                {"generation": "newer"},
            )


if __name__ == "__main__":
    unittest.main()
