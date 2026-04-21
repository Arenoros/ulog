/// Example: plain JSON formatter with a structured-log consumer workflow.
///
/// Run this binary and then pipe the resulting file through `jq` (or
/// any structured-log tool):
///
///     ./ulog-example-json-consumer
///     tail -f json_example.jsonl | jq -c 'select(.level == "ERROR")'
///     tail -f json_example.jsonl | jq -c 'select(.user_id)'
///
/// The emitted shape (one JSON object per line) is the canonical
/// Filebeat / Fluent Bit / Vector / Promtail / `jq` target: strings,
/// numbers, booleans come through as their native JSON types (Phase
/// 22 typed-tag API), so `jq '.latency_ms > 10'` works without any
/// parser hints.
///
/// Default file path is `json_example.jsonl` in the current directory;
/// override via the first CLI argument.

#include <cstdio>
#include <string>

#include <ulog/config.hpp>
#include <ulog/log.hpp>

int main(int argc, char** argv) {
    ulog::LoggerConfig cfg;
    cfg.file_path = (argc > 1) ? argv[1] : "json_example.jsonl";
    cfg.format    = ulog::Format::kJson;
    cfg.level     = ulog::Level::kDebug;
    auto logger   = ulog::InitDefaultLogger(cfg);

    LOG_INFO() << "user logged in" << ulog::LogExtra{
        {"user_id",    42},
        {"latency_ms", 13.5},
        {"ok",         true},
        {"endpoint",   std::string("/api/login")},
    };

    LOG_WARNING() << "cache miss" << ulog::LogExtra{
        {"key",    std::string("session:abc123")},
        {"bucket", std::string("hot")},
    };

    LOG_ERROR() << "rpc failed" << ulog::LogExtra{
        {"code",  std::string("INTERNAL")},
        {"retry", 3},
        {"fatal", false},
    };

    ulog::LogFlush();
    ulog::SetNullDefaultLogger();

    std::puts("Records written. Consume with:");
    std::printf("  tail -f %s | jq -c 'select(.level == \"ERROR\")'\n",
                cfg.file_path.c_str());
    return 0;
}
