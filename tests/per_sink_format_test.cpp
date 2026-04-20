#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <ulog/async_logger.hpp>
#include <ulog/log.hpp>
#include <ulog/sinks/base_sink.hpp>
#include <ulog/sync_logger.hpp>

namespace {

/// Test sink — captures every `Write` call. Thread-safe so AsyncLogger
/// can drain onto it from its worker thread while the test reads back.
class CaptureSink final : public ulog::sinks::BaseSink {
public:
    void Write(std::string_view payload) override {
        std::lock_guard lk(mu_);
        records_.emplace_back(payload);
    }
    void Flush() override {}

    std::vector<std::string> Records() const {
        std::lock_guard lk(mu_);
        return records_;
    }

private:
    mutable std::mutex mu_;
    std::vector<std::string> records_;
};

bool Contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

}  // namespace

TEST(PerSinkFormat, SyncDifferentFormatsOneRecord) {
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);

    auto tskv_sink = std::make_shared<CaptureSink>();
    auto json_sink = std::make_shared<CaptureSink>();
    logger->AddSink(tskv_sink);                                  // inherits TSKV
    logger->AddSink(json_sink, ulog::Format::kJson);             // override → JSON

    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "payload";
    ulog::LogFlush();
    ulog::SetDefaultLogger(nullptr);

    const auto tskv_recs = tskv_sink->Records();
    const auto json_recs = json_sink->Records();
    ASSERT_EQ(tskv_recs.size(), 1u);
    ASSERT_EQ(json_recs.size(), 1u);

    const auto& tskv = tskv_recs.front();
    const auto& json = json_recs.front();

    EXPECT_TRUE(Contains(tskv, "text=payload")) << tskv;
    EXPECT_FALSE(Contains(tskv, "\"text\":\"payload\"")) << tskv;

    EXPECT_TRUE(Contains(json, "\"text\":\"payload\"")) << json;
    EXPECT_FALSE(Contains(json, "text=payload")) << json;
}

TEST(PerSinkFormat, SyncSameFormatReusesSinglePayload) {
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);

    auto a = std::make_shared<CaptureSink>();
    auto b = std::make_shared<CaptureSink>();
    logger->AddSink(a);
    logger->AddSink(b, ulog::Format::kTskv);  // same as base

    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "same";
    ulog::LogFlush();
    ulog::SetDefaultLogger(nullptr);

    ASSERT_EQ(a->Records().size(), 1u);
    ASSERT_EQ(b->Records().size(), 1u);
    EXPECT_EQ(a->Records().front(), b->Records().front());
}

TEST(PerSinkFormat, SyncThreeDistinctFormats) {
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);

    auto tskv = std::make_shared<CaptureSink>();
    auto ltsv = std::make_shared<CaptureSink>();
    auto json = std::make_shared<CaptureSink>();
    logger->AddSink(tskv);                                     // base TSKV
    logger->AddSink(ltsv, ulog::Format::kLtsv);
    logger->AddSink(json, ulog::Format::kJson);

    ulog::SetDefaultLogger(logger);
    LOG_WARNING() << "triple";
    ulog::LogFlush();
    ulog::SetDefaultLogger(nullptr);

    ASSERT_EQ(tskv->Records().size(), 1u);
    ASSERT_EQ(ltsv->Records().size(), 1u);
    ASSERT_EQ(json->Records().size(), 1u);

    EXPECT_TRUE(Contains(tskv->Records().front(), "text=triple"));
    EXPECT_TRUE(Contains(ltsv->Records().front(), "text:triple"));
    EXPECT_TRUE(Contains(json->Records().front(), "\"text\":\"triple\""));
}

TEST(PerSinkFormat, SyncTagsFanOutToAllFormats) {
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);

    auto tskv = std::make_shared<CaptureSink>();
    auto json = std::make_shared<CaptureSink>();
    logger->AddSink(tskv);
    logger->AddSink(json, ulog::Format::kJson);

    ulog::SetDefaultLogger(logger);
    {
        ulog::LogExtra extra;
        extra.Extend("user_id", std::string("42"));
        LOG_INFO() << extra << "payload";
    }
    ulog::LogFlush();
    ulog::SetDefaultLogger(nullptr);

    const auto t = tskv->Records().front();
    const auto j = json->Records().front();
    EXPECT_TRUE(Contains(t, "user_id=42")) << t;
    EXPECT_TRUE(Contains(j, "\"user_id\":\"42\"")) << j;
}

TEST(PerSinkFormat, AsyncDifferentFormatsOneRecord) {
    ulog::AsyncLogger::Config cfg;
    cfg.format = ulog::Format::kTskv;
    auto logger = std::make_shared<ulog::AsyncLogger>(cfg);
    logger->SetLevel(ulog::Level::kTrace);

    auto tskv_sink = std::make_shared<CaptureSink>();
    auto json_sink = std::make_shared<CaptureSink>();
    logger->AddSink(tskv_sink);
    logger->AddSink(json_sink, ulog::Format::kJson);

    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "async-multi";
    logger->Flush();  // blocks until worker drains
    ulog::SetDefaultLogger(nullptr);

    ASSERT_EQ(tskv_sink->Records().size(), 1u);
    ASSERT_EQ(json_sink->Records().size(), 1u);
    EXPECT_TRUE(Contains(tskv_sink->Records().front(), "text=async-multi"));
    EXPECT_TRUE(Contains(json_sink->Records().front(), "\"text\":\"async-multi\""));
}

TEST(PerSinkFormat, SyncRawFormatOverride) {
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);

    auto tskv = std::make_shared<CaptureSink>();
    auto raw = std::make_shared<CaptureSink>();
    logger->AddSink(tskv);
    logger->AddSink(raw, ulog::Format::kRaw);

    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "payload";
    ulog::LogFlush();
    ulog::SetDefaultLogger(nullptr);

    const auto tskv_recs = tskv->Records();
    const auto raw_recs = raw->Records();
    ASSERT_EQ(tskv_recs.size(), 1u);
    ASSERT_EQ(raw_recs.size(), 1u);

    EXPECT_TRUE(Contains(tskv_recs.front(), "timestamp=")) << tskv_recs.front();
    EXPECT_TRUE(Contains(tskv_recs.front(), "text=payload"));
    // kRaw omits the default header fields (timestamp/level/module).
    EXPECT_EQ(raw_recs.front().find("timestamp="), std::string::npos) << raw_recs.front();
    EXPECT_EQ(raw_recs.front().find("level="), std::string::npos) << raw_recs.front();
    EXPECT_TRUE(Contains(raw_recs.front(), "text=payload")) << raw_recs.front();
}

TEST(PerSinkFormat, AsyncThreeDistinctFormatsBatched) {
    ulog::AsyncLogger::Config cfg;
    cfg.format = ulog::Format::kTskv;
    auto logger = std::make_shared<ulog::AsyncLogger>(cfg);
    logger->SetLevel(ulog::Level::kTrace);

    auto tskv = std::make_shared<CaptureSink>();
    auto ltsv = std::make_shared<CaptureSink>();
    auto json = std::make_shared<CaptureSink>();
    logger->AddSink(tskv);
    logger->AddSink(ltsv, ulog::Format::kLtsv);
    logger->AddSink(json, ulog::Format::kJson);

    ulog::SetDefaultLogger(logger);
    for (int i = 0; i < 5; ++i) {
        LOG_INFO() << "batch-" << i;
    }
    logger->Flush();
    ulog::SetDefaultLogger(nullptr);

    const auto tskv_recs = tskv->Records();
    const auto ltsv_recs = ltsv->Records();
    const auto json_recs = json->Records();
    ASSERT_EQ(tskv_recs.size(), 5u);
    ASSERT_EQ(ltsv_recs.size(), 5u);
    ASSERT_EQ(json_recs.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        const auto needle = "batch-" + std::to_string(i);
        EXPECT_TRUE(Contains(tskv_recs[i], "text=" + needle)) << tskv_recs[i];
        EXPECT_TRUE(Contains(ltsv_recs[i], "text:" + needle)) << ltsv_recs[i];
        EXPECT_TRUE(Contains(json_recs[i], "\"text\":\"" + needle + "\"")) << json_recs[i];
    }
}

TEST(PerSinkFormat, PerSinkLevelGateWithFormatOverride) {
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);

    auto tskv = std::make_shared<CaptureSink>();
    auto json = std::make_shared<CaptureSink>();
    json->SetLevel(ulog::Level::kError);  // JSON sink drops INFO
    logger->AddSink(tskv);
    logger->AddSink(json, ulog::Format::kJson);

    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "info-only";
    LOG_ERROR() << "both";
    ulog::LogFlush();
    ulog::SetDefaultLogger(nullptr);

    const auto tskv_recs = tskv->Records();
    const auto json_recs = json->Records();
    ASSERT_EQ(tskv_recs.size(), 2u);
    ASSERT_EQ(json_recs.size(), 1u) << "JSON sink should have filtered INFO out";
    EXPECT_TRUE(Contains(json_recs.front(), "\"text\":\"both\""));
}

TEST(PerSinkFormat, LoggerLevelFilterSkipsEntireRecord) {
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kError);  // logger drops everything below ERROR

    auto tskv = std::make_shared<CaptureSink>();
    auto json = std::make_shared<CaptureSink>();
    logger->AddSink(tskv);
    logger->AddSink(json, ulog::Format::kJson);

    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "filtered-out";
    LOG_ERROR() << "kept";
    ulog::LogFlush();
    ulog::SetDefaultLogger(nullptr);

    const auto tskv_recs = tskv->Records();
    const auto json_recs = json->Records();
    EXPECT_EQ(tskv_recs.size(), 1u);
    EXPECT_EQ(json_recs.size(), 1u);
    EXPECT_TRUE(Contains(tskv_recs.front(), "text=kept"));
    EXPECT_TRUE(Contains(json_recs.front(), "\"text\":\"kept\""));
}

TEST(PerSinkFormat, DuplicateOverrideFormatReusesIndex) {
    // Two sinks with the SAME non-base format override must share the single
    // extra formatter — their payloads must byte-equal.
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);

    auto json_a = std::make_shared<CaptureSink>();
    auto json_b = std::make_shared<CaptureSink>();
    logger->AddSink(json_a, ulog::Format::kJson);
    logger->AddSink(json_b, ulog::Format::kJson);

    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "dup";
    ulog::LogFlush();
    ulog::SetDefaultLogger(nullptr);

    const auto a = json_a->Records();
    const auto b = json_b->Records();
    ASSERT_EQ(a.size(), 1u);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(a.front(), b.front());
    EXPECT_TRUE(Contains(a.front(), "\"text\":\"dup\""));
}

TEST(PerSinkFormat, SyncEmptyRegistryLogsStillWork) {
    // No overrides — base format remains the single active format. The
    // multi-format code path must remain invisible on this hot path.
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);
    auto sink = std::make_shared<CaptureSink>();
    logger->AddSink(sink);

    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "plain";
    ulog::LogFlush();
    ulog::SetDefaultLogger(nullptr);

    ASSERT_EQ(sink->Records().size(), 1u);
    EXPECT_TRUE(Contains(sink->Records().front(), "text=plain"));
}
