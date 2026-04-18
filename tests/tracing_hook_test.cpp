#include <atomic>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <ulog/log.hpp>
#include <ulog/mem_logger.hpp>
#include <ulog/tracing_hook.hpp>

namespace {

struct Ctx {
    std::string trace_id = "abc123";
    std::string span_id = "xyz789";
};

void EmitTags(ulog::TagSink& sink, void* user_ctx) {
    auto* c = static_cast<Ctx*>(user_ctx);
    sink.AddTag("trace_id", c->trace_id);
    sink.AddTag("span_id", c->span_id);
}

}  // namespace

TEST(TracingHook, CallbackAddsTags) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);
    ulog::SetDefaultLogger(mem);

    Ctx ctx;
    ulog::SetTracingHook(&EmitTags, &ctx);

    LOG_INFO() << "record";
    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_NE(recs.front().find("trace_id=abc123"), std::string::npos);
    EXPECT_NE(recs.front().find("span_id=xyz789"), std::string::npos);

    ulog::SetTracingHook(nullptr);
    ulog::SetDefaultLogger(nullptr);
}

TEST(TracingHook, NullHookDoesNothing) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    ulog::SetDefaultLogger(mem);
    ulog::SetTracingHook(nullptr);
    LOG_INFO() << "no-hook";
    ASSERT_EQ(mem->GetRecords().size(), 1u);
    EXPECT_EQ(mem->GetRecords().front().find("trace_id="), std::string::npos);
    ulog::SetDefaultLogger(nullptr);
}
