#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <userver/logging/format.hpp>
#include <userver/logging/json_string.hpp>
#include <userver/logging/level.hpp>
#include <userver/logging/log_extra.hpp>
#include <userver/logging/log_helper.hpp>
#include <userver/utils/impl/source_location.hpp>
#include <utility>
#include <vector>

#include "probe_support.hpp"

namespace {

namespace baseline = USERVER_NAMESPACE;
namespace probe = ulog_baseline_probe;
using namespace std::string_literals;

constexpr std::uint_least32_t kSourceLine = 321;
constexpr std::string_view kFunctionName = "BaselineJsonProbe";

enum class Scenario {
  kTypedNestedEscaping,
  kReplacementFrozenDuplicate,
};

struct FormatDescriptor final {
  baseline::logging::Format value;
  std::string_view name;
  std::string_view case_prefix;
  std::string_view feature_id;
};

struct ScenarioDescriptor final {
  Scenario value;
  std::string_view name;
  std::vector<std::string_view> feature_ids;
  std::vector<std::string_view> difference_ids;
};

void AddTypedNestedEscaping(baseline::logging::LogHelper& log) {
  using Value = baseline::logging::LogExtra::Value;

  baseline::logging::LogExtra extra;
  extra.Extend("escaped-key\"\t\0\\"s, Value{"Привет🌍\t\r\n\0\\"s});
  extra.Extend("signed", Value{static_cast<long long>(-42)});
  extra.Extend("unsigned", Value{std::numeric_limits<unsigned long long>::max()});
  extra.Extend("float", Value{1.5F});
  extra.Extend("double", Value{-2.25});
  extra.Extend("bool-true", Value{true});
  extra.Extend("bool-false", Value{false});
  extra.Extend("null", Value{baseline::logging::JsonString{}});
  extra.Extend("nested",
               Value{baseline::logging::JsonString{
                   "{\n\"z\":1,\r\"a\":[\"x\",false,null],\"obj\":{\"b\":2,\"a\":true}}"s}});
  log << extra;
  const auto message = "message Привет🌍 \"\t\r\n\0\\"s;
  log << std::string_view{message.data(), message.size()};
}

void AddReplacementFrozenDuplicate(baseline::logging::LogHelper& log,
                                   baseline::logging::Format format) {
  using ExtendType = baseline::logging::LogExtra::ExtendType;
  using Value = baseline::logging::LogExtra::Value;

  baseline::logging::LogExtra extra;
  extra.Extend("first", Value{"one"s});
  extra.Extend("replace", Value{"old"s});
  extra.Extend("last", Value{"three"s});
  extra.Extend("replace", Value{"new"s});
  extra.Extend("frozen", Value{"keep"s}, ExtendType::kFrozen);
  extra.Extend("frozen", Value{"ignored"s});
  if (format == baseline::logging::Format::kJsonYaDeploy) {
    extra.Extend("@timestamp", Value{"shadow-timestamp"s});
    extra.Extend("levelStr", Value{"shadow-level"s});
    extra.Extend("message", Value{"shadow-message"s});
  }
  log << extra;
  log.PutTag("duplicate", Value{"one"s});
  log.PutTag("duplicate", Value{"two"s});
  if (format == baseline::logging::Format::kJson) {
    log.PutTag("timestamp", Value{"shadow-timestamp"s});
    log.PutTag("level", Value{"shadow-level"s});
    log.PutTag("text", Value{"shadow-text"s});
  }
  log << "collisions";
}

std::string Capture(baseline::logging::Format format, Scenario scenario) {
  probe::CapturingLogger logger{format};
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
      case Scenario::kTypedNestedEscaping:
        AddTypedNestedEscaping(log);
        break;
      case Scenario::kReplacementFrozenDuplicate:
        AddReplacementFrozenDuplicate(log, format);
        break;
    }
  }
  return logger.TakeOutput();
}

std::string ExtractTimestamp(std::string_view value, std::string_view format) {
  const std::string_view prefix = format == "json" ? "{\"timestamp\":\"" : "{\"@timestamp\":\"";
  if (!value.starts_with(prefix)) {
    throw std::runtime_error("JSON formatter output has no expected timestamp preamble");
  }
  const auto end = value.find('"', prefix.size());
  if (end == std::string_view::npos) {
    throw std::runtime_error("JSON formatter output has an unterminated timestamp");
  }
  return std::string{value.substr(prefix.size(), end - prefix.size())};
}

std::vector<std::string_view> FeatureIds(const FormatDescriptor& format,
                                         const ScenarioDescriptor& scenario) {
  auto ids = scenario.feature_ids;
  ids.push_back(format.feature_id);
  ids.push_back("API-010");
  return ids;
}

std::vector<probe::CapturedCase> CaptureCases() {
  const std::vector<FormatDescriptor> formats{
      {baseline::logging::Format::kJson, "json", "json", "FMT-005"},
      {
          baseline::logging::Format::kJsonYaDeploy,
          "json_yadeploy",
          "json-yadeploy",
          "FMT-006",
      },
  };
  const std::vector<ScenarioDescriptor> scenarios{
      {
          Scenario::kTypedNestedEscaping,
          "typed-nested-escaping",
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

  std::vector<probe::CapturedCase> cases;
  cases.reserve(formats.size() * scenarios.size());
  for (const auto& format : formats) {
    for (const auto& scenario : scenarios) {
      auto value = Capture(format.value, scenario.value);
      auto timestamp = ExtractTimestamp(value, format.name);
      cases.push_back({
          .id = std::string{format.case_prefix} + "-" + std::string{scenario.name},
          .feature_ids = FeatureIds(format, scenario),
          .difference_ids = scenario.difference_ids,
          .observed_kind = "json-line",
          .format = format.name,
          .value = std::move(value),
          .normalization =
              {
                  {
                      .kind = "timestamp_utc",
                      .path = "/value",
                      .value = std::move(timestamp),
                      .occurrence = 0,
                  },
                  {
                      .kind = "source_path",
                      .path = "/value",
                      .value = __FILE__,
                      .occurrence = 0,
                  },
              },
      });
    }
  }
  return cases;
}

}  // namespace

int main() {
  try {
    probe::WriteEnvelope(std::cout, CaptureCases());
  } catch (const std::exception& error) {
    std::cerr << "baseline JSON probe failed: " << error.what() << '\n';
    return 1;
  }
}
