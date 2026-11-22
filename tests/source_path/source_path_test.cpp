#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <ulog/log.hpp>
#include <ulog/runtime.hpp>
#include <ulog/source_location.hpp>
#include <ulog/testing/in_memory_destination.hpp>
#include <utility>

#include "sites.hpp"

namespace ulog::test {
namespace {

using namespace std::chrono_literals;
using detail::SourcePathPrefixLength;

// Separator boundaries.
static_assert(SourcePathPrefixLength("/src/app/main.cpp", "/src/app") == 9);
static_assert(SourcePathPrefixLength("/src/app/main.cpp", "/src/app/") == 9);
static_assert(SourcePathPrefixLength("/src/app///main.cpp", "/src/app") == 11);
static_assert(SourcePathPrefixLength("/src/app/main.cpp", "/src/ap") == 0);
static_assert(SourcePathPrefixLength("/src/application/main.cpp", "/src/app") == 0);
static_assert(SourcePathPrefixLength("/src/app", "/src/app") == 0);
static_assert(SourcePathPrefixLength("/src/app//", "/src/app") == 0);
static_assert(SourcePathPrefixLength("/src/app/main.cpp", "") == 0);
static_assert(SourcePathPrefixLength("/src/app/main.cpp", "/other") == 0);
#ifdef _WIN32
static_assert(SourcePathPrefixLength(R"(C:\src\app\main.cpp)", "C:/src/app") == 11);
static_assert(SourcePathPrefixLength(R"(C:\src\app\\/main.cpp)", "C:/src/app/") == 13);
static_assert(SourcePathPrefixLength("C:/src/app/main.cpp", R"(C:\src\app)") == 11);
static_assert(SourcePathPrefixLength(R"(C:\src\application\main.cpp)", "C:/src/app") == 0);
#else
static_assert(SourcePathPrefixLength(R"(/src/app\main.cpp)", "/src/app") == 0);
#endif

// The first matching prefix wins, and only the path changes.
constexpr SourceLocation kTrimmed = detail::TrimSourcePath(
    SourceLocation::Custom("/src/app/lib/x.cpp", "Fn", 7, 3), {"/src/app/li", "/src", "/src/app"});
static_assert(kTrimmed.GetFileName() == "app/lib/x.cpp");
static_assert(kTrimmed.GetFunctionName() == "Fn");
static_assert(kTrimmed.GetLine() == 7 && kTrimmed.GetColumn() == 3);
static_assert(detail::TrimSourcePath(SourceLocation::Custom("/elsewhere/x.cpp", "", 1), {"/src"})
                  .GetFileName() == "/elsewhere/x.cpp");

// The call-site identity is a constant expression.
static_assert(ULOG_SOURCE_LOCATION().GetLine() != 0);

constexpr std::size_t kRecordBytes = 1024;

class SourcePathTrimming : public ::testing::Test {
 protected:
  void SetUp() override {
    auto created = Runtime::Create(
        RuntimeConfig{
            .threshold = Level::kInfo,
            .payload_capacity_bytes = 2 * kRecordBytes,
            .maximum_record_bytes = kRecordBytes,
            .producer_slots = 1,
            .ingress_cells = 2,
            .control_operations = 2,
            .worker_threads = 1,
            .startup_timeout = 1s,
            .destruction_timeout = 1s,
        },
        destination_);
    ASSERT_TRUE(created);
    runtime_ = std::move(created.runtime);
  }

  // Logs through `log`, then checks that the delivered Record and the call-site identity agree.
  void ExpectSite(SourcePathSite (*log)(Logger), std::string_view expected_path) {
    const SourcePathSite site = log(runtime_->GetLogger());
    auto drained = runtime_->Drain();
    ASSERT_TRUE(drained);
    ASSERT_EQ(drained.operation.WaitUntil(std::chrono::steady_clock::now() + 1s).status,
              OperationWaitStatus::kCompleted);
    std::optional<testing::ObservedRecord> record = destination_.TryTake();
    ASSERT_TRUE(record.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access): asserted above.
    EXPECT_EQ(record->SourcePath(), expected_path);
    EXPECT_EQ(record->SourcePath(), site.identity.GetFileName());
    EXPECT_EQ(record->SourceFunction(), site.identity.GetFunctionName());
    // NOLINTEND(bugprone-unchecked-optional-access)
  }

  testing::InMemoryDestination destination_{
      {.capacity_records = 2, .maximum_record_bytes = kRecordBytes}};
  std::unique_ptr<Runtime> runtime_;
};

TEST_F(SourcePathTrimming, EachConfiguredPrefixTrimsItsTranslationUnit) {
  ExpectSite(&LogFromFirst, "site.cpp");
  ExpectSite(&LogFromSecond, "site.cpp");
}

TEST_F(SourcePathTrimming, PrefixesEndingInsideADirectoryNameDoNotMatch) {
  const SourcePathSite site = LogFromFirstExtra(runtime_->GetLogger());
  EXPECT_EQ(site.identity.GetFileName(), site.raw_path);
  EXPECT_TRUE(site.raw_path.ends_with("site.cpp"));
  ExpectSite(&LogFromFirstExtra, site.raw_path);
}

TEST_F(SourcePathTrimming, UnmatchedPathsKeepTheCompilerPath) {
  EXPECT_EQ(ULOG_SOURCE_LOCATION().GetFileName(), SourceLocation::Current().GetFileName());
  ExpectSite(
      +[](Logger logger) -> SourcePathSite {
        LOG_INFO_TO(logger, "unmatched");
        return {ULOG_SOURCE_LOCATION(), SourceLocation::Current().GetFileName()};
      },
      SourceLocation::Current().GetFileName());
}

}  // namespace
}  // namespace ulog::test
