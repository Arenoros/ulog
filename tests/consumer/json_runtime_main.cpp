// Writes every case of docs/migration/corpus/json-v1.json through the installed package, once
// through the encoded in-memory destination and once through a file route, and compares the
// frames byte for byte with json_corpus_frames.jsonl, whose path is the only argument.
// tests/scripts/json_corpus_frames_test.py checks that those golden lines parse to the committed
// corpus members, so together the two checks compare this output with the corpus. The corpus was
// captured at 2000-01-02T03:04:05.123456Z with its source path normalized, so this consumer
// injects that instant and replaces its source path with the placeholder.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <ulog/fields.hpp>
#include <ulog/format.hpp>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/operation.hpp>
#include <ulog/runtime.hpp>
#include <ulog/source_location.hpp>
#include <ulog/testing/in_memory_encoded_destination.hpp>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace std::string_view_literals;

constexpr std::string_view kSourcePath = "/work/probe/baseline_json_probe.cpp";
constexpr std::string_view kSourcePlaceholder = "<source-path>";
constexpr std::int64_t kCorpusInstant = 946'782'245'123'456;
constexpr std::size_t kMaximumRecordBytes = 1'024;
/// Golden lines per format: replacement-frozen-duplicate, then typed-nested-escaping.
constexpr std::size_t kCasesPerFormat = 2;

[[nodiscard]] std::int64_t CorpusClock(void*) noexcept { return kCorpusInstant; }

[[nodiscard]] std::string NormalizeSourcePaths(std::string text) {
  for (std::size_t position = text.find(kSourcePath); position != std::string::npos;
       position = text.find(kSourcePath, position + kSourcePlaceholder.size())) {
    text.replace(position, kSourcePath.size(), kSourcePlaceholder);
  }
  return text;
}

/// Logs the probe's replacement-frozen-duplicate scenario; false if a shadow key was accepted.
[[nodiscard]] bool LogReplacementFrozenDuplicate(const ulog::Logger& logger,
                                                 const ulog::SourceLocation& source,
                                                 ulog::Format format) {
  ulog::FieldSet extra;
  extra.Add("first", "one");
  extra.Add("replace", "old");
  extra.Add("last", "three");
  extra.Add("replace", "new");
  extra.Add("frozen", "keep", ulog::FieldMode::kFrozen);
  extra.Add("frozen", "ignored");
  bool reserved = true;
  if (format == ulog::Format::kJsonYaDeploy) {
    // The YaDeploy standard names are reserved, so these shadow values are rejected (DEF-004).
    for (const auto& [key, value] : std::array{std::array{"@timestamp"sv, "shadow-timestamp"sv},
                                               std::array{"levelStr"sv, "shadow-level"sv},
                                               std::array{"message"sv, "shadow-message"sv}}) {
      reserved = reserved && extra.Add(key, value) == ulog::FieldStatus::kReserved;
    }
  }
  logger.Log<ulog::Level::kInfo>(
      source,
      [&](ulog::FieldWriter& fields) {
        fields.Add(extra);
        fields.Add("duplicate", "one");
        fields.Add("duplicate", "two");
        if (format == ulog::Format::kJson) {
          for (const auto& [key, value] :
               std::array{std::array{"timestamp"sv, "shadow-timestamp"sv},
                          std::array{"level"sv, "shadow-level"sv},
                          std::array{"text"sv, "shadow-text"sv}}) {
            reserved = reserved && fields.Add(key, value) == ulog::FieldStatus::kReserved;
          }
        }
      },
      [] { return "collisions"sv; });
  return reserved;
}

void LogTypedNestedEscaping(const ulog::Logger& logger, const ulog::SourceLocation& source) {
  constexpr std::string_view kKey = "escaped-key\"\t\0\\"sv;
  constexpr std::string_view kValue =
      "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\xF0\x9F\x8C\x8D\t\r\n\0\\"sv;
  constexpr std::string_view kMessage =
      "message \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\xF0\x9F\x8C\x8D \"\t\r\n\0\\"sv;
  logger.Log<ulog::Level::kInfo>(
      source,
      [&](ulog::FieldWriter& fields) {
        fields.Add(ulog::Fields{
            {kKey, kValue},
            {"signed", -42LL},
            {"unsigned", std::numeric_limits<unsigned long long>::max()},
            {"float", 1.5F},
            {"double", -2.25},
            {"bool-true", true},
            {"bool-false", false},
            {"null", ulog::JsonString{}},
            {"nested",
             ulog::JsonString{
                 "{\n\"z\":1,\r\"a\":[\"x\",false,null],\"obj\":{\"b\":2,\"a\":true}}"}},
        });
      },
      [&] { return kMessage; });
}

[[nodiscard]] bool LogCorpus(const ulog::Logger& logger, ulog::Format format) {
  const auto source = ulog::SourceLocation::Custom(kSourcePath, "BaselineJsonProbe", 321);
  const bool reserved = LogReplacementFrozenDuplicate(logger, source, format);
  LogTypedNestedEscaping(logger, source);
  return reserved;
}

[[nodiscard]] ulog::RuntimeConfig CorpusRuntimeConfig() {
  return ulog::RuntimeConfig{
      .threshold = ulog::Level::kTrace,
      .payload_capacity_bytes = 4'096,
      .maximum_record_bytes = kMaximumRecordBytes,
      .producer_slots = 1,
      .ingress_cells = 4,
      .control_operations = 2,
      .worker_threads = 1,
      .startup_timeout = 2s,
      .destruction_timeout = 2s,
      .event_clock = ulog::EventClock{.context = nullptr, .now = &CorpusClock},
  };
}

[[nodiscard]] bool ShutdownSucceeded(ulog::Runtime& runtime) {
  auto shutdown = runtime.Shutdown();
  if (!shutdown) return false;
  const auto stopped = shutdown.operation.WaitUntil(std::chrono::steady_clock::now() + 2s);
  return stopped.status == ulog::OperationWaitStatus::kCompleted && stopped.completion &&
         stopped.completion->Outcome() == ulog::OperationOutcome::kSucceeded &&
         stopped.completion->Report().delivered_records == kCasesPerFormat;
}

/// Returns zero when the encoded destination receives `expected` in order, or a failure code.
[[nodiscard]] int RunDestination(ulog::Format format, const std::vector<std::string>& expected,
                                 int code) {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = kCasesPerFormat,
      .maximum_record_bytes = kMaximumRecordBytes,
      .format = format,
  }};
  auto created = ulog::Runtime::Create(CorpusRuntimeConfig(), destination);
  if (!created) return code;
  if (!LogCorpus(created.runtime->GetLogger(), format)) return code + 1;
  if (!ShutdownSucceeded(*created.runtime)) return code + 2;
  for (const std::string& frame : expected) {
    auto observed = destination.TryTake();
    if (!observed || NormalizeSourcePaths(std::string{observed->Bytes()}) != frame) {
      return code + 3;
    }
  }
  return destination.TryTake().has_value() ? code + 4 : 0;
}

/// Returns zero when a file route writes `expected`, or a failure code.
[[nodiscard]] int RunFile(ulog::Format format, const std::vector<std::string>& expected, int code) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / ("ulog-json-consumer-" + std::to_string(nonce));
  const std::filesystem::path path = directory / "app.log";
  int status = 0;
  {
    auto created = ulog::Runtime::Create(CorpusRuntimeConfig(),
                                         ulog::RawFileRouteConfig{.path = path, .format = format});
    if (!created) {
      status = code;
    } else if (!LogCorpus(created.runtime->GetLogger(), format)) {
      status = code + 1;
    } else if (!ShutdownSucceeded(*created.runtime)) {
      status = code + 2;
    }
  }
  if (status == 0) {
    std::ifstream input{path, std::ios::binary};
    const std::string bytes{std::istreambuf_iterator<char>{input},
                            std::istreambuf_iterator<char>{}};
    std::string joined;
    for (const std::string& frame : expected) {
      joined += frame;
    }
    if (NormalizeSourcePaths(bytes) != joined) status = code + 3;
  }
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  return status;
}

/// Reads the golden lines, each with its LF.
[[nodiscard]] std::vector<std::string> ReadGoldenLines(const char* path) {
  std::ifstream input{path, std::ios::binary};
  const std::string bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
  std::vector<std::string> lines;
  std::size_t begin = 0;
  for (std::size_t end = bytes.find('\n'); end != std::string::npos;
       end = bytes.find('\n', begin)) {
    lines.push_back(bytes.substr(begin, end + 1U - begin));
    begin = end + 1U;
  }
  return begin == bytes.size() ? lines : std::vector<std::string>{};
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 1;
  const std::vector<std::string> golden = ReadGoldenLines(argv[1]);
  if (golden.size() != 2U * kCasesPerFormat) return 2;
  const std::vector<std::string> json{golden[0], golden[1]};
  const std::vector<std::string> yadeploy{golden[2], golden[3]};
  if (const int status = RunDestination(ulog::Format::kJson, json, 10); status != 0) return status;
  if (const int status = RunDestination(ulog::Format::kJsonYaDeploy, yadeploy, 20); status != 0) {
    return status;
  }
  if (const int status = RunFile(ulog::Format::kJson, json, 30); status != 0) return status;
  return RunFile(ulog::Format::kJsonYaDeploy, yadeploy, 40);
}
