#pragma once

/// @file ulog/sinks/otlp_batch_sink.hpp
/// @brief Batches OTLP `LogRecord` JSON lines into an
/// `ExportLogsServiceRequest` envelope and POSTs them to an
/// OpenTelemetry-compatible HTTP collector.
///
/// Only compiled when `ULOG_HAVE_HTTP=1` is defined at build time
/// (CMake `-DULOG_WITH_HTTP=ON`). The dependency that brings it in
/// is `cpp-httplib` — single-header, MIT, no TLS unless the
/// consumer opts into the cpp-httplib OpenSSL path themselves.

#if defined(ULOG_HAVE_HTTP)

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <ulog/sinks/base_sink.hpp>

namespace ulog::sinks {

/// HTTP collector sink. Pair with `Format::kOtlpJson` loggers: each
/// record fed into `Write(...)` must be one finalized OTLP LogRecord
/// JSON object (the formatter's default output). The sink buffers
/// records and flushes them as a single `ExportLogsServiceRequest`
/// envelope via HTTP POST.
///
/// Thread-safety: `Write` / `Flush` / `Reopen` / `GetStats` all take an
/// internal mutex. Safe to share a single instance across producer
/// threads.
///
/// Example:
/// @code
///     ulog::sinks::OtlpBatchSink::Config cfg;
///     cfg.endpoint = "http://collector:4318/v1/logs";
///     cfg.batch_size = 512;
///     cfg.service_name = "myservice";
///     auto sink = std::make_shared<ulog::sinks::OtlpBatchSink>(cfg);
/// @endcode
class OtlpBatchSink final : public BaseSink {
public:
    struct Config {
        /// Full HTTP URL of the collector's OTLP logs endpoint.
        /// Typically `http://host:4318/v1/logs`. Parsing extracts host
        /// / port / path; missing port defaults to 80. HTTPS is not
        /// supported by this build — use the sidecar approach if you
        /// need TLS.
        std::string endpoint;

        /// Flush automatically once this many records have been
        /// buffered. `0` disables the auto-flush and relies purely on
        /// explicit `Flush()`.
        std::size_t batch_size = 512;

        /// HTTP read/write timeout. Applied to both connect and
        /// send/recv via cpp-httplib's Client.
        std::chrono::milliseconds timeout{std::chrono::seconds(5)};

        /// Emitted as the `service.name` resource attribute on every
        /// batch. Override to distinguish services in the collector.
        std::string service_name = "ulog";

        /// Extra HTTP headers to include on every POST — e.g. for
        /// `Authorization: Bearer <token>` against Grafana Cloud or
        /// Honeycomb. Vector of `{name, value}` pairs.
        std::vector<std::pair<std::string, std::string>> extra_headers;
    };

    explicit OtlpBatchSink(Config cfg);
    ~OtlpBatchSink() override;

    /// Accepts one OTLP LogRecord JSON object (with or without a
    /// trailing newline — the sink strips it). Appends to the buffer;
    /// auto-flushes when the batch threshold is reached.
    void Write(std::string_view record) override;

    /// Synchronously flushes the current buffer as a single HTTP POST.
    /// Safe to call at any time; no-op if the buffer is empty.
    void Flush() override;

    /// No-op — HTTP sinks have nothing to "reopen".
    void Reopen(ReopenMode /*mode*/) override {}

    /// Snapshot of flushes, POSTs, HTTP failures, dropped records.
    SinkStats GetStats() const noexcept override;

    /// Diagnostic — returns the currently-buffered record count.
    std::size_t BufferedRecords() const noexcept;

private:
    /// Builds the `ExportLogsServiceRequest` envelope from the current
    /// buffer. Caller must hold `mu_`.
    std::string BuildEnvelopeLocked() const;

    /// Ships the envelope as a single POST. Caller must hold `mu_`.
    /// Updates stats on the way. `record_count` lets us attribute the
    /// outcome (delivered / dropped) to the matching batch size.
    void PostEnvelopeLocked(const std::string& envelope,
                            std::size_t record_count);

    Config cfg_;
    std::string host_;
    std::uint16_t port_{0};
    std::string path_;

    mutable std::mutex mu_;
    std::vector<std::string> buffer_;

    std::uint64_t flushes_{0};
    std::uint64_t failures_{0};
    std::uint64_t records_sent_{0};
    std::uint64_t records_dropped_{0};
};

}  // namespace ulog::sinks

#endif  // ULOG_HAVE_HTTP
