/// Example: ship OTLP LogRecord JSON directly to an OpenTelemetry
/// collector over HTTP, no sidecar needed.
///
/// Prerequisites:
///   * build ulog with `-DULOG_WITH_HTTP=ON` (pulls cpp-httplib via
///     Conan) — otherwise this example is excluded from the build.
///   * an OTLP/HTTP collector reachable at
///     `http://<host>:4318/v1/logs`. Spin one up locally with:
///         docker run -p 4318:4318 otel/opentelemetry-collector
///
/// Default endpoint is `http://127.0.0.1:4318/v1/logs`; override via
/// the first CLI argument.

#include <cstdio>
#include <memory>
#include <string>

#include <ulog/config.hpp>
#include <ulog/log.hpp>
#include <ulog/sinks/otlp_batch_sink.hpp>
#include <ulog/sync_logger.hpp>

int main(int argc, char** argv) {
    const std::string endpoint =
        (argc > 1) ? argv[1] : "http://127.0.0.1:4318/v1/logs";

    ulog::sinks::OtlpBatchSink::Config sink_cfg;
    sink_cfg.endpoint = endpoint;
    sink_cfg.batch_size = 4;
    sink_cfg.service_name = "ulog-example-otlp-http";
    auto http_sink = std::make_shared<ulog::sinks::OtlpBatchSink>(sink_cfg);

    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kOtlpJson);
    logger->SetLevel(ulog::Level::kTrace);
    logger->AddSink(http_sink);
    ulog::impl::SetDefaultLoggerRef(*logger);

    LOG_INFO() << "user logged in" << ulog::LogExtra{
        {"user_id",    42},
        {"latency_ms", 13.5},
        {"ok",         true},
        {"endpoint",   std::string("/api/login")},
    };
    LOG_WARNING() << "cache miss" << ulog::LogExtra{
        {"key",    std::string("session:abc")},
        {"bucket", std::string("hot")},
    };
    LOG_ERROR() << "rpc failed" << ulog::LogExtra{
        {"code",   std::string("INTERNAL")},
        {"retry",  3},
    };
    LOG_INFO() << "request complete" << ulog::LogExtra{
        {"duration_ms", 87},
    };

    // Four records → auto-flush on batch_size=4; Flush below is a no-op
    // but shown for the canonical shutdown shape.
    ulog::LogFlush();
    ulog::SetNullDefaultLogger();

    std::printf("Posted to %s\n", endpoint.c_str());
    return 0;
}
