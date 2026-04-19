#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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
        // Close the accepted peer directly — on Windows `shutdown` alone
        // reliably takes minutes to unblock `recv`, while closesocket drops
        // the FD right away and recv returns WSAENOTSOCK / 0.
        {
            std::lock_guard lock(client_mu_);
            if (client_sock_ != ULOG_INVALID_SOCK) {
                ULOG_CLOSE_SOCK(client_sock_);
                client_sock_ = ULOG_INVALID_SOCK;
            }
        }
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
        {
            std::lock_guard lock(client_mu_);
            client_sock_ = c;
        }
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
        {
            std::lock_guard lock(client_mu_);
            client_sock_ = ULOG_INVALID_SOCK;
        }
        ULOG_CLOSE_SOCK(c);
    }

    sock_t listen_sock_{ULOG_INVALID_SOCK};
    std::uint16_t port_{0};
    std::thread accepter_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mu_;
    std::string buffer_;
    mutable std::mutex client_mu_;
    sock_t client_sock_{ULOG_INVALID_SOCK};
};

/// Multi-accept variant: loops on `accept()`, spawning one reader per
/// accepted peer. The consolidated `buffer_` keeps a transcript across
/// connections so tests can assert "messages A, B, C all arrived"
/// regardless of which connection carried each.
class MultiAcceptListener {
public:
    MultiAcceptListener() {
        ulog::detail::EnsureSocketSubsystem();

        listen_sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_sock_ == ULOG_INVALID_SOCK) throw std::runtime_error("socket() failed");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            throw std::runtime_error("bind() failed");
        if (::listen(listen_sock_, 4) != 0) throw std::runtime_error("listen() failed");

        sockaddr_in bound{};
        socklen_t blen = sizeof(bound);
        ::getsockname(listen_sock_, reinterpret_cast<sockaddr*>(&bound), &blen);
        port_ = ntohs(bound.sin_port);

        accepter_ = std::thread([this] { AcceptLoop(); });
    }

    ~MultiAcceptListener() {
        stop_.store(true, std::memory_order_relaxed);
        // Close the listen socket → blocked accept() returns.
        if (listen_sock_ != ULOG_INVALID_SOCK) {
            ULOG_CLOSE_SOCK(listen_sock_);
            listen_sock_ = ULOG_INVALID_SOCK;
        }
        if (accepter_.joinable()) accepter_.join();

        // Also close every accepted peer socket. If the test left the
        // sink alive (logger still holding the shared_ptr), reader
        // threads would be stuck inside `recv()`; forcibly closing the
        // fd here makes them drop out.
        {
            std::lock_guard lock(clients_mu_);
            for (auto s : client_socks_) {
                if (s != ULOG_INVALID_SOCK) ULOG_CLOSE_SOCK(s);
            }
            client_socks_.clear();
        }

        std::vector<std::thread> readers;
        {
            std::lock_guard lock(readers_mu_);
            readers = std::move(readers_);
        }
        for (auto& t : readers) if (t.joinable()) t.join();
    }

    std::uint16_t port() const noexcept { return port_; }

    std::string Received() const {
        std::lock_guard lock(buf_mu_);
        return buffer_;
    }

    int AcceptedConnections() const noexcept {
        return accepted_.load(std::memory_order_relaxed);
    }

private:
    void AcceptLoop() {
        while (!stop_.load(std::memory_order_relaxed)) {
            sock_t c = ::accept(listen_sock_, nullptr, nullptr);
            if (c == ULOG_INVALID_SOCK) return;  // listen socket closed.
            accepted_.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard lock(clients_mu_);
                client_socks_.push_back(c);
            }
            std::thread reader([this, c] { ReadLoop(c); });
            std::lock_guard lock(readers_mu_);
            readers_.push_back(std::move(reader));
        }
    }

    void ReadLoop(sock_t c) {
        char buf[4096];
        while (!stop_.load(std::memory_order_relaxed)) {
#if defined(_WIN32)
            const int n = ::recv(c, buf, sizeof(buf), 0);
#else
            const auto n = ::recv(c, buf, sizeof(buf), 0);
#endif
            if (n <= 0) break;
            std::lock_guard lock(buf_mu_);
            buffer_.append(buf, static_cast<std::size_t>(n));
        }
        // Mark this socket as no longer held by the reader — the dtor
        // must not double-close it.
        {
            std::lock_guard lock(clients_mu_);
            for (auto& s : client_socks_) {
                if (s == c) { s = ULOG_INVALID_SOCK; break; }
            }
        }
        ULOG_CLOSE_SOCK(c);
    }

    sock_t listen_sock_{ULOG_INVALID_SOCK};
    std::uint16_t port_{0};
    std::thread accepter_;
    std::atomic<bool> stop_{false};
    std::atomic<int> accepted_{0};
    mutable std::mutex buf_mu_;
    std::string buffer_;
    std::mutex readers_mu_;
    std::vector<std::thread> readers_;
    std::mutex clients_mu_;
    std::vector<sock_t> client_socks_;
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

TEST(TcpSocketSink, ReopenReconnectsToListener) {
    // Multi-accept variant of the single-shot ReopenClosesSocketWithoutCrash
    // case below: verify that Reopen(kAppend) followed by a subsequent
    // Write actually re-establishes the TCP connection and delivers the
    // post-reopen record. Listener counts accept() hits so the test
    // catches silent "reconnect never happened" regressions.
    MultiAcceptListener listener;

    auto sink = std::make_shared<ulog::sinks::TcpSocketSink>("127.0.0.1", listener.port());
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);
    logger->AddSink(sink);
    ulog::SetDefaultLogger(logger);

    LOG_INFO() << "before-reopen";
    ulog::LogFlush();
    // Give the listener a moment to accept + read the first message.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    sink->Reopen(ulog::sinks::ReopenMode::kAppend);

    LOG_INFO() << "after-reopen";
    ulog::LogFlush();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ulog::SetDefaultLogger(nullptr);

    const auto got = listener.Received();
    EXPECT_NE(got.find("text=before-reopen"), std::string::npos) << got;
    EXPECT_NE(got.find("text=after-reopen"), std::string::npos) << got;
    // Two separate TCP connections — one pre-Reopen, one post.
    EXPECT_EQ(listener.AcceptedConnections(), 2) << "accepted=" << listener.AcceptedConnections();
}

TEST(TcpSocketSink, ReopenClosesSocketWithoutCrash) {
    // Contract check — `Reopen` on a TCP sink tears down the current
    // connection. We don't attempt a subsequent Write here because the
    // one-shot listener above only accepts a single peer; a second
    // connect would block waiting for a second accept.
    LocalListener listener;

    auto sink = std::make_shared<ulog::sinks::TcpSocketSink>("127.0.0.1", listener.port());
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);
    logger->AddSink(sink);
    ulog::SetDefaultLogger(logger);

    LOG_INFO() << "before-reopen";
    ulog::LogFlush();
    EXPECT_NO_THROW(sink->Reopen(ulog::sinks::ReopenMode::kAppend));

    ulog::SetDefaultLogger(nullptr);
}
