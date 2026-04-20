#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <ulog/log.hpp>
#include <ulog/mem_logger.hpp>

// Phase 43 — `kSizeLimit = 10000`. Streaming writes beyond the cap
// become no-ops and the emitted record gains a `truncated=true` tag.

TEST(LogHelperTruncation, ShortMessageNoTruncatedTag) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);
    ulog::SetDefaultLogger(mem);

    LOG_INFO() << "short message";

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].find("truncated=true"), std::string::npos) << recs[0];
    ulog::SetDefaultLogger(nullptr);
}

TEST(LogHelperTruncation, LargeMessageTrimsAndTags) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    // Stream ~15 KB in 1 KB chunks — well past the 10 KB cap.
    const std::string chunk(1024, 'x');
    {
        ulog::LogHelper helper(*mem, ulog::Level::kInfo);
        for (int i = 0; i < 15; ++i) {
            helper << chunk;
        }
    }  // dtor flushes

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    // truncated=true tag is present.
    EXPECT_NE(recs[0].find("truncated=true"), std::string::npos) << recs[0];
    // TSKV emits `text=...` as the last field; measure its body length
    // from `text=` to the trailing newline. Upper-bound: one chunk past
    // the cap (10000 + 1024 = 11024). Lower-bound: at least the cap.
    auto text_pos = recs[0].find("text=");
    ASSERT_NE(text_pos, std::string::npos) << recs[0];
    const std::size_t text_body_begin = text_pos + 5;
    std::size_t text_end = recs[0].find('\n', text_body_begin);
    if (text_end == std::string::npos) text_end = recs[0].size();
    const std::size_t text_len = text_end - text_body_begin;
    EXPECT_GE(text_len, 10000u);       // at least cap
    EXPECT_LE(text_len, 11500u);       // not runaway past one chunk
}

TEST(LogHelperTruncation, IsLimitReachedPredicate) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    {
        ulog::LogHelper h(*mem, ulog::Level::kInfo);
        EXPECT_FALSE(h.IsLimitReached());
        h << std::string(5000, 'a');
        EXPECT_FALSE(h.IsLimitReached());
        h << std::string(6000, 'b');
        EXPECT_TRUE(h.IsLimitReached());
        // Writes after the limit are no-ops — size stays roughly the same.
        h << std::string(5000, 'c');
        EXPECT_TRUE(h.IsLimitReached());
    }  // dtor flushes
}
