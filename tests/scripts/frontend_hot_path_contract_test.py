import sys
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_DIRECTORY = PROJECT_ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS_DIRECTORY))

import frontend_hot_path_contract as contract  # noqa: E402


class FrontendHotPathContractTest(unittest.TestCase):
    def sources(self) -> dict[str, str]:
        return contract.read_project_sources(PROJECT_ROOT)

    def test_current_producer_sources_preserve_the_statistics_contract(self):
        contract.validate_project(PROJECT_ROOT)

    def test_rejects_a_dedicated_accepted_counter(self):
        sources = self.sources()
        sources["src/producer/producer_kernel.cpp"] = sources[
            "src/producer/producer_kernel.cpp"
        ].replace(
            "std::atomic<std::uint64_t> truncated_records{0};",
            "std::atomic<std::uint64_t> accepted_records{0};\n"
            "    std::atomic<std::uint64_t> truncated_records{0};",
        )
        with self.assertRaisesRegex(
            contract.FrontendHotPathContractError, "shared accepted-statistics"
        ):
            contract.validate_sources(sources)

    def test_requires_accepted_snapshots_to_reuse_the_admission_sequence(self):
        sources = self.sources()
        sources["src/producer/producer_kernel.cpp"] = sources[
            "src/producer/producer_kernel.cpp"
        ].replace(
            ".accepted_records = impl_->lanes.AcceptedCount(),",
            ".accepted_records = 0,",
        )
        with self.assertRaisesRegex(
            contract.FrontendHotPathContractError, "admission sequence"
        ):
            contract.validate_sources(sources)

    def test_rejects_an_accepted_counter_outside_producer_counters(self):
        sources = self.sources()
        sources["src/producer/producer_lanes.hpp"] = sources[
            "src/producer/producer_lanes.hpp"
        ].replace(
            "AdmissionSequence next_admission_sequence_{};",
            "std::atomic<std::uint64_t> accepted_statistics_ = 0;\n"
            "  AdmissionSequence next_admission_sequence_{};",
        )
        with self.assertRaisesRegex(
            contract.FrontendHotPathContractError, "atomic inventory"
        ):
            contract.validate_sources(sources)

    def test_rejects_an_accepted_counter_in_the_public_macro_path(self):
        sources = self.sources()
        sources["include/ulog/logger.hpp"] = sources["include/ulog/logger.hpp"].replace(
            "namespace detail {",
            "namespace detail {\n\n"
            "inline std::atomic<std::uint64_t> accepted_statistics = 0;",
        )
        with self.assertRaisesRegex(
            contract.FrontendHotPathContractError, "atomic inventory"
        ):
            contract.validate_sources(sources)

    def test_rejects_extra_shared_mutation_on_the_accepted_path(self):
        sources = self.sources()
        sources["src/producer/producer_kernel.cpp"] = sources[
            "src/producer/producer_kernel.cpp"
        ].replace(
            "const auto sequence = impl_->lanes.Publish(",
            "impl_->consumed_records.fetch_add(1, std::memory_order_relaxed);\n"
            "  const auto sequence = impl_->lanes.Publish(",
        )
        with self.assertRaisesRegex(
            contract.FrontendHotPathContractError,
            "only isolated exceptional outcome counters",
        ):
            contract.validate_sources(sources)

    def test_requires_one_atomic_default_target_load_per_getter(self):
        sources = self.sources()
        sources["src/logger.cpp"] = sources["src/logger.cpp"].replace(
            "return detail::LoggerAccess::FromState("
            "kDefaultLoggerState.load(std::memory_order_acquire));",
            "const auto* ignored = "
            "kDefaultLoggerState.load(std::memory_order_relaxed);\n"
            "  static_cast<void>(ignored);\n"
            "  return detail::LoggerAccess::FromState("
            "kDefaultLoggerState.load(std::memory_order_acquire));",
        )
        with self.assertRaisesRegex(
            contract.FrontendHotPathContractError, "exactly one atomic target load"
        ):
            contract.validate_sources(sources)


if __name__ == "__main__":
    unittest.main()
