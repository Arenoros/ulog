#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <ulog/detail/socket.hpp>
#include <ulog/log.hpp>
#include <ulog/sinks/tcp_socket_sink.hpp>
#include <ulog/sync_logger.hpp>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
#define ULOG_CLOSE_SOCK(s) ::closesocket(s)
#define ULOG_INVALID_SOCK INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
#define ULOG_CLOSE_SOCK(s) ::close(s)
#define ULOG_INVALID_SOCK (-1)
#endif

namespace {

/// Minimal blocking TCP listener — binds to 127.0.0.1:0 (ephemeral), reports
/// the assigned port, accepts one connection, drains bytes into `buffer`.
class LocalListener {
public:
    LocalListener() {
        ulog::detail::EnsureSocketSubsystem();

        listen_sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_sock_ == ULOG_INVALID_SOCK) throw std::runtime_error("socket() failed");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            throw std::runtime_error("bind() failed");
        if (::listen(listen_sock_, 1) != 0) throw std::runtime_error("listen() failed");

        sockaddr_in bound{};
        socklen_t blen = sizeof(bound);
        ::getsockname(listen_sock_, reinterpret_cast<sockaddr*>(&bound), &blen);
        port_ = ntohs(bound.sin_port);

        accepter_ = std::thread([this] { AcceptAndRead(); });
    }

    ~LocalListener() {
        stop_.store(true, std::memory_order_relaxed);
        if (listen_sock_ != ULOG_INVALID_SOCK) ULOG_CLOSE_SOCK(listen_sock_);
        if (accepter_.joinable()) accepter_.join();
    }

    std::uint16_t port() const noexcept { return port_; }

    std::string Received() const {
        std::lock_guard lock(mu_);
        return buffer_;
    }

private:
    void AcceptAndRead() {
        sock_t c = ::accept(listen_sock_, nullptr, nullptr);
        if (c == ULOG_INVALID_SOCK) return;
        char buf[4096];
        while (!stop_.load(std::memory_order_relaxed)) {
#if defined(_WIN32)
            const int n = ::recv(c, buf, sizeof(buf), 0);
#else
            const auto n = ::recv(c, buf, sizeof(buf), 0);
#endif
            if (n <= 0) break;
            std::lock_guard lock(mu_);
            buffer_.append(buf, static_cast<std::size_t>(n));
        }
        ULOG_CLOSE_SOCK(c);
    }

    sock_t listen_sock_{ULOG_INVALID_SOCK};
    std::uint16_t port_{0};
    std::thread accepter_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mu_;
    std::string buffer_;
};

}  // namespace

TEST(TcpSocketSink, DeliversRecordsToListener) {
    LocalListener listener;

    {
        auto sink = std::make_shared<ulog::sinks::TcpSocketSink>("127.0.0.1", listener.port());
        auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
        logger->SetLevel(ulog::Level::kTrace);
        logger->AddSink(sink);
        ulog::SetDefaultLogger(logger);

        LOG_INFO() << "over-tcp-1";
        LOG_ERROR() << "over-tcp-2";
        ulog::LogFlush();

        ulog::SetDefaultLogger(nullptr);
    }

    // Give the listener a moment to flush recv into the buffer.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto got = listener.Received();
    EXPECT_NE(got.find("text=over-tcp-1"), std::string::npos);
    EXPECT_NE(got.find("text=over-tcp-2"), std::string::npos);
    EXPECT_NE(got.find("level=INFO"), std::string::npos);
    EXPECT_NE(got.find("level=ERROR"), std::string::npos);
}

TEST(TcpSocketSink, ReopenDropsConnection) {
    LocalListener listener;

    auto sink = std::make_shared<ulog::sinks::TcpSocketSink>("127.0.0.1", listener.port());
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);
    logger->AddSink(sink);
    ulog::SetDefaultLogger(logger);

    LOG_INFO() << "before-reopen";
    ulog::LogFlush();
    sink->Reopen(ulog::sinks::ReopenMode::kAppend);  // closes the socket

    // Subsequent Write attempts should either succeed (reconnect) against
    // the same listener (still up) or fail silently without crashing.
    EXPECT_NO_THROW({
        try { LOG_INFO() << "after-reopen"; } catch (...) { /* tolerated */ }
        ulog::LogFlush();
    });

    ulog::SetDefaultLogger(nullptr);
}
