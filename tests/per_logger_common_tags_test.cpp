#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <ulog/log.hpp>
#include <ulog/mem_logger.hpp>
#include <ulog/record_enricher.hpp>

// Phase 44 — `LoggerBase::PrependCommonTags(TagSink&)` + CRUD API.
// Per-logger tags are published lock-free (atomic_shared_ptr snapshot)
// and appear on every emitted record alongside global enrichers.

namespace {

bool Contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

}  // namespace

TEST(PerLoggerCommonTags, TagAppearsOnRecord) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);
    mem->SetCommonTag("service", "ulog-test");
    mem->SetCommonTag("hostname", "localhost");

    ulog::LogHelper(*mem, ulog::Level::kInfo) << "hi";

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "service=ulog-test")) << recs[0];
    EXPECT_TRUE(Contains(recs[0], "hostname=localhost")) << recs[0];
}

TEST(PerLoggerCommonTags, TagsDoNotLeakBetweenLoggers) {
    auto a = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    auto b = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    a->SetLevel(ulog::Level::kTrace);
    b->SetLevel(ulog::Level::kTrace);

    a->SetCommonTag("logger", "A");
    b->SetCommonTag("logger", "B");

    ulog::LogHelper(*a, ulog::Level::kInfo) << "from A";
    ulog::LogHelper(*b, ulog::Level::kInfo) << "from B";

    auto ra = a->GetRecords();
    auto rb = b->GetRecords();
    ASSERT_EQ(ra.size(), 1u);
    ASSERT_EQ(rb.size(), 1u);
    EXPECT_TRUE(Contains(ra[0], "logger=A")) << ra[0];
    EXPECT_FALSE(Contains(ra[0], "logger=B")) << ra[0];
    EXPECT_TRUE(Contains(rb[0], "logger=B")) << rb[0];
    EXPECT_FALSE(Contains(rb[0], "logger=A")) << rb[0];
}

TEST(PerLoggerCommonTags, SetTwiceOverwrites) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);
    mem->SetCommonTag("env", "dev");
    mem->SetCommonTag("env", "prod");  // overwrite

    ulog::LogHelper(*mem, ulog::Level::kInfo) << "hi";
    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "env=prod")) << recs[0];
    // No stale "env=dev" remains in the record.
    auto pos = recs[0].find("env=");
    auto second = recs[0].find("env=", pos + 4);
    EXPECT_EQ(second, std::string::npos) << recs[0];
}

TEST(PerLoggerCommonTags, RemoveDropsTag) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);
    mem->SetCommonTag("a", "1");
    mem->SetCommonTag("b", "2");
    mem->RemoveCommonTag("a");

    ulog::LogHelper(*mem, ulog::Level::kInfo) << "hi";
    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_FALSE(Contains(recs[0], "a=1")) << recs[0];
    EXPECT_TRUE(Contains(recs[0], "b=2")) << recs[0];
}

TEST(PerLoggerCommonTags, ClearRemovesAll) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);
    mem->SetCommonTag("a", "1");
    mem->SetCommonTag("b", "2");
    mem->ClearCommonTags();

    ulog::LogHelper(*mem, ulog::Level::kInfo) << "hi";
    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_FALSE(Contains(recs[0], "a=1")) << recs[0];
    EXPECT_FALSE(Contains(recs[0], "b=2")) << recs[0];
}

TEST(PerLoggerCommonTags, GlobalEnricherAndCommonTagsCoexist) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);
    mem->SetCommonTag("service", "ulog-test");

    auto h = ulog::AddRecordEnricher(
        [](ulog::TagSink& sink, void*) noexcept {
            sink.AddTag("enriched", "yes");
        },
        nullptr);

    ulog::LogHelper(*mem, ulog::Level::kInfo) << "hi";
    ulog::RemoveRecordEnricher(h);

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "enriched=yes")) << recs[0];
    EXPECT_TRUE(Contains(recs[0], "service=ulog-test")) << recs[0];
}

TEST(PerLoggerCommonTags, ConcurrentSetAndLogIsSafe) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);
    // Seed before the writer thread starts so the very first emission
    // has a tag to read — the test exercises the concurrent update
    // path, not the uninitialised-snapshot one (covered elsewhere).
    mem->SetCommonTag("iter", "0");

    std::atomic<bool> stop{false};
    std::thread writer([&] {
        int i = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            mem->SetCommonTag("iter", std::to_string(i++));
        }
    });

    // Emit a bunch of records while the writer thread rotates the tag.
    for (int i = 0; i < 500; ++i) {
        ulog::LogHelper(*mem, ulog::Level::kInfo) << "concurrent";
    }

    stop.store(true, std::memory_order_relaxed);
    writer.join();

    const auto recs = mem->GetRecords();
    // All 500 records are present; each carries an `iter=<N>` tag.
    ASSERT_EQ(recs.size(), 500u);
    for (const auto& r : recs) {
        EXPECT_TRUE(Contains(r, "iter=")) << r;
    }
    mem->ClearCommonTags();
}
