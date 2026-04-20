#include <atomic>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <ulog/log.hpp>
#include <ulog/mem_logger.hpp>

// Phase 49 — LogHelper niceties bundle:
//   * LogHelper::Format(fmt, args...) — fmt-forwarding helper method.
//   * operator<<(const std::atomic<T>&) — .load() dispatch.
//   * operator<<(Fun) — callable hook for deferred/conditional formatting.

namespace {

bool Contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

}  // namespace

// ---- LogHelper::Format ----------------------------------------------------

TEST(Niceties, FormatInline) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    ulog::LogHelper(*mem, ulog::Level::kInfo)
        .Format("user={} id={}", "alice", 42);

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "text=user=alice id=42")) << recs[0];
}

TEST(Niceties, FormatChainedWithStream) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    ulog::LogHelper(*mem, ulog::Level::kInfo)
        .Format("x={}", 7) << " y=" << 8;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "text=x=7 y=8")) << recs[0];
}

// ---- std::atomic<T> --------------------------------------------------------

TEST(Niceties, AtomicIntStreamsLoaded) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    std::atomic<int> a{7};
    ulog::LogHelper(*mem, ulog::Level::kInfo) << "a=" << a;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "a=7")) << recs[0];
}

TEST(Niceties, AtomicBoolStreamsTrueFalse) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    std::atomic<bool> a{true};
    ulog::LogHelper(*mem, ulog::Level::kInfo) << "a=" << a;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "a=true")) << recs[0];
}

// ---- callable overload -----------------------------------------------------

TEST(Niceties, CallableInvokedWithHelper) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    ulog::LogHelper(*mem, ulog::Level::kInfo)
        << "pre "
        << [](ulog::LogHelper& h) { h << "inside"; }
        << " post";

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "text=pre inside post")) << recs[0];
}

TEST(Niceties, CallableRespectsIsLimitReached) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    bool expensive_ran = false;
    ulog::LogHelper(*mem, ulog::Level::kInfo)
        << std::string(11000, 'x')  // puts text size past the cap
        << [&](ulog::LogHelper& h) {
            // Callable sees the helper post-cap — it can check and skip.
            if (h.IsLimitReached()) {
                expensive_ran = false;
                return;
            }
            expensive_ran = true;
            h << "should-not-appear";
        };

    EXPECT_FALSE(expensive_ran);
    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "truncated=true")) << recs[0];
    EXPECT_FALSE(Contains(recs[0], "should-not-appear")) << recs[0];
}

TEST(Niceties, CallableCapturingInt) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    const int value = 123;
    ulog::LogHelper(*mem, ulog::Level::kInfo)
        << "v="
        << [&](ulog::LogHelper& h) { h << value; };

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "text=v=123")) << recs[0];
}
