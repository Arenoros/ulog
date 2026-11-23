#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>
#include <string_view>
#include <ulog/format.hpp>

namespace {

constexpr std::array kFormats{ulog::Format::kTskv, ulog::Format::kLtsv, ulog::Format::kRaw,
                              ulog::Format::kJson, ulog::Format::kJsonYaDeploy};
constexpr std::array<std::string_view, 5> kNames{"tskv", "ltsv", "raw", "json", "json_yadeploy"};

static_assert(ulog::ParseFormat("json_yadeploy").format == ulog::Format::kJsonYaDeploy);
static_assert(ulog::ToString(ulog::Format::kLtsv) == "ltsv");

TEST(Format, EveryFormatRoundTripsThroughItsCanonicalName) {
  for (std::size_t index = 0; index < kFormats.size(); ++index) {
    EXPECT_EQ(ulog::ToString(kFormats[index]), kNames[index]);
    const auto parsed = ulog::ParseFormat(kNames[index]);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.format, kFormats[index]);
    EXPECT_TRUE(parsed.HowToFix().empty());
  }
}

TEST(Format, ParsingIsExactAndNamesTheAcceptedSet) {
  for (const std::string_view name :
       {"", "TSKV", "Tskv", " tskv", "tskv ", "json-yadeploy", "jsonyadeploy", "text"}) {
    const auto parsed = ulog::ParseFormat(name);
    EXPECT_FALSE(parsed) << name;
    EXPECT_EQ(parsed.HowToFix(),
              "Use one of the format names tskv, ltsv, raw, json, or json_yadeploy, in "
              "lowercase.");
  }
  EXPECT_FALSE(ulog::ParseFormat(std::string_view{"tskv\0", 5}));
}

TEST(Format, ValuesOutsideTheEnumerationHaveNoName) {
  EXPECT_TRUE(ulog::ToString(std::bit_cast<ulog::Format>(std::uint8_t{5})).empty());
  EXPECT_TRUE(ulog::ToString(std::bit_cast<ulog::Format>(std::uint8_t{255})).empty());
}

}  // namespace
