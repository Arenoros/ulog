#include <array>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include <ulog/log.hpp>
#include <ulog/mem_logger.hpp>

// Phase 48 — PutRange: `operator<<(LogHelper&, Range)` for containers.
// Sequence → `[a, b, c]`. Map → `{"k1": v1, "k2": v2}`. String-like
// elements wrapped in `Quoted{}`. Honours IsLimitReached via trailing
// `, ...N more`. Char-element ranges are a compile error.

namespace {

bool Contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

}  // namespace

// ---- sequence containers ---------------------------------------------------

TEST(PutRange, VectorOfInt) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    const std::vector<int> v{1, 2, 3};
    ulog::LogHelper(*mem, ulog::Level::kInfo) << "v=" << v;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "v=[1, 2, 3]")) << recs[0];
}

TEST(PutRange, EmptyVector) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    const std::vector<int> v;
    ulog::LogHelper(*mem, ulog::Level::kInfo) << "v=" << v;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "v=[]")) << recs[0];
}

TEST(PutRange, VectorOfString) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    const std::vector<std::string> v{"alpha", "beta"};
    ulog::LogHelper(*mem, ulog::Level::kInfo) << "v=" << v;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "v=[\"alpha\", \"beta\"]")) << recs[0];
}

TEST(PutRange, StdArray) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    const std::array<int, 4> a{10, 20, 30, 40};
    ulog::LogHelper(*mem, ulog::Level::kInfo) << "a=" << a;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "a=[10, 20, 30, 40]")) << recs[0];
}

TEST(PutRange, CStyleArray) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    const int a[3] = {7, 8, 9};
    ulog::LogHelper(*mem, ulog::Level::kInfo) << "a=" << a;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "a=[7, 8, 9]")) << recs[0];
}

TEST(PutRange, SetOrdered) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    const std::set<int> s{3, 1, 2};
    ulog::LogHelper(*mem, ulog::Level::kInfo) << "s=" << s;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    // Ordered traversal by key.
    EXPECT_TRUE(Contains(recs[0], "s=[1, 2, 3]")) << recs[0];
}

TEST(PutRange, Nested) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    const std::vector<std::vector<int>> vv{{1, 2}, {3, 4}};
    ulog::LogHelper(*mem, ulog::Level::kInfo) << "vv=" << vv;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "vv=[[1, 2], [3, 4]]")) << recs[0];
}

// ---- map ---------------------------------------------------------------

TEST(PutRange, MapIntToString) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    const std::map<int, std::string> m{{1, "one"}, {2, "two"}};
    ulog::LogHelper(*mem, ulog::Level::kInfo) << "m=" << m;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    // Ordered traversal; value strings get Quoted; use { / } brackets.
    EXPECT_TRUE(Contains(recs[0], "m={1: \"one\", 2: \"two\"}")) << recs[0];
}

TEST(PutRange, MapStringKeyQuoted) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    const std::map<std::string, int> m{{"a", 1}, {"b", 2}};
    ulog::LogHelper(*mem, ulog::Level::kInfo) << "m=" << m;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "m={\"a\": 1, \"b\": 2}")) << recs[0];
}

TEST(PutRange, UnorderedMapHasKeysAndBraces) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    const std::unordered_map<int, int> m{{1, 10}, {2, 20}};
    ulog::LogHelper(*mem, ulog::Level::kInfo) << "m=" << m;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    // Order is implementation-defined; verify shape + both pairs present.
    EXPECT_TRUE(Contains(recs[0], "m={")) << recs[0];
    EXPECT_TRUE(Contains(recs[0], "1: 10")) << recs[0];
    EXPECT_TRUE(Contains(recs[0], "2: 20")) << recs[0];
    EXPECT_TRUE(Contains(recs[0], "}")) << recs[0];
}

// ---- String is NOT a range ---------------------------------------------

TEST(PutRange, StdStringRendersAsTextNotRange) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    const std::string s = "hello";
    ulog::LogHelper(*mem, ulog::Level::kInfo) << "s=" << s;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "s=hello")) << recs[0];
    EXPECT_FALSE(Contains(recs[0], "s=[")) << recs[0];
}

TEST(PutRange, StringViewRendersAsTextNotRange) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    std::string_view sv = "world";
    ulog::LogHelper(*mem, ulog::Level::kInfo) << "sv=" << sv;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "sv=world")) << recs[0];
    EXPECT_FALSE(Contains(recs[0], "sv=[")) << recs[0];
}

// ---- Limit-reached truncation suffix ------------------------------------

TEST(PutRange, TruncationCarriesTag) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    // 2000 strings × ~6 chars each = ~12 KB — well past the 10 KB cap.
    std::vector<std::string> v;
    v.reserve(2000);
    for (int i = 0; i < 2000; ++i) v.emplace_back("aaaaa");

    ulog::LogHelper(*mem, ulog::Level::kInfo) << "v=" << v;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    // Record carries the `truncated=true` tag — the authoritative
    // signal. The trailing `, ...` is best-effort and may no-op if
    // the cap already saturated, so we do not require it here.
    EXPECT_TRUE(Contains(recs[0], "truncated=true")) << recs[0];
    EXPECT_TRUE(Contains(recs[0], "v=[")) << recs[0];
}

// ---- Empty / single-element edge --------------------------------------

TEST(PutRange, SingleElementMap) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    const std::map<int, int> m{{42, 100}};
    ulog::LogHelper(*mem, ulog::Level::kInfo) << "m=" << m;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "m={42: 100}")) << recs[0];
}
