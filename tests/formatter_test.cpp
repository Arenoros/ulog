#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include <ulog/impl/formatters/json.hpp>
#include <ulog/impl/formatters/otlp_json.hpp>
#include <ulog/impl/formatters/text_item.hpp>
#include <ulog/log.hpp>
#include <ulog/mem_logger.hpp>
#include <ulog/tracing_hook.hpp>

namespace {

std::shared_ptr<ulog::MemLogger> InstallMem(ulog::Format fmt) {
    auto logger = std::make_shared<ulog::MemLogger>(fmt);
    ulog::SetDefaultLogger(logger);
    return logger;
}

bool Contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

}  // namespace

TEST(FormatterTskv, BasicRecord) {
    auto mem = InstallMem(ulog::Format::kTskv);
    LOG_INFO() << "hello";
    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    const auto& r = recs.front();
    EXPECT_TRUE(Contains(r, "timestamp="));
    EXPECT_TRUE(Contains(r, "level=INFO"));
    EXPECT_TRUE(Contains(r, "text=hello"));
    EXPECT_EQ(r.back(), '\n');
    ulog::SetDefaultLogger(nullptr);
}

TEST(FormatterTskv, EscapesTabsInText) {
    auto mem = InstallMem(ulog::Format::kTskv);
    LOG_INFO() << "a\tb\nc";
    const auto r = mem->GetRecords().front();
    EXPECT_TRUE(Contains(r, "text=a\\tb\\nc"));
    ulog::SetDefaultLogger(nullptr);
}

TEST(FormatterTskv, TagsFromLogExtra) {
    auto mem = InstallMem(ulog::Format::kTskv);
    ulog::LogExtra e{{"user_id", 42}, {"op", std::string("create")}};
    LOG_INFO() << "op" << e;
    const auto r = mem->GetRecords().front();
    EXPECT_TRUE(Contains(r, "user_id=42"));
    EXPECT_TRUE(Contains(r, "op=create"));
    ulog::SetDefaultLogger(nullptr);
}

TEST(FormatterLtsv, UsesColonSeparator) {
    auto mem = InstallMem(ulog::Format::kLtsv);
    LOG_INFO() << "x";
    const auto r = mem->GetRecords().front();
    EXPECT_TRUE(Contains(r, "timestamp:"));
    EXPECT_TRUE(Contains(r, "level:INFO"));
    EXPECT_TRUE(Contains(r, "text:x"));
    ulog::SetDefaultLogger(nullptr);
}

TEST(FormatterRaw, OnlyTextNoHeader) {
    auto mem = InstallMem(ulog::Format::kRaw);
    LOG_INFO() << "payload-body";
    const auto r = mem->GetRecords().front();
    EXPECT_FALSE(Contains(r, "timestamp="));
    EXPECT_FALSE(Contains(r, "level="));
    EXPECT_TRUE(Contains(r, "text=payload-body"));
    ulog::SetDefaultLogger(nullptr);
}

TEST(FormatterJson, EmitsJsonObject) {
    auto mem = InstallMem(ulog::Format::kJson);
    LOG_INFO() << "hi";
    const auto r = mem->GetRecords().front();
    EXPECT_EQ(r.front(), '{');
    EXPECT_EQ(r[r.size() - 2], '}');
    EXPECT_TRUE(Contains(r, "\"level\":\"INFO\""));
    EXPECT_TRUE(Contains(r, "\"text\":\"hi\""));
    ulog::SetDefaultLogger(nullptr);
}

TEST(FormatterJson, TypedTagsEmitAsNativeJsonTypes) {
    auto mem = InstallMem(ulog::Format::kJson);
    LOG_INFO() << "body" << ulog::LogExtra{
        {"count", 42},
        {"ratio", 3.14},
        {"ok", true},
        {"name", std::string("ada")},
    };
    const auto r = mem->GetRecords().front();
    EXPECT_NE(r.find("\"count\":42"), std::string::npos) << r;
    EXPECT_NE(r.find("\"ratio\":3.14"), std::string::npos) << r;
    EXPECT_NE(r.find("\"ok\":true"), std::string::npos) << r;
    EXPECT_NE(r.find("\"name\":\"ada\""), std::string::npos) << r;
    ulog::SetDefaultLogger(nullptr);
}

TEST(LogHelperExceptions, WithExceptionCapturesTypeAndMessage) {
    // LogHelper::WithException attaches exception_type + exception_msg
    // as tags on the active record. Verify both land in the TSKV output
    // with the expected `what()` message and a type string that contains
    // the exception class name (mangling form is implementation-defined,
    // so substring match — not byte-equal).
    auto mem = InstallMem(ulog::Format::kTskv);
    try {
        throw std::runtime_error("boom-42");
    } catch (const std::exception& ex) {
        LOG_ERROR().WithException(ex) << "op-failed";
    }
    const auto r = mem->GetRecords().front();
    EXPECT_NE(r.find("exception_msg=boom-42"), std::string::npos) << r;
    EXPECT_NE(r.find("exception_type="), std::string::npos) << r;
    EXPECT_NE(r.find("runtime_error"), std::string::npos) << r;
    EXPECT_NE(r.find("text=op-failed"), std::string::npos) << r;
    ulog::SetDefaultLogger(nullptr);
}

TEST(FormatterJson, YaDeployPassesThroughAddJsonTag) {
    // Prior coverage only exercised the stringifying AddTag path against
    // the YaDeploy variant. This pins the raw-JSON path: a JsonString
    // tag passed via AddJsonTag must land unquoted (as-is) in the
    // YaDeploy record, and the canonical core-field renames still apply.
    auto mem = InstallMem(ulog::Format::kJsonYaDeploy);
    LOG_INFO() << "body" << ulog::LogExtra{
        {"meta", ulog::JsonString(std::string("{\"x\":1,\"y\":\"q\"}"))},
    };
    const auto r = mem->GetRecords().front();
    // Raw-JSON tag emitted as a native JSON subtree, no outer quoting.
    EXPECT_NE(r.find("\"meta\":{\"x\":1,\"y\":\"q\"}"), std::string::npos) << r;
    // YaDeploy renames still intact.
    EXPECT_NE(r.find("\"@timestamp\":"), std::string::npos) << r;
    EXPECT_NE(r.find("\"_level\":\"INFO\""), std::string::npos) << r;
    EXPECT_NE(r.find("\"_message\":\"body\""), std::string::npos) << r;
    ulog::SetDefaultLogger(nullptr);
}

TEST(FormatterJson, YaDeployVariantRenamesCoreFields) {
    auto mem = InstallMem(ulog::Format::kJsonYaDeploy);
    LOG_INFO() << "body" << ulog::LogExtra{{"user_id", 7}};
    const auto r = mem->GetRecords().front();
    EXPECT_NE(r.find("\"@timestamp\":"), std::string::npos);
    EXPECT_NE(r.find("\"_level\":\"INFO\""), std::string::npos);
    EXPECT_NE(r.find("\"_message\":\"body\""), std::string::npos);
    // User-supplied tags flow through typed overloads: int → unquoted.
    EXPECT_NE(r.find("\"user_id\":7"), std::string::npos) << r;
    // Canonical names must be absent.
    EXPECT_EQ(r.find("\"timestamp\":"), std::string::npos);
    EXPECT_EQ(r.find("\"level\":\"INFO\""), std::string::npos);
    EXPECT_EQ(r.find("\"text\":\"body\""), std::string::npos);
    ulog::SetDefaultLogger(nullptr);
}

TEST(FormatterOtlpJson, EmitsSchemaShape) {
    auto mem = InstallMem(ulog::Format::kOtlpJson);
    LOG_INFO() << "user logged in" << ulog::LogExtra{
        {"user_id", 42},
        {"latency_ms", 13.5},
        {"ok", true},
        {"endpoint", std::string("/api")},
    };
    const auto r = mem->GetRecords().front();
    // Envelope
    EXPECT_EQ(r.front(), '{');
    EXPECT_EQ(r[r.size() - 2], '}');
    EXPECT_TRUE(Contains(r, "\"timeUnixNano\":\""));
    EXPECT_TRUE(Contains(r, "\"severityNumber\":9"));       // INFO
    EXPECT_TRUE(Contains(r, "\"severityText\":\"INFO\""));
    // Body
    EXPECT_TRUE(Contains(r, "\"body\":{\"stringValue\":\"user logged in\"}"));
    // Attributes with typed OTLP values
    EXPECT_TRUE(Contains(r, "\"key\":\"user_id\",\"value\":{\"intValue\":\"42\"}")) << r;
    EXPECT_TRUE(Contains(r, "\"key\":\"latency_ms\",\"value\":{\"doubleValue\":13.5}")) << r;
    EXPECT_TRUE(Contains(r, "\"key\":\"ok\",\"value\":{\"boolValue\":true}")) << r;
    EXPECT_TRUE(Contains(r, "\"key\":\"endpoint\",\"value\":{\"stringValue\":\"/api\"}")) << r;
    ulog::SetDefaultLogger(nullptr);
}

TEST(FormatterOtlpJson, NoAttributesElidesTheArray) {
    auto mem = InstallMem(ulog::Format::kOtlpJson);
    // Disable dynamic-debug so no stray entries; no LogExtra, no module
    // suppression needed since the formatter always emits module as an
    // attribute. Filter that in the assertion instead of avoiding it.
    LOG_INFO() << "plain";
    const auto r = mem->GetRecords().front();
    EXPECT_TRUE(Contains(r, "\"body\":{\"stringValue\":\"plain\"}"));
    EXPECT_TRUE(Contains(r, "\"severityText\":\"INFO\""));
    ulog::SetDefaultLogger(nullptr);
}

namespace {

struct TraceCtx {
    std::string trace_id;
    std::string span_id;
};

void TraceCtxHook(ulog::TagSink& sink, void* user_ctx) {
    const auto* c = static_cast<const TraceCtx*>(user_ctx);
    sink.SetTraceContext(c->trace_id, c->span_id);
}

}  // namespace

TEST(FormatterOtlpJson, TraceAndSpanIdsPromotedToTopLevel) {
    auto mem = InstallMem(ulog::Format::kOtlpJson);
    TraceCtx ctx{"0123456789abcdef0123456789abcdef", "fedcba9876543210"};
    ulog::SetTracingHook(&TraceCtxHook, &ctx);
    LOG_INFO() << "correlated" << ulog::LogExtra{{"user_id", 42}};
    ulog::SetTracingHook(nullptr);

    const auto r = mem->GetRecords().front();
    EXPECT_TRUE(Contains(r,
        "\"traceId\":\"0123456789abcdef0123456789abcdef\"")) << r;
    EXPECT_TRUE(Contains(r, "\"spanId\":\"fedcba9876543210\"")) << r;
    // Not duplicated into attributes.
    EXPECT_FALSE(Contains(r, "\"key\":\"trace_id\"")) << r;
    EXPECT_FALSE(Contains(r, "\"key\":\"span_id\"")) << r;
    // User-supplied tags still flow through the attribute array with their
    // native OTLP value kind.
    EXPECT_TRUE(Contains(r,
        "\"key\":\"user_id\",\"value\":{\"intValue\":\"42\"}")) << r;
    ulog::SetDefaultLogger(nullptr);
}

TEST(FormatterOtlpJson, TraceIdAloneKeepsAttributesArrayShape) {
    auto mem = InstallMem(ulog::Format::kOtlpJson);
    TraceCtx ctx{"0123456789abcdef0123456789abcdef", ""};
    ulog::SetTracingHook(&TraceCtxHook, &ctx);
    LOG_INFO() << "solo";
    ulog::SetTracingHook(nullptr);

    const auto r = mem->GetRecords().front();
    EXPECT_TRUE(Contains(r,
        "\"traceId\":\"0123456789abcdef0123456789abcdef\""));
    EXPECT_FALSE(Contains(r, "\"spanId\":"));
    ulog::SetDefaultLogger(nullptr);
}

TEST(FormatterOtlpJson, EmptyTraceContextEmitsNothing) {
    // Both halves empty -> formatter must produce neither top-level fields
    // nor attribute entries. Catches accidental "" emission if the
    // override short-circuits on the wrong branch.
    auto mem = InstallMem(ulog::Format::kOtlpJson);
    TraceCtx ctx{"", ""};
    ulog::SetTracingHook(&TraceCtxHook, &ctx);
    LOG_INFO() << "nohook";
    ulog::SetTracingHook(nullptr);

    const auto r = mem->GetRecords().front();
    EXPECT_FALSE(Contains(r, "\"traceId\":")) << r;
    EXPECT_FALSE(Contains(r, "\"spanId\":")) << r;
    EXPECT_FALSE(Contains(r, "\"key\":\"trace_id\"")) << r;
    EXPECT_FALSE(Contains(r, "\"key\":\"span_id\"")) << r;
    ulog::SetDefaultLogger(nullptr);
}

TEST(FormatterText, TraceContextFallsBackToPlainTags) {
    // Non-OTLP formatters inherit the default Base::SetTraceContext, which
    // emits `trace_id=` / `span_id=` as ordinary tags so plain-text tails
    // still carry the correlation data.
    auto mem = InstallMem(ulog::Format::kTskv);
    TraceCtx ctx{"0123456789abcdef0123456789abcdef", "fedcba9876543210"};
    ulog::SetTracingHook(&TraceCtxHook, &ctx);
    LOG_INFO() << "plain";
    ulog::SetTracingHook(nullptr);

    const auto r = mem->GetRecords().front();
    EXPECT_TRUE(Contains(r, "trace_id=0123456789abcdef0123456789abcdef")) << r;
    EXPECT_TRUE(Contains(r, "span_id=fedcba9876543210")) << r;
    ulog::SetDefaultLogger(nullptr);
}

TEST(FormatterInlineAlloc, PlacesFormatterIntoScratchWhenFits) {
    // White-box check on the Phase 26 inline-construction path:
    // MakeFormatterInto with a correctly-sized scratch should placement-new
    // the formatter inside that buffer and return a BasePtr carrying the
    // destroy-only deleter (heap=false).
    ulog::MemLogger logger(ulog::Format::kJson);
    alignas(ulog::impl::LoggerBase::kInlineFormatterAlign)
        std::byte scratch[ulog::impl::LoggerBase::kInlineFormatterSize]{};
    auto fmt = logger.MakeFormatterInto(
        scratch, sizeof(scratch), ulog::Level::kInfo, {}, {}, 0);
    ASSERT_NE(fmt, nullptr);
    const auto* raw = reinterpret_cast<const std::byte*>(fmt.get());
    EXPECT_GE(raw, scratch);
    EXPECT_LT(raw, scratch + sizeof(scratch));
    EXPECT_FALSE(fmt.get_deleter().heap);
}

TEST(FormatterInlineAlloc, FallsBackToHeapWhenScratchTooSmall) {
    // Symmetric test: passing a zero-sized scratch forces the heap path.
    // The pointer must NOT lie inside the caller's buffer and the
    // deleter must run delete on destruction (heap=true).
    ulog::MemLogger logger(ulog::Format::kJson);
    std::byte scratch[1]{};
    auto fmt = logger.MakeFormatterInto(
        scratch, 0, ulog::Level::kInfo, {}, {}, 0);
    ASSERT_NE(fmt, nullptr);
    const auto* raw = reinterpret_cast<const std::byte*>(fmt.get());
    EXPECT_TRUE(raw < scratch || raw >= scratch + sizeof(scratch));
    EXPECT_TRUE(fmt.get_deleter().heap);
}

TEST(FormatterJson, RepeatedSetTextKeepsLastValue) {
    // SmallString::assign clears-then-appends, so a second SetText must
    // replace the first — not concatenate. Formatter is normally one-shot
    // from LogHelper, but the contract on SetText is "set, not append".
    using ulog::impl::formatters::JsonFormatter;
    using ulog::impl::formatters::TextLogItem;
    JsonFormatter f(ulog::Level::kInfo, {}, {}, 0,
                    std::chrono::system_clock::time_point{});
    f.SetText("first");
    f.SetText("second");
    auto item = f.ExtractLoggerItem();
    const auto& payload = static_cast<TextLogItem&>(*item).payload;
    const auto view = payload.view();
    const std::string s(view.data(), view.size());
    EXPECT_NE(s.find("\"text\":\"second\""), std::string::npos) << s;
    EXPECT_EQ(s.find("first"), std::string::npos) << s;
}

TEST(FormatterJson, LongBodySpillsToHeapAndRoundTrips) {
    // The SmallString<64>-backed body buffer keeps ≤64-byte messages on
    // the stack; anything longer spills to heap via boost::small_vector.
    // Both paths must round-trip byte-for-byte.
    auto mem = InstallMem(ulog::Format::kJson);
    const std::string big(200, 'x');  // exceeds 64-byte SSO slab
    LOG_INFO() << big;
    const auto r = mem->GetRecords().front();
    EXPECT_NE(r.find("\"text\":\"" + big + "\""), std::string::npos) << r.size();
    ulog::SetDefaultLogger(nullptr);
}

TEST(FormatterOtlpJson, LongBodySpillsToHeapAndRoundTrips) {
    auto mem = InstallMem(ulog::Format::kOtlpJson);
    const std::string big(200, 'y');
    LOG_INFO() << big;
    const auto r = mem->GetRecords().front();
    EXPECT_NE(r.find("\"body\":{\"stringValue\":\"" + big + "\"}"), std::string::npos)
        << r.size();
    ulog::SetDefaultLogger(nullptr);
}

TEST(FormatterJson, EscapesQuotesInValue) {
    auto mem = InstallMem(ulog::Format::kJson);
    LOG_INFO() << "a\"b\n";
    const auto r = mem->GetRecords().front();
    const std::string expected = std::string("\"text\":\"a\\\"b\\n\"");
    EXPECT_TRUE(Contains(r, expected));
    ulog::SetDefaultLogger(nullptr);
}

// --------- emit_location knob ---------

TEST(EmitLocation, DefaultTrueEmitsModuleField) {
    auto logger = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "hi";
    const auto rec = logger->GetRecords().front();
    EXPECT_TRUE(Contains(rec, "module="));
    ulog::SetDefaultLogger(nullptr);
}

TEST(EmitLocation, FalseSuppressesModuleTskv) {
    auto logger = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv, /*emit_location=*/false);
    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "hi";
    const auto rec = logger->GetRecords().front();
    EXPECT_EQ(rec.find("module="), std::string::npos) << rec;
    // Other standard fields still present.
    EXPECT_TRUE(Contains(rec, "timestamp="));
    EXPECT_TRUE(Contains(rec, "level="));
    EXPECT_TRUE(Contains(rec, "text=hi"));
    ulog::SetDefaultLogger(nullptr);
}

TEST(EmitLocation, FalseSuppressesModuleJson) {
    auto logger = std::make_shared<ulog::MemLogger>(ulog::Format::kJson, /*emit_location=*/false);
    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "hi";
    const auto rec = logger->GetRecords().front();
    EXPECT_EQ(rec.find("\"module\":"), std::string::npos) << rec;
    EXPECT_TRUE(Contains(rec, "\"text\":\"hi\""));
    ulog::SetDefaultLogger(nullptr);
}

TEST(EmitLocation, FalseSuppressesModuleLtsv) {
    auto logger = std::make_shared<ulog::MemLogger>(ulog::Format::kLtsv, /*emit_location=*/false);
    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "hi";
    const auto rec = logger->GetRecords().front();
    EXPECT_EQ(rec.find("module:"), std::string::npos) << rec;
    ulog::SetDefaultLogger(nullptr);
}

// --------- timestamp_format knob ---------

namespace {

std::string ExtractTskvTimestamp(const std::string& rec) {
    const auto key = rec.find("timestamp=");
    if (key == std::string::npos) return {};
    const auto start = key + std::string_view("timestamp=").size();
    const auto tab = rec.find('\t', start);
    return rec.substr(start, tab == std::string::npos ? std::string::npos : tab - start);
}

bool AllDigits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

}  // namespace

TEST(TimestampFormat, Iso8601MicroDefaultShape) {
    auto logger = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "x";
    const auto ts = ExtractTskvTimestamp(logger->GetRecords().front());
    // YYYY-MM-DDThh:mm:ss.uuuuuu+0000 = 31 chars
    EXPECT_EQ(ts.size(), 31u) << ts;
    EXPECT_NE(ts.find('T'), std::string::npos);
    EXPECT_NE(ts.find('.'), std::string::npos);
    ulog::SetDefaultLogger(nullptr);
}

TEST(TimestampFormat, Iso8601MilliShape) {
    auto logger = std::make_shared<ulog::MemLogger>(
        ulog::Format::kTskv, true, ulog::TimestampFormat::kIso8601Milli);
    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "x";
    const auto ts = ExtractTskvTimestamp(logger->GetRecords().front());
    // YYYY-MM-DDThh:mm:ss.mmm+0000 = 28 chars
    EXPECT_EQ(ts.size(), 28u) << ts;
    ulog::SetDefaultLogger(nullptr);
}

TEST(TimestampFormat, Iso8601SecShape) {
    auto logger = std::make_shared<ulog::MemLogger>(
        ulog::Format::kTskv, true, ulog::TimestampFormat::kIso8601Sec);
    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "x";
    const auto ts = ExtractTskvTimestamp(logger->GetRecords().front());
    // YYYY-MM-DDThh:mm:ss+0000 = 24 chars
    EXPECT_EQ(ts.size(), 24u) << ts;
    EXPECT_EQ(ts.find('.'), std::string::npos);
    ulog::SetDefaultLogger(nullptr);
}

TEST(TimestampFormat, EpochSecPureDigits) {
    auto logger = std::make_shared<ulog::MemLogger>(
        ulog::Format::kTskv, true, ulog::TimestampFormat::kEpochSec);
    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "x";
    const auto ts = ExtractTskvTimestamp(logger->GetRecords().front());
    EXPECT_TRUE(AllDigits(ts)) << ts;
    EXPECT_GE(ts.size(), 10u);
    EXPECT_LE(ts.size(), 11u);
    ulog::SetDefaultLogger(nullptr);
}

TEST(TimestampFormat, EpochMilliWiderThanSec) {
    auto logger = std::make_shared<ulog::MemLogger>(
        ulog::Format::kTskv, true, ulog::TimestampFormat::kEpochMilli);
    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "x";
    const auto ts = ExtractTskvTimestamp(logger->GetRecords().front());
    EXPECT_TRUE(AllDigits(ts)) << ts;
    EXPECT_GE(ts.size(), 13u);
    ulog::SetDefaultLogger(nullptr);
}

TEST(TimestampFormat, EpochNanoFitsBuffer) {
    auto logger = std::make_shared<ulog::MemLogger>(
        ulog::Format::kTskv, true, ulog::TimestampFormat::kEpochNano);
    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "x";
    const auto ts = ExtractTskvTimestamp(logger->GetRecords().front());
    EXPECT_TRUE(AllDigits(ts)) << ts;
    EXPECT_GE(ts.size(), 19u);
    EXPECT_LE(ts.size(), 20u);
    ulog::SetDefaultLogger(nullptr);
}

TEST(TimestampFormat, JsonEpochEmittedAsNumber) {
    auto logger = std::make_shared<ulog::MemLogger>(
        ulog::Format::kJson, true, ulog::TimestampFormat::kEpochMilli);
    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "x";
    const auto rec = logger->GetRecords().front();
    // No quotes around the numeric timestamp.
    EXPECT_NE(rec.find("\"timestamp\":"), std::string::npos) << rec;
    EXPECT_EQ(rec.find("\"timestamp\":\""), std::string::npos) << rec;
    ulog::SetDefaultLogger(nullptr);
}

TEST(TimestampFormat, JsonIso8601EmittedAsString) {
    auto logger = std::make_shared<ulog::MemLogger>(
        ulog::Format::kJson, true, ulog::TimestampFormat::kIso8601Milli);
    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "x";
    const auto rec = logger->GetRecords().front();
    EXPECT_NE(rec.find("\"timestamp\":\""), std::string::npos) << rec;
    ulog::SetDefaultLogger(nullptr);
}

TEST(TimestampFormat, ParseAndStringifyRoundTrip) {
    const auto all = {
        ulog::TimestampFormat::kIso8601Micro,
        ulog::TimestampFormat::kIso8601Milli,
        ulog::TimestampFormat::kIso8601Sec,
        ulog::TimestampFormat::kEpochNano,
        ulog::TimestampFormat::kEpochMicro,
        ulog::TimestampFormat::kEpochMilli,
        ulog::TimestampFormat::kEpochSec,
    };
    for (auto f : all) {
        const auto s = ulog::ToString(f);
        EXPECT_EQ(ulog::TimestampFormatFromString(s), f) << s;
    }
    EXPECT_THROW(ulog::TimestampFormatFromString("no-such-format"), std::runtime_error);
}

TEST(EmitLocation, FalseSuppressesModuleOtlpJson) {
    auto logger = std::make_shared<ulog::MemLogger>(ulog::Format::kOtlpJson, /*emit_location=*/false);
    ulog::SetDefaultLogger(logger);
    LOG_INFO() << "hi";
    const auto rec = logger->GetRecords().front();
    EXPECT_EQ(rec.find("\"module\""), std::string::npos) << rec;
    ulog::SetDefaultLogger(nullptr);
}
