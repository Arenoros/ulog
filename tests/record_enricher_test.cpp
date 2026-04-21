#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

#include <ulog/log.hpp>
#include <ulog/mem_logger.hpp>
#include <ulog/record_enricher.hpp>

namespace {

std::shared_ptr<ulog::MemLogger> Install(ulog::Format fmt = ulog::Format::kTskv) {
    auto logger = std::make_shared<ulog::MemLogger>(fmt);
    ulog::impl::SetDefaultLoggerRef(*logger);
    return logger;
}

bool Contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

void StaticAppHook(ulog::TagSink& sink, void* /*ctx*/) noexcept {
    sink.AddTag("app", "demo");
}

/// Fixture that clears enricher registry before and after each test so
/// cross-test state cannot leak through the process-global registry.
class RecordEnricherTest : public ::testing::Test {
protected:
    void SetUp() override { ulog::ClearRecordEnrichers(); }
    void TearDown() override {
        ulog::ClearRecordEnrichers();
        ulog::SetNullDefaultLogger();
    }
};

}  // namespace

TEST_F(RecordEnricherTest, AddThenEmitAttachesTag) {
    auto mem = Install();
    const auto h = ulog::AddRecordEnricher(&StaticAppHook);
    EXPECT_NE(h, 0u);

    LOG_INFO() << "hi";
    ASSERT_EQ(mem->GetRecords().size(), 1u);
    EXPECT_TRUE(Contains(mem->GetRecords().front(), "app=demo"));
}

TEST_F(RecordEnricherTest, RemoveStopsEmission) {
    auto mem = Install();
    const auto h = ulog::AddRecordEnricher(&StaticAppHook);
    ulog::RemoveRecordEnricher(h);

    LOG_INFO() << "hi";
    ASSERT_EQ(mem->GetRecords().size(), 1u);
    EXPECT_FALSE(Contains(mem->GetRecords().front(), "app="));
}

TEST_F(RecordEnricherTest, ClearRemovesAll) {
    auto mem = Install();
    ulog::AddRecordEnricher(&StaticAppHook);
    ulog::AddRecordEnricher(+[](ulog::TagSink& s, void*) noexcept { s.AddTag("k", "v"); });
    ulog::ClearRecordEnrichers();

    LOG_INFO() << "hi";
    ASSERT_EQ(mem->GetRecords().size(), 1u);
    EXPECT_FALSE(Contains(mem->GetRecords().front(), "app="));
    EXPECT_FALSE(Contains(mem->GetRecords().front(), "k=v"));
}

TEST_F(RecordEnricherTest, MultipleEnrichersFireInOrder) {
    auto mem = Install();
    ulog::AddRecordEnricher(+[](ulog::TagSink& s, void*) noexcept { s.AddTag("a", "1"); });
    ulog::AddRecordEnricher(+[](ulog::TagSink& s, void*) noexcept { s.AddTag("b", "2"); });

    LOG_INFO() << "hi";
    const auto rec = mem->GetRecords().front();
    const auto pos_a = rec.find("a=1");
    const auto pos_b = rec.find("b=2");
    ASSERT_NE(pos_a, std::string::npos);
    ASSERT_NE(pos_b, std::string::npos);
    EXPECT_LT(pos_a, pos_b) << rec;
}

TEST_F(RecordEnricherTest, UserCtxIsPassedThrough) {
    auto mem = Install();
    int counter = 0;
    ulog::AddRecordEnricher(
        [](ulog::TagSink& s, void* ctx) noexcept {
            auto* c = static_cast<int*>(ctx);
            ++(*c);
            s.AddTag("ctx_seen", "yes");
        },
        &counter);

    LOG_INFO() << "x";
    LOG_INFO() << "y";
    EXPECT_EQ(counter, 2);
    ASSERT_EQ(mem->GetRecords().size(), 2u);
    EXPECT_TRUE(Contains(mem->GetRecords().front(), "ctx_seen=yes"));
}

TEST_F(RecordEnricherTest, NullHookIsRejected) {
    const auto h = ulog::AddRecordEnricher(nullptr);
    EXPECT_EQ(h, 0u);
}

TEST_F(RecordEnricherTest, StaleHandleRemoveIsNoop) {
    ulog::RemoveRecordEnricher(999999);  // never issued
    SUCCEED();  // no crash
}

TEST_F(RecordEnricherTest, ThreadIdEnricherPopulatesTag) {
    auto mem = Install();
    ulog::EnableThreadIdEnricher("tid");

    LOG_INFO() << "hi";
    const auto rec = mem->GetRecords().front();
    EXPECT_TRUE(Contains(rec, "tid=")) << rec;
    // Some digits after `tid=` — hash output is decimal.
    const auto pos = rec.find("tid=");
    ASSERT_NE(pos, std::string::npos);
    ASSERT_GT(rec.size(), pos + 4);
    const char c = rec[pos + 4];
    EXPECT_TRUE(c >= '0' && c <= '9') << rec;
}

TEST_F(RecordEnricherTest, ThreadIdDiffersAcrossThreads) {
    auto mem = Install();
    ulog::EnableThreadIdEnricher("tid");

    LOG_INFO() << "main";
    std::string main_rec = mem->GetRecords().front();

    std::thread t([&]{
        LOG_INFO() << "worker";
    });
    t.join();

    ASSERT_GE(mem->GetRecords().size(), 2u);
    const std::string worker_rec = mem->GetRecords().back();

    auto extract_tid = [](std::string_view r) -> std::string {
        const auto pos = r.find("tid=");
        if (pos == std::string_view::npos) return {};
        const auto end = r.find_first_of(" \t\n", pos + 4);
        return std::string(r.substr(pos + 4, end - pos - 4));
    };
    const auto main_tid = extract_tid(main_rec);
    const auto worker_tid = extract_tid(worker_rec);
    EXPECT_FALSE(main_tid.empty());
    EXPECT_FALSE(worker_tid.empty());
    EXPECT_NE(main_tid, worker_tid);
}

TEST_F(RecordEnricherTest, EnrichmentFansOutToEveryFormat) {
    // Per-sink format sanity: enricher tags must reach all formatters,
    // not just the primary.
    auto logger = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    ulog::impl::SetDefaultLoggerRef(*logger);

    // Sanity: MemLogger is single-sink — this test only verifies that
    // the tag made it via the fanout path. Full multi-sink coverage
    // lives in per_sink_format_test.cpp.
    ulog::AddRecordEnricher(&StaticAppHook);

    LOG_INFO() << "fanout";
    ASSERT_EQ(logger->GetRecords().size(), 1u);
    EXPECT_TRUE(Contains(logger->GetRecords().front(), "app=demo"));
}

TEST_F(RecordEnricherTest, RemoveFromInsideCallbackIsSafe) {
    // Self-deregistering hook — common pattern for one-shots. The
    // implementation snapshots the registry before walking it, so
    // Remove() inside the callback affects only subsequent records.
    auto mem = Install();
    static std::atomic<int> fire_count{0};
    static ulog::RecordEnricherHandle h{0};
    h = ulog::AddRecordEnricher(+[](ulog::TagSink& s, void*) noexcept {
        ++fire_count;
        ulog::RemoveRecordEnricher(h);
        s.AddTag("once", "yes");
    });

    LOG_INFO() << "a";
    LOG_INFO() << "b";
    EXPECT_EQ(fire_count.load(), 1);
    ASSERT_EQ(mem->GetRecords().size(), 2u);
    EXPECT_TRUE(Contains(mem->GetRecords()[0], "once=yes"));
    EXPECT_FALSE(Contains(mem->GetRecords()[1], "once=yes"));
}

TEST_F(RecordEnricherTest, FastPathUnchangedWithNoEnrichers) {
    // No enrichers registered — output must match pre-feature behaviour.
    auto mem = Install();
    LOG_INFO() << "plain";
    const auto rec = mem->GetRecords().front();
    EXPECT_FALSE(Contains(rec, "app="));
    EXPECT_FALSE(Contains(rec, "tid="));
    EXPECT_FALSE(Contains(rec, "thread_name="));
}
