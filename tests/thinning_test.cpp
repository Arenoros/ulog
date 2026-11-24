#include "producer/thinning.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <ulog/level.hpp>
#include <ulog/source_location.hpp>
#include <vector>

#include "producer/producer_kernel.hpp"

namespace ulog::detail::producer {
namespace {

static_assert(ThinningKeepRatio(0, 1'024, 4'096) == kThinningScale);
static_assert(ThinningKeepRatio(1'024, 1'024, 4'096) == kThinningScale);
static_assert(ThinningKeepRatio(4'096, 1'024, 4'096) == 0U);
static_assert(ThinningKeepRatio(2'560, 1'024, 4'096) == kThinningScale / 2U);
// The same site and nonce always decide alike, at compile time too.
static_assert(ThinningKeeps(CallSiteHash("a.cpp", 7), 42, kThinningScale / 2U) ==
              ThinningKeeps(CallSiteHash("a.cpp", 7), 42, kThinningScale / 2U));
static_assert(CallSiteHash("a.cpp", 7) != CallSiteHash("a.cpp", 8));
static_assert(CallSiteHash("a.cpp", 7) != CallSiteHash("b.cpp", 7));

TEST(AdaptiveThinning, KeepRatioFallsLinearlyAboveTheLowWatermark) {
  std::uint32_t previous = kThinningScale;
  for (std::size_t occupied = 1'024; occupied <= 4'096; occupied += 64) {
    const std::uint32_t ratio = ThinningKeepRatio(occupied, 1'024, 4'096);
    EXPECT_LE(ratio, previous) << occupied;
    previous = ratio;
  }
  EXPECT_EQ(ThinningKeepRatio(1'792, 1'024, 4'096), kThinningScale * 3U / 4U);
  EXPECT_EQ(ThinningKeepRatio(3'328, 1'024, 4'096), kThinningScale / 4U);
}

TEST(AdaptiveThinning, DecisionsMatchTheKeepRatioAndNeverDependOnState) {
  const std::uint64_t site = CallSiteHash("src/app.cpp", 12);
  EXPECT_FALSE(ThinningKeeps(site, 0, 0));
  constexpr std::uint64_t kNonces = 200'000;
  for (const std::uint32_t ratio :
       {0U, kThinningScale / 4U, kThinningScale / 2U, kThinningScale * 3U / 4U, kThinningScale}) {
    std::uint64_t kept = 0;
    for (std::uint64_t nonce = 0; nonce < kNonces; ++nonce) {
      kept += ThinningKeeps(site, nonce, ratio) ? 1U : 0U;
    }
    const double expected = static_cast<double>(ratio) / kThinningScale;
    EXPECT_NEAR(static_cast<double>(kept) / kNonces, expected, 0.01) << ratio;
    // Replaying the same nonces gives the same decisions.
    for (std::uint64_t nonce = 0; nonce < 1'000; ++nonce) {
      EXPECT_EQ(ThinningKeeps(site, nonce, ratio), ThinningKeeps(site, nonce, ratio));
    }
  }
}

TEST(AdaptiveThinning, EverySiteKeepsAboutTheSameShare) {
  constexpr std::uint64_t kNonces = 20'000;
  for (std::uint32_t line = 1; line <= 64; ++line) {
    const std::uint64_t site = CallSiteHash("src/service/handler.cpp", line);
    std::uint64_t kept = 0;
    for (std::uint64_t nonce = 0; nonce < kNonces; ++nonce) {
      kept += ThinningKeeps(site, nonce, kThinningScale / 2U) ? 1U : 0U;
    }
    EXPECT_NEAR(static_cast<double>(kept) / kNonces, 0.5, 0.03) << line;
  }
}

BuildStatus CountBuild(void* context, RecordAppender&) {
  ++*static_cast<std::size_t*>(context);
  return BuildStatus::kComplete;
}

/// Publishes `attempts` calls without consuming and returns each outcome.
[[nodiscard]] std::vector<PublishOutcome> PublishWithoutConsuming(std::size_t attempts,
                                                                  std::size_t& evaluations) {
  ProducerKernel kernel{KernelConfig{
      .threshold = Level::kTrace,
      .payload_capacity_bytes = 16'384,
      .maximum_record_bytes = 256,
      .producer_slots = 1,
      .ingress_cells = 64,
      .adaptive_thinning = true,
      .thinning_low_watermark_bytes = 4'096,
  }};
  auto producer = kernel.TryRegisterProducer();
  std::vector<PublishOutcome> outcomes;
  for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
    outcomes.push_back(kernel
                           .TryPublish(producer, Level::kInfo,
                                       SourceLocation::Custom("thin.cpp", "Thin", 3),
                                       BuildOperation{&evaluations, &CountBuild})
                           .outcome);
  }
  const auto snapshot = kernel.GetSnapshot();
  std::size_t thinned = 0;
  std::size_t accepted = 0;
  for (const PublishOutcome outcome : outcomes) {
    thinned += outcome == PublishOutcome::kThinned ? 1U : 0U;
    accepted += outcome == PublishOutcome::kAccepted ? 1U : 0U;
  }
  EXPECT_EQ(snapshot.rejected_thinned, thinned);
  EXPECT_EQ(snapshot.accepted_records, accepted);
  return outcomes;
}

TEST(AdaptiveThinning, KernelKeepsEverythingBelowTheWatermarkThenThinsWithoutEvaluating) {
  std::size_t evaluations = 0;
  const std::vector<PublishOutcome> outcomes = PublishWithoutConsuming(64, evaluations);
  // Each small Record takes 128 bytes from the shared budget, so the first 32 Records stay at or
  // below the 4096-byte watermark and are all kept.
  for (std::size_t attempt = 0; attempt < 32; ++attempt) {
    EXPECT_EQ(outcomes[attempt], PublishOutcome::kAccepted) << attempt;
  }
  std::size_t thinned = 0;
  std::size_t accepted = 0;
  for (const PublishOutcome outcome : outcomes) {
    thinned += outcome == PublishOutcome::kThinned ? 1U : 0U;
    accepted += outcome == PublishOutcome::kAccepted ? 1U : 0U;
  }
  EXPECT_GT(thinned, 0U);
  // A thinned call builds nothing and consumes no admission sequence.
  EXPECT_EQ(evaluations, accepted);

  // The same calls against a fresh kernel give exactly the same decisions.
  std::size_t replay_evaluations = 0;
  EXPECT_EQ(PublishWithoutConsuming(64, replay_evaluations), outcomes);
}

}  // namespace
}  // namespace ulog::detail::producer
