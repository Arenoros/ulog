#include <ulog/sinks/otlp_batch_sink.hpp>

#if defined(ULOG_HAVE_HTTP)

#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

// cpp-httplib pulls in platform sockets (winsock2 on Windows, BSD
// sockets on POSIX) — include only in this TU to keep compile times
// for the rest of ulog unchanged.
#include <httplib.h>

namespace ulog::sinks {

namespace {

struct ParsedEndpoint {
    std::string host;
    std::uint16_t port{0};
    std::string path;
};

/// Parses `http://host[:port][/path]` into three pieces. Throws on a
/// malformed endpoint; HTTPS is not supported (out of scope — the sink
/// is intended for same-cluster collector POSTs).
ParsedEndpoint ParseEndpoint(const std::string& url) {
    constexpr std::string_view kScheme = "http://";
    if (url.compare(0, kScheme.size(), kScheme) != 0) {
        throw std::runtime_error("OtlpBatchSink: endpoint must start with http://: " + url);
    }
    const std::string_view tail(url.data() + kScheme.size(),
                                url.size() - kScheme.size());
    const auto slash = tail.find('/');
    const std::string_view host_port = (slash == std::string_view::npos)
        ? tail
        : tail.substr(0, slash);
    const std::string_view path = (slash == std::string_view::npos)
        ? std::string_view{"/"}
        : tail.substr(slash);

    ParsedEndpoint out;
    const auto colon = host_port.find(':');
    if (colon == std::string_view::npos) {
        out.host.assign(host_port);
        out.port = 80;
    } else {
        out.host.assign(host_port.substr(0, colon));
        const auto port_str = host_port.substr(colon + 1);
        try {
            const int p = std::stoi(std::string(port_str));
            if (p <= 0 || p > 65535) throw std::out_of_range("port");
            out.port = static_cast<std::uint16_t>(p);
        } catch (...) {
            throw std::runtime_error("OtlpBatchSink: invalid port in endpoint: " + url);
        }
    }
    out.path.assign(path);
    return out;
}

/// Appends `sv` as a JSON-escaped string body (no surrounding quotes).
/// Mirrors `OtlpJsonFormatter`'s escape rules so the envelope speaks
/// the same dialect as the records it carries.
void AppendJsonEscaped(std::string& out, std::string_view sv) {
    for (unsigned char c : sv) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
}

/// Strips a trailing '\n' if present — `OtlpJsonFormatter` ends every
/// record with one, but the envelope embeds records directly so the
/// newline would produce invalid JSON.
std::string_view RstripNewline(std::string_view sv) {
    if (!sv.empty() && sv.back() == '\n') sv.remove_suffix(1);
    return sv;
}

}  // namespace

OtlpBatchSink::OtlpBatchSink(Config cfg) : cfg_(std::move(cfg)) {
    if (cfg_.endpoint.empty()) {
        throw std::runtime_error("OtlpBatchSink: empty endpoint");
    }
    const auto parsed = ParseEndpoint(cfg_.endpoint);
    host_ = parsed.host;
    port_ = parsed.port;
    path_ = parsed.path;
}

OtlpBatchSink::~OtlpBatchSink() {
    // Best-effort final flush so a clean shutdown still delivers
    // records. Failures are swallowed.
    try { Flush(); } catch (...) {}
}

void OtlpBatchSink::Write(std::string_view record) {
    std::lock_guard lock(mu_);
    buffer_.emplace_back(RstripNewline(record));
    if (cfg_.batch_size > 0 && buffer_.size() >= cfg_.batch_size) {
        const auto envelope = BuildEnvelopeLocked();
        const std::size_t count = buffer_.size();
        buffer_.clear();
        PostEnvelopeLocked(envelope, count);
    }
}

void OtlpBatchSink::Flush() {
    std::lock_guard lock(mu_);
    if (buffer_.empty()) return;
    const auto envelope = BuildEnvelopeLocked();
    const std::size_t count = buffer_.size();
    buffer_.clear();
    PostEnvelopeLocked(envelope, count);
}

SinkStats OtlpBatchSink::GetStats() const noexcept {
    std::lock_guard lock(mu_);
    SinkStats s;
    s.writes = records_sent_;
    s.errors = records_dropped_;
    // Latency histogram stays zeroed — batch sinks have per-batch
    // timing, not per-record; operators that need HTTP-level metrics
    // should wrap the sink in an `InstrumentedSink`, which records
    // per-`Write()` latency.
    return s;
}

std::size_t OtlpBatchSink::BufferedRecords() const noexcept {
    std::lock_guard lock(mu_);
    return buffer_.size();
}

std::string OtlpBatchSink::BuildEnvelopeLocked() const {
    std::string out;
    std::size_t reserve = 256;
    for (const auto& r : buffer_) reserve += r.size() + 1;
    out.reserve(reserve);

    out += R"({"resourceLogs":[{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":")";
    AppendJsonEscaped(out, cfg_.service_name);
    out += R"("}}]},"scopeLogs":[{"scope":{"name":"ulog"},"logRecords":[)";
    bool first = true;
    for (const auto& rec : buffer_) {
        if (!first) out += ',';
        out += rec;
        first = false;
    }
    out += "]}]}]}";
    return out;
}

void OtlpBatchSink::PostEnvelopeLocked(const std::string& envelope,
                                        std::size_t record_count) {
    httplib::Client cli(host_, port_);
    cli.set_connection_timeout(cfg_.timeout);
    cli.set_read_timeout(cfg_.timeout);
    cli.set_write_timeout(cfg_.timeout);

    httplib::Headers headers{{"Content-Type", "application/json"}};
    for (const auto& kv : cfg_.extra_headers) {
        headers.emplace(kv.first, kv.second);
    }

    ++flushes_;
    auto res = cli.Post(path_.c_str(), headers, envelope, "application/json");
    const bool ok = res && res->status >= 200 && res->status < 300;
    if (ok) {
        records_sent_ += record_count;
    } else {
        ++failures_;
        records_dropped_ += record_count;
    }
}

}  // namespace ulog::sinks

#endif  // ULOG_HAVE_HTTP
