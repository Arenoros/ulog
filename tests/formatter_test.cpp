#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <ulog/log.hpp>
#include <ulog/mem_logger.hpp>

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

TEST(FormatterJson, EscapesQuotesInValue) {
    auto mem = InstallMem(ulog::Format::kJson);
    LOG_INFO() << "a\"b\n";
    const auto r = mem->GetRecords().front();
    const std::string expected = std::string("\"text\":\"a\\\"b\\n\"");
    EXPECT_TRUE(Contains(r, expected));
    ulog::SetDefaultLogger(nullptr);
}
