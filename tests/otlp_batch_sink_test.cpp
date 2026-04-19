#if defined(ULOG_HAVE_HTTP)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <httplib.h>

#include <ulog/sinks/otlp_batch_sink.hpp>

namespace {

/// Minimal in-process HTTP server for the OTLP collector role. Records
/// each POST's body for inspection by the test. Uses cpp-httplib's
/// Server on a separate thread + ephemeral port.
class MiniCollector {
public:
    MiniCollector() {
        server_.Post("/v1/logs", [this](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard lock(mu_);
                bodies_.push_back(req.body);
                ++posts_;
            }
            res.status = fail_status_.load(std::memory_order_relaxed);
            res.set_content("{}", "application/json");
        });
        // Bind to loopback, port 0 lets the OS pick.
        port_ = server_.bind_to_any_port("127.0.0.1");
        runner_ = std::thread([this] { server_.listen_after_bind(); });
        // Wait until the server is actually serving so the first POST
        // from the sink doesn't race with setup.
        while (!server_.is_running()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ~MiniCollector() {
        server_.stop();
        if (runner_.joinable()) runner_.join();
    }

    int port() const noexcept { return port_; }

    std::vector<std::string> Bodies() const {
        std::lock_guard lock(mu_);
        return bodies_;
    }

    int PostCount() const {
        std::lock_guard lock(mu_);
        return posts_;
    }

    /// Force the next POSTs to respond with `status` (e.g. 500 for
    /// failure-mode tests). Default is 200.
    void SetResponseStatus(int status) {
        fail_status_.store(status, std::memory_order_relaxed);
    }

private:
    httplib::Server server_;
    std::thread runner_;
    int port_{0};
    mutable std::mutex mu_;
    std::vector<std::string> bodies_;
    int posts_{0};
    std::atomic<int> fail_status_{200};
};

std::string EndpointOf(const MiniCollector& c) {
    return "http://127.0.0.1:" + std::to_string(c.port()) + "/v1/logs";
}

/// Mimics what `OtlpJsonFormatter` would hand the sink: one LogRecord
/// per line, trailing '\n'. Enough for the sink's escape/envelope
/// tests — no need for the full formatter pipeline.
std::string MakeRecord(int i) {
    return "{\"severityNumber\":9,\"body\":{\"stringValue\":\"msg " + std::to_string(i) + "\"}}\n";
}

}  // namespace

TEST(OtlpBatchSink, FlushBuildsEnvelopeAndPosts) {
    MiniCollector collector;
    ulog::sinks::OtlpBatchSink::Config cfg;
    cfg.endpoint = EndpointOf(collector);
    cfg.batch_size = 0;  // disable auto-flush: explicit Flush only.
    cfg.service_name = "test-svc";
    auto sink = std::make_shared<ulog::sinks::OtlpBatchSink>(cfg);

    sink->Write(MakeRecord(1));
    sink->Write(MakeRecord(2));
    sink->Write(MakeRecord(3));

    ASSERT_EQ(sink->BufferedRecords(), 3u);
    sink->Flush();
    ASSERT_EQ(sink->BufferedRecords(), 0u);

    const auto bodies = collector.Bodies();
    ASSERT_EQ(bodies.size(), 1u);
    const auto& body = bodies.front();
    EXPECT_NE(body.find("\"resourceLogs\""), std::string::npos) << body;
    EXPECT_NE(body.find("\"service.name\""), std::string::npos) << body;
    EXPECT_NE(body.find("\"stringValue\":\"test-svc\""), std::string::npos) << body;
    EXPECT_NE(body.find("\"scope\":{\"name\":\"ulog\"}"), std::string::npos) << body;
    EXPECT_NE(body.find("\"logRecords\""), std::string::npos) << body;
    EXPECT_NE(body.find("msg 1"), std::string::npos) << body;
    EXPECT_NE(body.find("msg 2"), std::string::npos) << body;
    EXPECT_NE(body.find("msg 3"), std::string::npos) << body;
    // Each record's trailing newline must be stripped — embedded '\n'
    // would make the envelope invalid JSON.
    EXPECT_EQ(body.find("}\n,"), std::string::npos) << body;
}

TEST(OtlpBatchSink, AutoFlushOnBatchSize) {
    MiniCollector collector;
    ulog::sinks::OtlpBatchSink::Config cfg;
    cfg.endpoint = EndpointOf(collector);
    cfg.batch_size = 4;  // auto-flush every 4 records.
    auto sink = std::make_shared<ulog::sinks::OtlpBatchSink>(cfg);

    for (int i = 0; i < 10; ++i) sink->Write(MakeRecord(i));
    // 10 records / 4-batch → two auto-flushes, 2 records remain buffered.
    EXPECT_EQ(sink->BufferedRecords(), 2u);
    EXPECT_EQ(collector.PostCount(), 2);

    sink->Flush();
    EXPECT_EQ(sink->BufferedRecords(), 0u);
    EXPECT_EQ(collector.PostCount(), 3);
}

TEST(OtlpBatchSink, StatsTrackSentAndDropped) {
    MiniCollector collector;
    ulog::sinks::OtlpBatchSink::Config cfg;
    cfg.endpoint = EndpointOf(collector);
    cfg.batch_size = 0;
    auto sink = std::make_shared<ulog::sinks::OtlpBatchSink>(cfg);

    sink->Write(MakeRecord(1));
    sink->Write(MakeRecord(2));
    sink->Flush();
    {
        const auto stats = sink->GetStats();
        EXPECT_EQ(stats.writes, 2u);
        EXPECT_EQ(stats.errors, 0u);
    }

    // Flip the collector to return 500. Next flush should be counted
    // as errors.
    collector.SetResponseStatus(500);
    sink->Write(MakeRecord(3));
    sink->Write(MakeRecord(4));
    sink->Write(MakeRecord(5));
    sink->Flush();
    {
        const auto stats = sink->GetStats();
        EXPECT_EQ(stats.writes, 2u);       // unchanged
        EXPECT_EQ(stats.errors, 3u);
    }
}

TEST(OtlpBatchSink, DestructorFlushesOutstandingBuffer) {
    MiniCollector collector;
    ulog::sinks::OtlpBatchSink::Config cfg;
    cfg.endpoint = EndpointOf(collector);
    cfg.batch_size = 0;
    {
        auto sink = std::make_shared<ulog::sinks::OtlpBatchSink>(cfg);
        sink->Write(MakeRecord(1));
        // No explicit Flush — dtor should POST before returning.
    }
    EXPECT_EQ(collector.PostCount(), 1);
}

TEST(OtlpBatchSink, EmptyFlushIsNoop) {
    MiniCollector collector;
    ulog::sinks::OtlpBatchSink::Config cfg;
    cfg.endpoint = EndpointOf(collector);
    cfg.batch_size = 0;
    auto sink = std::make_shared<ulog::sinks::OtlpBatchSink>(cfg);
    sink->Flush();
    sink->Flush();
    EXPECT_EQ(collector.PostCount(), 0);
}

TEST(OtlpBatchSink, RejectsNonHttpEndpoint) {
    ulog::sinks::OtlpBatchSink::Config cfg;
    cfg.endpoint = "https://secure.example/v1/logs";
    EXPECT_THROW(ulog::sinks::OtlpBatchSink{cfg}, std::runtime_error);
}

#endif  // ULOG_HAVE_HTTP
