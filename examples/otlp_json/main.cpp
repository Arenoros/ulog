// Emits OTLP LogRecord JSON — one record per line — to a file.
// Hook up to otel-collector's `filelog` receiver + `json_parser` for
// real ingestion into Loki / Tempo / any OTLP backend.

#include <ulog/config.hpp>
#include <ulog/log.hpp>

int main() {
    ulog::LoggerConfig cfg;
    cfg.file_path = "otlp_example.jsonl";
    cfg.format    = ulog::Format::kOtlpJson;
    cfg.level     = ulog::Level::kDebug;
    auto logger   = ulog::InitDefaultLogger(cfg);

    LOG_INFO() << "user logged in" << ulog::LogExtra{
        {"user_id",    42},
        {"latency_ms", 13.5},
        {"ok",         true},
        {"endpoint",   std::string("/api/login")},
    };

    LOG_ERROR() << "rpc failed" << ulog::LogExtra{
        {"code",   "INTERNAL"},
        {"retry",  3},
    };

    ulog::LogFlush();
    ulog::SetNullDefaultLogger();
    return 0;
}
