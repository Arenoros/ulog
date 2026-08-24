#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <userver/logging/format.hpp>
#include <userver/logging/impl/logger_base.hpp>
#include <userver/logging/json_string.hpp>
#include <userver/logging/level.hpp>
#include <userver/logging/log_extra.hpp>
#include <userver/logging/log_helper.hpp>
#include <userver/utils/impl/source_location.hpp>
#include <utility>
#include <vector>

#include "baseline_attestation.hpp"

namespace {

namespace baseline = USERVER_NAMESPACE;
using namespace std::string_literals;

constexpr std::uint_least32_t kSourceLine = 321;
constexpr std::string_view kFunctionName = "BaselineTextProbe";

enum class Scenario {
  kEmpty,
  kSimple,
  kUnicodeControls,
  kOrderedScalars,
  kReplacementFrozenDuplicate,
};

struct FormatDescriptor final {
  baseline::logging::Format value;
  std::string_view name;
  std::string_view feature_id;
  bool has_metadata;
};

struct ScenarioDescriptor final {
  Scenario value;
  std::string_view name;
  std::vector<std::string_view> feature_ids;
  std::vector<std::string_view> difference_ids;
};

struct CapturedCase final {
  std::string id;
  std::vector<std::string_view> feature_ids;
  std::vector<std::string_view> difference_ids;
  std::string_view format;
  std::string value;
  std::string timestamp;
  bool has_metadata;
};

class CapturingLogger final : public baseline::logging::impl::TextLogger {
 public:
  explicit CapturingLogger(baseline::logging::Format format) : TextLogger(format) {
    SetLevel(baseline::logging::Level::kTrace);
  }

  void Log(baseline::logging::Level,
           baseline::logging::impl::formatters::LoggerItemRef item) override {
    const auto* text_item = dynamic_cast<const baseline::logging::impl::TextLogItem*>(&item);
    if (!text_item) {
      error_ = "the baseline logger submitted a non-text item";
      return;
    }
    if (captured_) {
      error_ = "the baseline logger submitted more than one item";
      return;
    }
    output_.assign(text_item->log_line.data(), text_item->log_line.size());
    captured_ = true;
  }

  std::string TakeOutput() {
    if (!error_.empty()) {
      throw std::runtime_error(error_);
    }
    if (!captured_) {
      throw std::runtime_error("the baseline logger produced no text item");
    }
    return std::move(output_);
  }

 private:
  std::string error_;
  std::string output_;
  bool captured_{false};
};

baseline::logging::LogExtra::Value StringValue(std::string value) {
  return baseline::logging::LogExtra::Value{std::move(value)};
}

void AddUnicodeControls(baseline::logging::LogHelper& log) {
  baseline::logging::LogExtra extra;
  extra.Extend("A=.\t\r\n\0\\"s, StringValue("é=ok:still|\t\r\n\0\\"s));
  log << extra;
  const auto message = "Привет🌍|\r\n\0\t\\|literal:\\r\\n\\0\\t\\\\"s;
  log << std::string_view{message.data(), message.size()};
}

void AddOrderedScalars(baseline::logging::LogHelper& log) {
  using Value = baseline::logging::LogExtra::Value;

  baseline::logging::LogExtra extra;
  extra.Extend("string", Value{"alpha"s});
  extra.Extend("signed", Value{static_cast<long long>(-42)});
  extra.Extend("unsigned", Value{std::numeric_limits<unsigned long long>::max()});
  extra.Extend("float", Value{1.5F});
  extra.Extend("double", Value{-2.25});
  extra.Extend("bool_true", Value{true});
  extra.Extend("bool_false", Value{false});
  extra.Extend("json", Value{baseline::logging::JsonString{"{\n\"x\":1\r}"s}});
  log << extra;
  log << "scalars";
}

void AddReplacementFrozenDuplicate(baseline::logging::LogHelper& log) {
  using ExtendType = baseline::logging::LogExtra::ExtendType;
  using Value = baseline::logging::LogExtra::Value;

  baseline::logging::LogExtra extra;
  extra.Extend("first", Value{"one"s});
  extra.Extend("replace", Value{"old"s});
  extra.Extend("last", Value{"three"s});
  extra.Extend("replace", Value{"new"s});
  extra.Extend("frozen", Value{"keep"s}, ExtendType::kFrozen);
  extra.Extend("frozen", Value{"ignored"s});
  log << extra;
  log.PutTag("duplicate", Value{"one"s});
  log.PutTag("duplicate", Value{"two"s});
  log << "collisions";
}

std::string Capture(baseline::logging::Format format, Scenario scenario) {
  CapturingLogger logger{format};
  {
    const auto location =
        baseline::utils::impl::SourceLocation::Custom(kSourceLine, __FILE__, kFunctionName);
    baseline::logging::LogHelper log{
        logger,
        baseline::logging::Level::kInfo,
        baseline::logging::LogClass::kLog,
        location,
    };
    switch (scenario) {
      case Scenario::kEmpty:
        break;
      case Scenario::kSimple:
        log << "hello";
        break;
      case Scenario::kUnicodeControls:
        AddUnicodeControls(log);
        break;
      case Scenario::kOrderedScalars:
        AddOrderedScalars(log);
        break;
      case Scenario::kReplacementFrozenDuplicate:
        AddReplacementFrozenDuplicate(log);
        break;
    }
  }
  return logger.TakeOutput();
}

std::string ExtractTimestamp(std::string_view value, std::string_view format) {
  const std::string_view prefix = format == "ltsv" ? "timestamp:" : "tskv\ttimestamp=";
  if (!value.starts_with(prefix)) {
    throw std::runtime_error("text formatter output has no expected timestamp preamble");
  }
  const auto end = value.find('\t', prefix.size());
  if (end == std::string_view::npos) {
    throw std::runtime_error("text formatter output has no field after timestamp");
  }
  return std::string{value.substr(prefix.size(), end - prefix.size())};
}

std::vector<std::string_view> FeatureIds(const FormatDescriptor& format,
                                         const ScenarioDescriptor& scenario) {
  auto ids = scenario.feature_ids;
  ids.push_back(format.feature_id);
  if (format.has_metadata) {
    ids.push_back("API-010");
  }
  return ids;
}

std::vector<CapturedCase> CaptureCases() {
  const std::vector<FormatDescriptor> formats{
      {baseline::logging::Format::kTskv, "tskv", "FMT-002", true},
      {baseline::logging::Format::kLtsv, "ltsv", "FMT-003", true},
      {baseline::logging::Format::kRaw, "raw", "FMT-004", false},
  };
  const std::vector<ScenarioDescriptor> scenarios{
      {Scenario::kEmpty, "empty", {}, {}},
      {Scenario::kSimple, "simple", {"VAL-001"}, {}},
      {Scenario::kUnicodeControls, "unicode-controls", {"VAL-001", "VAL-006"}, {}},
      {
          Scenario::kOrderedScalars,
          "ordered-scalars",
          {"VAL-001", "VAL-006", "VAL-008"},
          {},
      },
      {
          Scenario::kReplacementFrozenDuplicate,
          "replacement-frozen-duplicate",
          {"VAL-001", "VAL-006", "VAL-007"},
          {"DEF-004"},
      },
  };

  std::vector<CapturedCase> cases;
  cases.reserve(formats.size() * scenarios.size());
  for (const auto& format : formats) {
    for (const auto& scenario : scenarios) {
      auto value = Capture(format.value, scenario.value);
      cases.push_back({
          .id = std::string{format.name} + "-" + std::string{scenario.name},
          .feature_ids = FeatureIds(format, scenario),
          .difference_ids = scenario.difference_ids,
          .format = format.name,
          .value = std::move(value),
          .timestamp = {},
          .has_metadata = format.has_metadata,
      });
      auto& captured_case = cases.back();
      if (captured_case.has_metadata) {
        captured_case.timestamp = ExtractTimestamp(captured_case.value, captured_case.format);
      }
    }
  }
  return cases;
}

void WriteJsonString(std::ostream& output, std::string_view value) {
  constexpr std::string_view kHex = "0123456789abcdef";

  output.put('"');
  for (const unsigned char character : value) {
    switch (character) {
      case '"':
        output << "\\\"";
        break;
      case '\\':
        output << "\\\\";
        break;
      case '\b':
        output << "\\b";
        break;
      case '\f':
        output << "\\f";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        if (character < 0x20) {
          output << "\\u00" << kHex[character >> 4] << kHex[character & 0x0f];
        } else {
          output.put(static_cast<char>(character));
        }
    }
  }
  output.put('"');
}

void WriteStringList(std::ostream& output, const std::vector<std::string_view>& values) {
  output.put('[');
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      output.put(',');
    }
    WriteJsonString(output, values[index]);
  }
  output.put(']');
}

void WriteCase(std::ostream& output, const CapturedCase& captured_case) {
  output << "{\"id\":";
  WriteJsonString(output, captured_case.id);
  output << ",\"feature_ids\":";
  WriteStringList(output, captured_case.feature_ids);
  output << ",\"difference_ids\":";
  WriteStringList(output, captured_case.difference_ids);
  output << ",\"platform\":\"portable\",\"observed\":{\"kind\":\"utf8\",\"format\":";
  WriteJsonString(output, captured_case.format);
  output << ",\"value\":";
  WriteJsonString(output, captured_case.value);
  output << "},\"normalization\":[";
  if (captured_case.has_metadata) {
    output << "{\"kind\":\"timestamp_local\",\"path\":\"/value\",\"value\":";
    WriteJsonString(output, captured_case.timestamp);
    output << ",\"occurrence\":0},{\"kind\":\"source_path\",\"path\":\"/value\",\"value\":";
    WriteJsonString(output, __FILE__);
    output << ",\"occurrence\":0}";
  }
  output << "]}";
}

void WriteEnvelope(std::ostream& output, const std::vector<CapturedCase>& cases) {
  output << "{\"probe_schema_version\":1,\"baseline\":{\"repository\":";
  WriteJsonString(output, ulog_baseline_attestation::kRepository);
  output << ",\"revision\":";
  WriteJsonString(output, ulog_baseline_attestation::kRevision);
  output << "},\"cases\":[";
  for (std::size_t index = 0; index < cases.size(); ++index) {
    if (index != 0) {
      output.put(',');
    }
    WriteCase(output, cases[index]);
  }
  output << "]}\n";
}

}  // namespace

int main() {
  try {
    WriteEnvelope(std::cout, CaptureCases());
  } catch (const std::exception& error) {
    std::cerr << "baseline text probe failed: " << error.what() << '\n';
    return 1;
  }
}
