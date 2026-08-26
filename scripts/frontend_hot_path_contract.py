#!/usr/bin/env python3

import argparse
import re
import sys
from collections import Counter
from pathlib import Path


EXPECTED_PRODUCER_COUNTERS = {
    "rejected_lane_full",
    "rejected_budget",
    "abandoned_builds",
    "invalid_records",
    "truncated_records",
}
EXPECTED_ATOMIC_INVENTORY = Counter(
    {
        ("src/logger.cpp", "kDefaultLoggerState"): 1,
        ("src/logger_state.hpp", "threshold"): 1,
        ("src/producer/credit_ledger.hpp", "returned_credit_bytes"): 1,
        ("src/producer/credit_ledger.hpp", "logical_charge_bytes"): 1,
        ("src/producer/credit_ledger.hpp", "central_available_bytes_"): 1,
        ("src/producer/producer_kernel.cpp", "next_missing_producer_shard"): 1,
        ("src/producer/producer_kernel.cpp", "next_kernel_identity"): 1,
        ("src/producer/producer_kernel.cpp", "registered_generation"): 1,
        ("src/producer/producer_kernel.cpp", "references"): 1,
        ("src/producer/producer_kernel.cpp", "generation"): 1,
        ("src/producer/producer_kernel.cpp", "state"): 1,
        ("src/producer/producer_kernel.cpp", "rejected_lane_full"): 1,
        ("src/producer/producer_kernel.cpp", "rejected_budget"): 1,
        ("src/producer/producer_kernel.cpp", "abandoned_builds"): 1,
        ("src/producer/producer_kernel.cpp", "invalid_records"): 1,
        ("src/producer/producer_kernel.cpp", "truncated_records"): 1,
        ("src/producer/producer_kernel.cpp", "rejections"): 1,
        ("src/producer/producer_kernel.cpp", "consumed_records"): 1,
        ("src/producer/producer_kernel.cpp", "consumer_validation_errors"): 1,
        ("src/producer/producer_kernel.cpp", "admission_open"): 1,
        ("src/producer/producer_lanes.hpp", "read_position"): 1,
        ("src/producer/producer_lanes.hpp", "published_position"): 1,
        ("src/producer/producer_lanes.hpp", "publishing"): 1,
        ("src/producer/producer_lanes.hpp", "value"): 1,
    }
)
EXPECTED_PUBLISH_COUNTER_MUTATIONS = Counter(
    {
        "counters.rejected_lane_full": 1,
        "counters.rejected_budget": 1,
        "counters.abandoned_builds": 2,
        "counters.invalid_records": 6,
        "counters.truncated_records": 1,
    }
)
FORBIDDEN_CONTROL_REFERENCES = (
    "#include <ulog/operation.hpp>",
    '#include "control/',
    "detail::control",
    "ControlReserve",
    "OperationCallback",
    "OperationWait",
)
FORBIDDEN_BLOCKING_OR_OWNING_PRIMITIVES = (
    "std::mutex",
    "std::recursive_mutex",
    "std::condition_variable",
    "std::shared_mutex",
    "std::lock_guard",
    "std::unique_lock",
    "std::scoped_lock",
    "std::shared_ptr",
    "std::weak_ptr",
)
ATOMIC_DECLARATION = re.compile(
    r"(?m)^[ \t]*(?:(?:inline|static|constinit|const)\s+)*"
    r"std::atomic(?:<[^;\n]+>|_flag)\s+"
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)"
    r"\s*(?:\{[^;\n]*\}|\([^;\n]*\)|=\s*[^;\n]+)?\s*;"
)


class FrontendHotPathContractError(RuntimeError):
    pass


def require_match(source: str, pattern: str, description: str) -> re.Match[str]:
    match = re.search(pattern, source, flags=re.DOTALL)
    if not match:
        raise FrontendHotPathContractError(
            f"Could not find {description}. Keep the producer contract structure explicit or "
            "update this guard together with docs/performance-contract.md."
        )
    return match


def require_source(sources: dict[str, str], path: str) -> str:
    try:
        return sources[path]
    except KeyError as error:
        raise FrontendHotPathContractError(
            f"Missing required hot-path source {path!r}. Check ULOG_SOURCE_DIR and retry."
        ) from error


def validate_atomic_inventory(sources: dict[str, str]) -> None:
    actual = Counter(
        (path, match.group("name"))
        for path, source in sources.items()
        for match in ATOMIC_DECLARATION.finditer(source)
    )
    if actual == EXPECTED_ATOMIC_INVENTORY:
        return
    added = sorted((actual - EXPECTED_ATOMIC_INVENTORY).elements())
    removed = sorted((EXPECTED_ATOMIC_INVENTORY - actual).elements())
    raise FrontendHotPathContractError(
        "The macro-to-producer atomic inventory changed. The accepted path may use only the "
        "documented target/threshold load, synchronization, credit accounting, exceptional, "
        "consumer, and admission-sequence atomics; it must not add shared accepted-statistics "
        f"traffic. Added: {added}; removed: {removed}. Review the performance contract and "
        "update this guard only for a required non-statistics atomic."
    )


def validate_control_isolation(sources: dict[str, str]) -> None:
    control_references = sorted(
        (path, token)
        for path, source in sources.items()
        for token in FORBIDDEN_CONTROL_REFERENCES
        if token in source
    )
    if control_references:
        raise FrontendHotPathContractError(
            "The ordinary logger/producer path gained a control-state dependency. Operation and "
            f"control-reserve code must remain outside LOG* calls: {control_references}."
        )

    forbidden_primitives = sorted(
        (path, token)
        for path, source in sources.items()
        for token in FORBIDDEN_BLOCKING_OR_OWNING_PRIMITIVES
        if token in source
    )
    if forbidden_primitives:
        raise FrontendHotPathContractError(
            "The ordinary logger/producer path gained a blocking or shared-ownership primitive. "
            "Keep locks, waits, and reference counting in the separate control path: "
            f"{forbidden_primitives}."
        )


def validate_sources(sources: dict[str, str]) -> None:
    producer_kernel = require_source(sources, "src/producer/producer_kernel.cpp")
    producer_lanes = require_source(sources, "src/producer/producer_lanes.hpp")
    logger = require_source(sources, "src/logger.cpp")
    counters_match = require_match(
        producer_kernel,
        r"struct alignas\(kAccountingQuantumBytes\) ProducerCounters final \{"
        r"(?P<body>.*?)\n  \};",
        "ProducerKernel::Impl::ProducerCounters",
    )
    counter_names = set(
        re.findall(
            r"std::atomic<std::uint64_t>\s+([a-z0-9_]+)\{0\};",
            counters_match.group("body"),
        )
    )
    if counter_names != EXPECTED_PRODUCER_COUNTERS:
        raise FrontendHotPathContractError(
            "ProducerCounters must contain only the isolated exceptional outcome counters "
            f"{sorted(EXPECTED_PRODUCER_COUNTERS)}; found {sorted(counter_names)}. "
            "Do not add a shared accepted-statistics write to the ordinary producer path."
        )

    validate_atomic_inventory(sources)
    validate_control_isolation(sources)

    snapshot_match = require_match(
        producer_kernel,
        r"KernelSnapshot snapshot\{(?P<body>.*?)\n  \};",
        "the KernelSnapshot initializer",
    )
    if ".accepted_records = impl_->lanes.AcceptedCount()," not in snapshot_match.group(
        "body"
    ):
        raise FrontendHotPathContractError(
            "KernelSnapshot.accepted_records must remain derived from the admission sequence via "
            "ProducerLanes::AcceptedCount(), not from a dedicated shared statistics counter."
        )

    accepted_count_match = require_match(
        producer_lanes,
        r"\[\[nodiscard\]\] std::uint64_t AcceptedCount\(\) const noexcept \{"
        r"(?P<body>.*?)\n  \}",
        "ProducerLanes::AcceptedCount",
    )
    accepted_count_body = accepted_count_match.group("body")
    if (
        "return next_admission_sequence_.value.load(std::memory_order_relaxed);"
        not in accepted_count_body
    ):
        raise FrontendHotPathContractError(
            "ProducerLanes::AcceptedCount must read the mandatory admission sequence; a separate "
            "accepted-statistics counter would add forbidden shared hot-path traffic."
        )

    sequence_increment = (
        "next_admission_sequence_.value.fetch_add(1, std::memory_order_relaxed)"
    )
    if producer_lanes.count(sequence_increment) != 1:
        raise FrontendHotPathContractError(
            "ProducerLanes must perform exactly one shared admission-sequence increment at "
            "publication. Keep statistics derived from that required protocol operation."
        )

    publish_match = require_match(
        producer_lanes,
        r"\[\[nodiscard\]\] std::optional<std::uint64_t> Publish\(.*?\) noexcept \{"
        r"(?P<body>.*?)\n  \}",
        "ProducerLanes::Publish",
    )
    publish_body = publish_match.group("body")
    forbidden_publish_mutations = (
        ".fetch_sub(",
        ".exchange(",
        ".compare_exchange_",
        ".test_and_set(",
    )
    if (
        publish_body.count(".fetch_add(") != 1
        or sequence_increment not in publish_body
        or publish_body.count(".store(") != 1
        or "lane.published_position.store(" not in publish_body
        or publish_body.count(".clear(") != 1
        or "lane.publishing.clear(" not in publish_body
        or any(mutation in publish_body for mutation in forbidden_publish_mutations)
    ):
        raise FrontendHotPathContractError(
            "ProducerLanes::Publish may mutate only the mandatory admission sequence and lane "
            "publication synchronization. Do not add shared accepted-statistics traffic."
        )

    try_publish_match = require_match(
        producer_kernel,
        r"PublishResult ProducerKernel::TryPublishSlot\(.*?BuildOperation operation\) \{"
        r"(?P<body>.*?)\n\}",
        "ProducerKernel::TryPublishSlot",
    )
    try_publish_body = try_publish_match.group("body")
    observed_counter_mutations = Counter(
        re.findall(r"\b(counters\.[a-z0-9_]+)\.fetch_add\(", try_publish_body)
    )
    if (
        observed_counter_mutations != EXPECTED_PUBLISH_COUNTER_MUTATIONS
        or try_publish_body.count(".fetch_add(")
        != sum(EXPECTED_PUBLISH_COUNTER_MUTATIONS.values())
        or any(
            mutation in try_publish_body
            for mutation in (
                ".fetch_sub(",
                ".exchange(",
                ".compare_exchange_",
                ".test_and_set(",
                ".store(",
            )
        )
    ):
        raise FrontendHotPathContractError(
            "ProducerKernel::TryPublishSlot may update only isolated exceptional outcome "
            "counters directly. Accepted statistics must remain derived from the admission "
            "sequence."
        )

    default_getter_match = require_match(
        logger,
        r"Logger GetDefaultLogger\(\) noexcept \{(?P<body>.*?)\n\}",
        "GetDefaultLogger",
    )
    default_getter_body = default_getter_match.group("body")
    default_loads = re.findall(r"kDefaultLoggerState\s*\.\s*load\s*\(", default_getter_body)
    if (
        len(default_loads) != 1
        or "kDefaultLoggerState.load(std::memory_order_acquire)" not in default_getter_body
    ):
        raise FrontendHotPathContractError(
            "GetDefaultLogger must perform exactly one atomic target load with acquire ordering. "
            "Each active unnamed LOG call invokes this getter exactly once."
        )


def read_source(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as error:
        raise FrontendHotPathContractError(
            f"Unable to read {path}: {error}. Check ULOG_SOURCE_DIR and retry."
        ) from error


def read_project_sources(project_root: Path) -> dict[str, str]:
    source_paths = [
        project_root / "include" / "ulog" / "log.hpp",
        project_root / "include" / "ulog" / "logger.hpp",
        project_root / "src" / "logger.cpp",
        project_root / "src" / "logger_state.hpp",
    ]
    source_paths.extend(
        path
        for path in (project_root / "src" / "producer").rglob("*")
        if path.suffix in (".cpp", ".hpp")
    )
    return {
        path.relative_to(project_root).as_posix(): read_source(path)
        for path in sorted(source_paths)
    }


def validate_project(project_root: Path) -> None:
    validate_sources(read_project_sources(project_root))


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Guard the producer frontend against shared accepted-statistics traffic."
    )
    parser.add_argument("--source-dir", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        validate_project(arguments.source_dir)
    except FrontendHotPathContractError as error:
        print(f"frontend hot-path contract check failed: {error}", file=sys.stderr)
        return 1
    print("Validated the producer frontend shared-statistics contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
