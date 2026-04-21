#include <ulog/config.hpp>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <ulog/log.hpp>
#include <ulog/sinks/fd_sink.hpp>
#include <ulog/sinks/file_sink.hpp>
#include <ulog/sinks/null_sink.hpp>
#include <ulog/sinks/tcp_socket_sink.hpp>

#if !defined(_WIN32) || defined(ULOG_HAVE_AFUNIX)
#include <ulog/sinks/unix_socket_sink.hpp>
#endif

#if defined(ULOG_HAVE_HTTP)
#include <ulog/sinks/otlp_batch_sink.hpp>
#endif

namespace ulog {

namespace {

bool StartsWith(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

sinks::SinkPtr MakeTcpSink(std::string_view spec) {
    // "tcp:host:port"
    auto rest = spec.substr(std::string_view("tcp:").size());
    const auto colon = rest.rfind(':');
    if (colon == std::string_view::npos) {
        throw std::invalid_argument(fmt::format("Invalid tcp sink spec '{}'", spec));
    }
    std::string host(rest.substr(0, colon));
    const auto port_str = rest.substr(colon + 1);
    int port = 0;
    for (char c : port_str) {
        if (c < '0' || c > '9') throw std::invalid_argument(fmt::format("Invalid tcp port '{}'", port_str));
        port = port * 10 + (c - '0');
    }
    if (port <= 0 || port > 65535) throw std::invalid_argument(fmt::format("tcp port out of range '{}'", port_str));
    return std::make_shared<sinks::TcpSocketSink>(std::move(host), static_cast<std::uint16_t>(port));
}

}  // namespace

sinks::SinkPtr MakeSinkFromSpec(const std::string& spec, bool truncate_on_start) {
    if (spec == "@stdout") return sinks::StdoutSink();
    if (spec == "@stderr") return sinks::StderrSink();
    if (spec == "@null")   return std::make_shared<sinks::NullSink>();
    if (StartsWith(spec, "tcp:")) return MakeTcpSink(spec);
    if (StartsWith(spec, "unix:")) {
#if !defined(_WIN32) || defined(ULOG_HAVE_AFUNIX)
        return std::make_shared<sinks::UnixSocketSink>(spec.substr(std::string_view("unix:").size()));
#else
        throw std::invalid_argument("unix sockets unsupported on this platform");
#endif
    }
    if (StartsWith(spec, "otlp:")) {
#if defined(ULOG_HAVE_HTTP)
        // spec = "otlp:http://host:port/path"
        sinks::OtlpBatchSink::Config cfg;
        cfg.endpoint = std::string(spec.substr(std::string_view("otlp:").size()));
        return std::make_shared<sinks::OtlpBatchSink>(cfg);
#else
        throw std::invalid_argument("otlp:// sinks require -DULOG_WITH_HTTP=ON");
#endif
    }
    return std::make_shared<sinks::FileSink>(spec, truncate_on_start);
}

std::shared_ptr<SyncLogger> MakeSyncLogger(const LoggerConfig& cfg) {
    auto logger = std::make_shared<SyncLogger>(cfg.format, cfg.emit_location, cfg.timestamp_format);
    logger->SetLevel(cfg.level);
    logger->SetFlushOn(cfg.flush_level);
    logger->AddSink(MakeSinkFromSpec(cfg.file_path, cfg.truncate_on_start));
    return logger;
}

std::shared_ptr<AsyncLogger> MakeAsyncLogger(const LoggerConfig& cfg) {
    AsyncLogger::Config async_cfg;
    async_cfg.format = cfg.format;
    async_cfg.queue_capacity = cfg.queue_capacity;
    async_cfg.overflow = cfg.overflow;
    async_cfg.emit_location = cfg.emit_location;
    async_cfg.timestamp_format = cfg.timestamp_format;
    auto logger = std::make_shared<AsyncLogger>(async_cfg);
    logger->SetLevel(cfg.level);
    logger->SetFlushOn(cfg.flush_level);
    logger->AddSink(MakeSinkFromSpec(cfg.file_path, cfg.truncate_on_start));
    return logger;
}

std::shared_ptr<AsyncLogger> InitDefaultLogger(const LoggerConfig& cfg) {
    auto logger = MakeAsyncLogger(cfg);
    impl::SetDefaultLoggerRef(*logger);
    return logger;
}

}  // namespace ulog
