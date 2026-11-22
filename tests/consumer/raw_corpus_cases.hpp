#pragma once

// The field-rich Raw cases of docs/migration/corpus/text-v1.json, written through the public
// native API. Expected frames are byte-for-byte copies of each case's `observed.value`, except
// that raw-replacement-frozen-duplicate keeps one `duplicate` field, as recorded under DEF-004.

#include <array>
#include <limits>
#include <string_view>
#include <ulog/fields.hpp>
#include <ulog/log.hpp>
#include <ulog/logger.hpp>

namespace raw_corpus {

using namespace std::string_view_literals;

inline constexpr std::string_view kOrderedScalars =
    "tskv\tstring=alpha\tsigned=-42\tunsigned=18446744073709551615\tfloat=1.5\tdouble=-2.25"
    "\tbool_true=1\tbool_false=0\tjson={\"x\":1}\ttext=scalars\n"sv;

inline constexpr std::string_view kReplacementFrozenDuplicate =
    "tskv\tfirst=one\treplace=new\tlast=three\tfrozen=keep\tduplicate=two\ttext=collisions\n"sv;

inline constexpr std::string_view kUnicodeControls =
    "tskv\ta\\=.\\t\\r\\n\\0\\\\=\xC3\xA9=ok:still|\\t\\r\\n\\0\\\\\ttext="
    "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\xF0\x9F\x8C\x8D|"
    "\\r\\n\\0\\t\\\\|literal:\\\\r\\\\n\\\\0\\\\t\\\\\\\\\n"sv;

/// Frames in the order LogAll writes them.
inline constexpr std::array<std::string_view, 3> kFrames{
    kOrderedScalars, kReplacementFrozenDuplicate, kUnicodeControls};

inline void LogOrderedScalars(ulog::Logger logger) {
  LOG_INFO_TO(logger,
              ulog::Fields{{"string", "alpha"},
                           {"signed", -42LL},
                           {"unsigned", std::numeric_limits<unsigned long long>::max()},
                           {"float", 1.5F},
                           {"double", -2.25},
                           {"bool_true", true},
                           {"bool_false", false},
                           {"json", ulog::JsonString{"{\n\"x\":1\r}"}}},
              "scalars");
}

inline void LogReplacementFrozenDuplicate(ulog::Logger logger) {
  ulog::FieldSet extra;
  extra.Add("first", "one");
  extra.Add("replace", "old");
  extra.Add("last", "three");
  extra.Add("replace", "new");
  extra.Add("frozen", "keep", ulog::FieldMode::kFrozen);
  extra.Add("frozen", "ignored");
  LOG_INFO_TO(logger, ulog::Fields{extra, {"duplicate", "one"}, {"duplicate", "two"}},
              "collisions");
}

inline void LogUnicodeControls(ulog::Logger logger) {
  constexpr std::string_view kKey = "A=.\t\r\n\0\\"sv;
  constexpr std::string_view kValue = "\xC3\xA9=ok:still|\t\r\n\0\\"sv;
  constexpr std::string_view kMessage =
      "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\xF0\x9F\x8C\x8D|\r\n\0\t\\|literal:\\r\\n\\0\\t\\\\"sv;
  LOG_INFO_TO(logger, ulog::Fields{{kKey, kValue}}, "{}", kMessage);
}

inline void LogAll(ulog::Logger logger) {
  LogOrderedScalars(logger);
  LogReplacementFrozenDuplicate(logger);
  LogUnicodeControls(logger);
}

}  // namespace raw_corpus
