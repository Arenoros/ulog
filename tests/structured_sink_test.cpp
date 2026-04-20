#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <ulog/async_logger.hpp>
#include <ulog/log.hpp>
#include <ulog/log_extra.hpp>
#include <ulog/sinks/base_sink.hpp>
#include <ulog/sinks/structured_sink.hpp>
#include <ulog/sync_logger.hpp>
#include <ulog/tracing_hook.hpp>

namespace {

class CaptureTextSink final : public ulog::sinks::BaseSink {
public:
    void Write(std::string_view p) override {
        std::lock_guard lk(mu_);
        records_.emplace_back(p);
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

class CaptureStructuredSink final : public ulog::sinks::StructuredSink {
public:
    void Write(const ulog::sinks::LogRecord& record) override {
        std::lock_guard lk(mu_);
        records_.push_back(record);  // copy — the view is single-call scoped
    }
    void Flush() override {}

    std::vector<ulog::sinks::LogRecord> Records() const {
        std::lock_guard lk(mu_);
        return records_;
    }

    std::size_t Count() const {
        std::lock_guard lk(mu_);
        return records_.size();
    }

private:
    mutable std::mutex mu_;
    std::vector<ulog::sinks::LogRecord> records_;
};

const ulog::sinks::Tag* FindTag(const ulog::sinks::LogRecord& r, std::string_view key) {
    for (const auto& t : r.tags) {
        if (t.key == key) return &t;
    }
    return nullptr;
}

}  // namespace

TEST(StructuredSink, SyncReceivesLevelAndText) {
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);
    auto s_sink = std::make_shared<CaptureStructuredSink>();
    logger->AddStructuredSink(s_sink);

    ulog::SetDefaultLogger(logger);
    LOG_WARNING() << "hello";
    ulog::LogFlush();
    ulog::SetDefaultLogger(nullptr);

    const auto recs = s_sink->Records();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs.front().level, ulog::Level::kWarning);
    EXPECT_EQ(recs.front().text, "hello");
    EXPECT_FALSE(recs.front().location.file_name().empty());
    EXPECT_GT(recs.front().location.line(), 0u);
}

TEST(StructuredSink, TagsPreserveNativeTypes) {
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);
    auto s_sink = std::make_shared<CaptureStructuredSink>();
    logger->AddStructuredSink(s_sink);

    ulog::SetDefaultLogger(logger);
    {
        ulog::LogExtra extra;
        extra.Extend("count", std::int64_t{42});
        extra.Extend("ratio", 3.14);
        extra.Extend("ok",    true);
        extra.Extend("name",  std::string{"alice"});
        LOG_INFO() << extra << "payload";
    }
    ulog::SetDefaultLogger(nullptr);

    const auto recs = s_sink->Records();
    ASSERT_EQ(recs.size(), 1u);
    const auto& r = recs.front();
    EXPECT_EQ(r.text, "payload");

    const auto* count = FindTag(r, "count");
    ASSERT_NE(count, nullptr);
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(count->value));
    EXPECT_EQ(std::get<std::int64_t>(count->value), 42);

    const auto* ratio = FindTag(r, "ratio");
    ASSERT_NE(ratio, nullptr);
    ASSERT_TRUE(std::holds_alternative<double>(ratio->value));
    EXPECT_DOUBLE_EQ(std::get<double>(ratio->value), 3.14);

    const auto* ok = FindTag(r, "ok");
    ASSERT_NE(ok, nullptr);
    ASSERT_TRUE(std::holds_alternative<bool>(ok->value));
    EXPECT_TRUE(std::get<bool>(ok->value));

    const auto* name = FindTag(r, "name");
    ASSERT_NE(name, nullptr);
    ASSERT_TRUE(std::holds_alternative<std::string>(name->value));
    EXPECT_EQ(std::get<std::string>(name->value), "alice");
}

TEST(StructuredSink, StructuredOnlyLoggerSkipsTextFormatter) {
    // Logger with zero text sinks + one structured sink: the LogHelper
    // must not materialize a text formatter, and the record still reaches
    // the structured sink intact.
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);
    auto s_sink = std::make_shared<CaptureStructuredSink>();
    logger->AddStructuredSink(s_sink);
    // Note: no text sinks attached.

    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "structured-only";
    ulog::SetDefaultLogger(nullptr);

    const auto recs = s_sink->Records();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs.front().text, "structured-only");
    EXPECT_EQ(recs.front().level, ulog::Level::kInfo);
}

TEST(StructuredSink, MixedTextAndStructuredFanOut) {
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);
    auto text = std::make_shared<CaptureTextSink>();
    auto structured = std::make_shared<CaptureStructuredSink>();
    logger->AddSink(text);
    logger->AddStructuredSink(structured);

    ulog::SetDefaultLogger(logger);
    {
        ulog::LogExtra extra;
        extra.Extend("user", std::string{"bob"});
        LOG_ERROR() << extra << "oops";
    }
    ulog::LogFlush();
    ulog::SetDefaultLogger(nullptr);

    // Both paths observe the record.
    const auto text_recs = text->Records();
    const auto struct_recs = structured->Records();
    ASSERT_EQ(text_recs.size(), 1u);
    ASSERT_EQ(struct_recs.size(), 1u);

    EXPECT_NE(text_recs.front().find("text=oops"), std::string::npos);
    EXPECT_NE(text_recs.front().find("user=bob"), std::string::npos);

    EXPECT_EQ(struct_recs.front().text, "oops");
    const auto* user = FindTag(struct_recs.front(), "user");
    ASSERT_NE(user, nullptr);
    ASSERT_TRUE(std::holds_alternative<std::string>(user->value));
    EXPECT_EQ(std::get<std::string>(user->value), "bob");
}

TEST(StructuredSink, PerSinkLevelGate) {
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);
    auto s_sink = std::make_shared<CaptureStructuredSink>();
    s_sink->SetLevel(ulog::Level::kError);
    logger->AddStructuredSink(s_sink);

    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "below";
    LOG_ERROR() << "at";
    ulog::SetDefaultLogger(nullptr);

    ASSERT_EQ(s_sink->Count(), 1u);
    EXPECT_EQ(s_sink->Records().front().text, "at");
}

TEST(StructuredSink, AsyncDispatchesOnWorker) {
    ulog::AsyncLogger::Config cfg;
    cfg.format = ulog::Format::kTskv;
    auto logger = std::make_shared<ulog::AsyncLogger>(cfg);
    logger->SetLevel(ulog::Level::kTrace);
    auto s_sink = std::make_shared<CaptureStructuredSink>();
    logger->AddStructuredSink(s_sink);

    ulog::SetDefaultLogger(logger);
    for (int i = 0; i < 5; ++i) LOG_INFO() << "async-" << i;
    logger->Flush();
    ulog::SetDefaultLogger(nullptr);

    const auto recs = s_sink->Records();
    ASSERT_EQ(recs.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(recs[static_cast<std::size_t>(i)].text,
                  std::string("async-") + std::to_string(i));
    }
}

TEST(StructuredSink, AsyncMixedTextAndStructuredOneEnqueue) {
    // AsyncLogger must pack both halves into ONE queue entry — otherwise
    // ordering and accounting break. This test emits N records and
    // asserts both sinks see exactly N of them.
    ulog::AsyncLogger::Config cfg;
    cfg.format = ulog::Format::kTskv;
    auto logger = std::make_shared<ulog::AsyncLogger>(cfg);
    logger->SetLevel(ulog::Level::kTrace);

    auto text = std::make_shared<CaptureTextSink>();
    auto structured = std::make_shared<CaptureStructuredSink>();
    logger->AddSink(text);
    logger->AddStructuredSink(structured);

    ulog::SetDefaultLogger(logger);
    for (int i = 0; i < 10; ++i) LOG_INFO() << "both-" << i;
    logger->Flush();
    ulog::SetDefaultLogger(nullptr);

    EXPECT_EQ(text->Records().size(), 10u);
    EXPECT_EQ(structured->Count(), 10u);
    EXPECT_EQ(logger->GetTotalLogged(), 10u) << "queue should count one entry per LOG_*";
}

TEST(StructuredSink, EmitLocationFalseSuppressesModuleFields) {
    // Logger configured with emit_location=false must propagate the
    // suppression to the structured record — `location` stays
    // default-constructed (!has_value()). Mirrors the text formatter's
    // behaviour so callers can rely on a single config knob.
    auto logger = std::make_shared<ulog::SyncLogger>(
        ulog::Format::kTskv, /*emit_location=*/false);
    logger->SetLevel(ulog::Level::kTrace);
    auto s_sink = std::make_shared<CaptureStructuredSink>();
    logger->AddStructuredSink(s_sink);

    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "x";
    ulog::SetDefaultLogger(nullptr);

    const auto recs = s_sink->Records();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_FALSE(recs.front().location.has_value());
    EXPECT_TRUE(recs.front().location.function_name().empty());
    EXPECT_TRUE(recs.front().location.file_name().empty());
    EXPECT_EQ(recs.front().location.line(), 0u);
    EXPECT_EQ(recs.front().text, "x");
}

TEST(StructuredSink, EmitLocationTrueKeepsModuleFields) {
    auto logger = std::make_shared<ulog::SyncLogger>(
        ulog::Format::kTskv, /*emit_location=*/true);
    logger->SetLevel(ulog::Level::kTrace);
    auto s_sink = std::make_shared<CaptureStructuredSink>();
    logger->AddStructuredSink(s_sink);

    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "x";
    ulog::SetDefaultLogger(nullptr);

    const auto recs = s_sink->Records();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(recs.front().location.has_value());
    EXPECT_FALSE(recs.front().location.file_name().empty());
    EXPECT_GT(recs.front().location.line(), 0u);
}

TEST(StructuredSink, MultipleStructuredSinksAllReceive) {
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);
    auto a = std::make_shared<CaptureStructuredSink>();
    auto b = std::make_shared<CaptureStructuredSink>();
    auto c = std::make_shared<CaptureStructuredSink>();
    logger->AddStructuredSink(a);
    logger->AddStructuredSink(b);
    logger->AddStructuredSink(c);

    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "fanout";
    ulog::SetDefaultLogger(nullptr);

    EXPECT_EQ(a->Count(), 1u);
    EXPECT_EQ(b->Count(), 1u);
    EXPECT_EQ(c->Count(), 1u);
    EXPECT_EQ(a->Records().front().text, "fanout");
    EXPECT_EQ(b->Records().front().text, "fanout");
    EXPECT_EQ(c->Records().front().text, "fanout");
}

TEST(StructuredSink, JsonStringTagPreserved) {
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);
    auto s_sink = std::make_shared<CaptureStructuredSink>();
    logger->AddStructuredSink(s_sink);

    ulog::SetDefaultLogger(logger);
    {
        ulog::LogExtra extra;
        extra.Extend("meta", ulog::JsonString{"{\"k\":1}"});
        LOG_INFO() << extra << "x";
    }
    ulog::SetDefaultLogger(nullptr);

    const auto recs = s_sink->Records();
    ASSERT_EQ(recs.size(), 1u);
    const auto* meta = FindTag(recs.front(), "meta");
    ASSERT_NE(meta, nullptr);
    ASSERT_TRUE(std::holds_alternative<ulog::JsonString>(meta->value));
    EXPECT_EQ(std::get<ulog::JsonString>(meta->value).View(), "{\"k\":1}");
}

TEST(StructuredSink, ThrowingSinkDoesNotAffectSiblings) {
    class Thrower final : public ulog::sinks::StructuredSink {
    public:
        void Write(const ulog::sinks::LogRecord&) override {
            throw std::runtime_error("boom");
        }
    };
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);
    auto bad = std::make_shared<Thrower>();
    auto good = std::make_shared<CaptureStructuredSink>();
    logger->AddStructuredSink(bad);
    logger->AddStructuredSink(good);

    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "survives";
    ulog::SetDefaultLogger(nullptr);

    EXPECT_EQ(good->Count(), 1u);
    EXPECT_EQ(good->Records().front().text, "survives");
}

TEST(StructuredSink, AsyncFlushWaitsForStructuredRecords) {
    // The producer enqueues records and returns immediately; only after
    // Flush() does the consumer guarantee all records are drained into
    // the structured sink.
    ulog::AsyncLogger::Config cfg;
    cfg.format = ulog::Format::kTskv;
    cfg.queue_capacity = 4096;
    auto logger = std::make_shared<ulog::AsyncLogger>(cfg);
    logger->SetLevel(ulog::Level::kTrace);
    auto s_sink = std::make_shared<CaptureStructuredSink>();
    logger->AddStructuredSink(s_sink);

    ulog::SetDefaultLogger(logger);
    for (int i = 0; i < 100; ++i) LOG_INFO() << "r-" << i;
    logger->Flush();
    ulog::SetDefaultLogger(nullptr);

    EXPECT_EQ(s_sink->Count(), 100u);
}

TEST(StructuredSink, AsyncFlushDrainsStructuredSinkFlushCount) {
    // Flush should invoke Flush() on every structured sink after draining.
    struct CountingSink : ulog::sinks::StructuredSink {
        std::atomic<int> flush_calls{0};
        void Write(const ulog::sinks::LogRecord&) override {}
        void Flush() override { ++flush_calls; }
    };
    ulog::AsyncLogger::Config cfg;
    cfg.format = ulog::Format::kTskv;
    auto logger = std::make_shared<ulog::AsyncLogger>(cfg);
    logger->SetLevel(ulog::Level::kTrace);
    auto counter = std::make_shared<CountingSink>();
    logger->AddStructuredSink(counter);

    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "x";
    logger->Flush();
    ulog::SetDefaultLogger(nullptr);

    EXPECT_GE(counter->flush_calls.load(), 1);
}

TEST(StructuredSink, TraceContextReachesRecord) {
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);
    auto s_sink = std::make_shared<CaptureStructuredSink>();
    logger->AddStructuredSink(s_sink);

    ulog::SetTracingHook([](ulog::TagSink& sink, void*) {
        sink.SetTraceContext("deadbeef", "cafef00d");
    });

    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "traced";
    ulog::SetDefaultLogger(nullptr);
    ulog::SetTracingHook(nullptr);

    const auto recs = s_sink->Records();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs.front().trace_id, "deadbeef");
    EXPECT_EQ(recs.front().span_id,  "cafef00d");
}
