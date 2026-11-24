"""Checks that the JSON consumer's golden lines match the committed JSON corpus.

tests/consumer/json_runtime_main.cpp compares Ulog's JSON output byte for byte with
tests/consumer/json_corpus_frames.jsonl. This test parses each golden line the way the corpus was
captured and compares its normalized members with the case's committed members, so key order,
typed values, nested JSON, escaping, and framing all trace back to the baseline. The only allowed
difference is the DEF-004 field-key policy: a repeated user key keeps one member, at its first
position with its last value, and reserved keys never appear as user members.
"""

import json
import sys
import unittest
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_DIRECTORY = PROJECT_ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS_DIRECTORY))

import baseline_corpus  # noqa: E402

JSON_CORPUS = PROJECT_ROOT / "docs" / "migration" / "corpus" / "json-v1.json"
GOLDEN_LINES = PROJECT_ROOT / "tests" / "consumer" / "json_corpus_frames.jsonl"

# IsReservedFieldKey in include/ulog/fields.hpp, compared ignoring ASCII case.
RESERVED_KEYS = {
    "timestamp",
    "level",
    "module",
    "task_id",
    "thread_id",
    "text",
    "@timestamp",
    "levelstr",
    "message",
}


def is_reserved(key: str) -> bool:
    lowered = "".join(
        chr(ord(character) + 32) if "A" <= character <= "Z" else character
        for character in key
    )
    return lowered in RESERVED_KEYS or lowered.startswith("ulog.")


def apply_field_key_policy(members: list[dict[str, object]]) -> list[dict[str, object]]:
    """Applies DEF-004 to the user members between module and the final text member."""
    head, user_members, tail = members[:3], members[3:-1], members[-1:]
    kept: list[dict[str, object]] = []
    positions: dict[str, int] = {}
    for member in user_members:
        key = str(member["key"])
        if is_reserved(key):
            continue
        if key in positions:
            kept[positions[key]] = member
        else:
            positions[key] = len(kept)
            kept.append(member)
    return head + kept + tail


def golden_observation(line: str, observed_format: str, case_id: str) -> dict[str, object]:
    return baseline_corpus.normalize_json_observation(
        {"kind": "json-line", "format": observed_format, "value": line}, case_id
    )


class JsonCorpusFramesTest(unittest.TestCase):
    def test_golden_lines_match_every_committed_case(self) -> None:
        corpus = json.loads(JSON_CORPUS.read_text(encoding="utf-8"))
        cases = sorted(corpus["cases"], key=lambda case: case["id"])
        lines = GOLDEN_LINES.read_bytes().decode("utf-8").splitlines(keepends=True)
        self.assertEqual(len(lines), len(cases))
        for case, line in zip(cases, lines):
            with self.subTest(case=case["id"]):
                observed = case["observed"]
                golden = golden_observation(line, observed["format"], case["id"])
                expected = dict(observed)
                expected["members"] = apply_field_key_policy(observed["members"])
                if expected["members"] != observed["members"]:
                    self.assertIn("DEF-004", case["difference_ids"])
                self.assertEqual(golden, expected)

    def test_policy_drops_reserved_and_repeated_user_keys_only(self) -> None:
        def member(key: str, value: str) -> dict[str, object]:
            return {"key": key, "value": {"kind": "string", "value": value}}

        members = [
            member("timestamp", "t"),
            member("level", "INFO"),
            member("module", "m"),
            member("a", "1"),
            member("LevelStr", "x"),
            member("a", "2"),
            member("ulog.x", "y"),
            member("b", "3"),
            member("text", "body"),
        ]
        self.assertEqual(
            apply_field_key_policy(members),
            [
                member("timestamp", "t"),
                member("level", "INFO"),
                member("module", "m"),
                member("a", "2"),
                member("b", "3"),
                member("text", "body"),
            ],
        )


if __name__ == "__main__":
    unittest.main()
