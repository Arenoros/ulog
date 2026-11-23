// Writes every tskv-* and ltsv-* case of docs/migration/corpus/text-v1.json through the installed
// package and compares the frames with the committed corpus values. The corpus was captured at
// 2000-01-02T03:04:05.123456 in UTC with its source path normalized, so this consumer injects that
// instant, fixes the process time zone to UTC, and replaces its source path with the placeholder.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>
#include <string_view>
#include <ulog/format.hpp>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/operation.hpp>
#include <ulog/runtime.hpp>
#include <ulog/source_location.hpp>
#include <ulog/testing/in_memory_encoded_destination.hpp>
#include <ulog/timestamp.hpp>

#include "raw_corpus_cases.hpp"

namespace {

using namespace std::chrono_literals;
using namespace std::string_view_literals;

// Byte-for-byte copies of each case's `observed.value`, except that the
// replacement-frozen-duplicate cases keep one `duplicate` field, as recorded under DEF-004.
inline constexpr std::array<std::string_view, 5> kTskvFrames{
    // tskv-empty
    "tskv\ttimestamp=2000-01-02T03:04:05.123456\tlevel=INFO\tmodule=BaselineTextProbe ( <source-path>:321 )\ttext=\n"sv,
    // tskv-simple
    "tskv\ttimestamp=2000-01-02T03:04:05.123456\tlevel=INFO\tmodule=BaselineTextProbe ( <source-path>:321 )\ttext=hello\n"sv,
    // tskv-ordered-scalars
    "tskv\ttimestamp=2000-01-02T03:04:05.123456\tlevel=INFO\tmodule=BaselineTextProbe ( <source-path>:321 )\tstring=alpha\tsigned=-42\tunsigned=18446744073709551615\tfloat=1.5\tdouble=-2.25\tbool_true=1\tbool_false=0\tjson={\"x\":1}\ttext=scalars\n"sv,
    // tskv-replacement-frozen-duplicate
    "tskv\ttimestamp=2000-01-02T03:04:05.123456\tlevel=INFO\tmodule=BaselineTextProbe ( <source-path>:321 )\tfirst=one\treplace=new\tlast=three\tfrozen=keep\tduplicate=two\ttext=collisions\n"sv,
    // tskv-unicode-controls
    "tskv\ttimestamp=2000-01-02T03:04:05.123456\tlevel=INFO\tmodule=BaselineTextProbe ( <source-path>:321 )\ta\\=.\\t\\r\\n\\0\\\\=\xC3\xA9=ok:still|\\t\\r\\n\\0\\\\\ttext=\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\xF0\x9F\x8C\x8D|\\r\\n\\0\\t\\\\|literal:\\\\r\\\\n\\\\0\\\\t\\\\\\\\\n"sv,
};

inline constexpr std::array<std::string_view, 5> kLtsvFrames{
    // ltsv-empty
    "timestamp:2000-01-02T03:04:05.123456\tlevel:INFO\tmodule:BaselineTextProbe ( <source-path>:321 )\ttext:\n"sv,
    // ltsv-simple
    "timestamp:2000-01-02T03:04:05.123456\tlevel:INFO\tmodule:BaselineTextProbe ( <source-path>:321 )\ttext:hello\n"sv,
    // ltsv-ordered-scalars
    "timestamp:2000-01-02T03:04:05.123456\tlevel:INFO\tmodule:BaselineTextProbe ( <source-path>:321 )\tstring:alpha\tsigned:-42\tunsigned:18446744073709551615\tfloat:1.5\tdouble:-2.25\tbool_true:1\tbool_false:0\tjson:{\"x\":1}\ttext:scalars\n"sv,
    // ltsv-replacement-frozen-duplicate
    "timestamp:2000-01-02T03:04:05.123456\tlevel:INFO\tmodule:BaselineTextProbe ( <source-path>:321 )\tfirst:one\treplace:new\tlast:three\tfrozen:keep\tduplicate:two\ttext:collisions\n"sv,
    // ltsv-unicode-controls
    "timestamp:2000-01-02T03:04:05.123456\tlevel:INFO\tmodule:BaselineTextProbe ( <source-path>:321 )\ta\\=.\\t\\r\\n\\0\\\\:\xC3\xA9=ok:still|\\t\\r\\n\\0\\\\\ttext:\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\xF0\x9F\x8C\x8D|\\r\\n\\0\\t\\\\|literal:\\\\r\\\\n\\\\0\\\\t\\\\\\\\\n"sv,
};

constexpr std::string_view kSourcePath = "/work/probe/baseline_text_probe.cpp";
constexpr std::string_view kSourcePlaceholder = "<source-path>";
constexpr std::int64_t kCorpusInstant = 946'782'245'123'456;

[[nodiscard]] std::int64_t CorpusClock(void*) noexcept { return kCorpusInstant; }

void UseUtcTimeZone() {
#if defined(_WIN32)
  static_cast<void>(_putenv_s("TZ", "UTC0"));
  _tzset();
#else
  static_cast<void>(setenv("TZ", "UTC0", 1));
  tzset();
#endif
}

[[nodiscard]] std::string NormalizeSourcePath(std::string_view frame) {
  std::string normalized{frame};
  const std::size_t position = normalized.find(kSourcePath);
  if (position != std::string::npos) {
    normalized.replace(position, kSourcePath.size(), kSourcePlaceholder);
  }
  return normalized;
}

[[nodiscard]] ulog::RuntimeConfig CorpusRuntimeConfig() {
  return ulog::RuntimeConfig{
      .threshold = ulog::Level::kTrace,
      .payload_capacity_bytes = 2'560,
      .maximum_record_bytes = 512,
      .producer_slots = 1,
      .ingress_cells = 5,
      .control_operations = 2,
      .worker_threads = 1,
      .startup_timeout = 1s,
      .destruction_timeout = 1s,
      .event_clock = ulog::EventClock{.context = nullptr, .now = &CorpusClock},
  };
}

/// Returns zero when `format` writes `expected` in order, or a distinct failure code.
[[nodiscard]] int RunCorpus(ulog::Format format, const std::array<std::string_view, 5>& expected,
                            int code) {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = expected.size(),
      .maximum_record_bytes = 512,
      .format = format,
  }};
  auto created = ulog::Runtime::Create(CorpusRuntimeConfig(), destination);
  if (!created) return code;

  const ulog::Logger logger = created.runtime->GetLogger();
  const auto source = ulog::SourceLocation::Custom(kSourcePath, "BaselineTextProbe", 321);
  logger.Log<ulog::Level::kInfo>(source, [] { return ""sv; });
  logger.Log<ulog::Level::kInfo>(source, [] { return "hello"sv; });
  raw_corpus::LogAll(logger, source);

  auto shutdown = created.runtime->Shutdown();
  if (!shutdown) return code + 1;
  const auto stopped = shutdown.operation.WaitUntil(std::chrono::steady_clock::now() + 1s);
  if (stopped.status != ulog::OperationWaitStatus::kCompleted || !stopped.completion ||
      stopped.completion->Outcome() != ulog::OperationOutcome::kSucceeded) {
    return code + 2;
  }
  for (const std::string_view frame : expected) {
    auto observed = destination.TryTake();
    if (!observed || NormalizeSourcePath(observed->Bytes()) != frame) return code + 3;
  }
  return destination.TryTake().has_value() ? code + 4 : 0;
}

}  // namespace

int main() {
  UseUtcTimeZone();
  if (ulog::FormatLocalTimestamp(kCorpusInstant).View() != "2000-01-02T03:04:05.123456") return 1;
  if (const int status = RunCorpus(ulog::Format::kTskv, kTskvFrames, 10); status != 0) {
    return status;
  }
  return RunCorpus(ulog::Format::kLtsv, kLtsvFrames, 20);
}
