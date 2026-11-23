#pragma once

// The field-rich Raw cases of docs/migration/corpus/text-v1.json, written through the public
// native API. Expected frames are byte-for-byte copies of each case's `observed.value`, except
// that raw-replacement-frozen-duplicate keeps one `duplicate` field, as recorded under DEF-004.
// The same calls write the field-rich TSKV and LTSV cases; see text_runtime_main.cpp.

#include <array>
#include <limits>
#include <string_view>
#include <ulog/fields.hpp>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/source_location.hpp>

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

inline void LogOrderedScalars(ulog::Logger logger, const ulog::SourceLocation& source) {
  logger.Log<ulog::Level::kInfo>(
      source,
      [](ulog::FieldWriter& fields) {
        fields.Add(ulog::Fields{{"string", "alpha"},
                                {"signed", -42LL},
                                {"unsigned", std::numeric_limits<unsigned long long>::max()},
                                {"float", 1.5F},
                                {"double", -2.25},
                                {"bool_true", true},
                                {"bool_false", false},
                                {"json", ulog::JsonString{"{\n\"x\":1\r}"}}});
      },
      [] { return "scalars"sv; });
}

inline void LogReplacementFrozenDuplicate(ulog::Logger logger, const ulog::SourceLocation& source) {
  ulog::FieldSet extra;
  extra.Add("first", "one");
  extra.Add("replace", "old");
  extra.Add("last", "three");
  extra.Add("replace", "new");
  extra.Add("frozen", "keep", ulog::FieldMode::kFrozen);
  extra.Add("frozen", "ignored");
  logger.Log<ulog::Level::kInfo>(
      source,
      [&](ulog::FieldWriter& fields) {
        fields.Add(ulog::Fields{extra, {"duplicate", "one"}, {"duplicate", "two"}});
      },
      [] { return "collisions"sv; });
}

inline void LogUnicodeControls(ulog::Logger logger, const ulog::SourceLocation& source) {
  constexpr std::string_view kKey = "A=.\t\r\n\0\\"sv;
  constexpr std::string_view kValue = "\xC3\xA9=ok:still|\t\r\n\0\\"sv;
  constexpr std::string_view kMessage =
      "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\xF0\x9F\x8C\x8D|\r\n\0\t\\|literal:\\r\\n\\0\\t\\\\"sv;
  logger.Log<ulog::Level::kInfo>(
      source, [&](ulog::FieldWriter& fields) { fields.Add(ulog::Fields{{kKey, kValue}}); },
      [&] { return kMessage; });
}

/// A short fixed location keeps Record sizes independent of the checkout path and compiler.
inline constexpr ulog::SourceLocation kSource =
    ulog::SourceLocation::Custom("raw_corpus_cases.hpp", "LogAll", 1);

inline void LogAll(ulog::Logger logger, const ulog::SourceLocation& source = kSource) {
  LogOrderedScalars(logger, source);
  LogReplacementFrozenDuplicate(logger, source);
  LogUnicodeControls(logger, source);
}

}  // namespace raw_corpus
