#pragma once

#include <cstddef>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <userver/logging/format.hpp>
#include <userver/logging/impl/logger_base.hpp>
#include <userver/logging/level.hpp>
#include <utility>
#include <vector>

#include "baseline_attestation.hpp"

namespace ulog_baseline_probe {

namespace baseline = USERVER_NAMESPACE;

struct NormalizationRule final {
  std::string_view kind;
  std::string_view path;
  std::string value;
  std::size_t occurrence;
};

struct CapturedCase final {
  std::string id;
  std::vector<std::string_view> feature_ids;
  std::vector<std::string_view> difference_ids;
  std::string_view observed_kind;
  std::string_view format;
  std::string value;
  std::vector<NormalizationRule> normalization;
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

inline void WriteJsonString(std::ostream& output, std::string_view value) {
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

inline void WriteStringList(std::ostream& output, const std::vector<std::string_view>& values) {
  output.put('[');
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      output.put(',');
    }
    WriteJsonString(output, values[index]);
  }
  output.put(']');
}

inline void WriteNormalization(std::ostream& output,
                               const std::vector<NormalizationRule>& normalization) {
  output.put('[');
  for (std::size_t index = 0; index < normalization.size(); ++index) {
    if (index != 0) {
      output.put(',');
    }
    const auto& rule = normalization[index];
    output << "{\"kind\":";
    WriteJsonString(output, rule.kind);
    output << ",\"path\":";
    WriteJsonString(output, rule.path);
    output << ",\"value\":";
    WriteJsonString(output, rule.value);
    output << ",\"occurrence\":" << rule.occurrence << '}';
  }
  output.put(']');
}

inline void WriteCase(std::ostream& output, const CapturedCase& captured_case) {
  output << "{\"id\":";
  WriteJsonString(output, captured_case.id);
  output << ",\"feature_ids\":";
  WriteStringList(output, captured_case.feature_ids);
  output << ",\"difference_ids\":";
  WriteStringList(output, captured_case.difference_ids);
  output << ",\"platform\":\"portable\",\"observed\":{\"kind\":";
  WriteJsonString(output, captured_case.observed_kind);
  output << ",\"format\":";
  WriteJsonString(output, captured_case.format);
  output << ",\"value\":";
  WriteJsonString(output, captured_case.value);
  output << "},\"normalization\":";
  WriteNormalization(output, captured_case.normalization);
  output.put('}');
}

inline void WriteEnvelope(std::ostream& output, const std::vector<CapturedCase>& cases) {
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

}  // namespace ulog_baseline_probe
