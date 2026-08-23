#!/usr/bin/env python3

import json
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_conan_graph.py <graph.json>", file=sys.stderr)
        return 2

    graph_path = Path(sys.argv[1])
    graph = json.loads(graph_path.read_text(encoding="utf-8-sig"))
    nodes = graph.get("graph", {}).get("nodes", {})
    forbidden_name = "user" + "ver"
    violations = []
    for node in nodes.values():
        reference = node.get("ref")
        if not reference:
            continue
        if forbidden_name in reference.lower():
            violations.append(reference)

    if violations:
        print("forbidden standalone dependency references:", file=sys.stderr)
        for reference in violations:
            print(f"  - {reference}", file=sys.stderr)
        print("Move compatibility glue to the consuming project.", file=sys.stderr)
        return 1

    print(f"Conan graph independence check passed ({len(nodes)} nodes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
