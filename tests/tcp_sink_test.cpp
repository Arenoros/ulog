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

#include "ulog/null_logger.hpp"

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
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
#define ULOG_CLOSE_SOCK(s) ::close(s)
#define ULOG_INVALID_SOCK (-1)
#endif

namespace {

/// Makes the reader thread's `recv()` return with EAGAIN every `ms`
/// so it can notice a `stop_` flip. Avoids having to wake a blocked
/// recv() from another thread (close()/shutdown() both have subtle
/// race + wake-up issues across Linux and Windows).
inline void SetRecvTimeoutMs(sock_t s, int ms) {
#if defined(_WIN32)
    DWORD t = static_cast<DWORD>(ms);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&t), sizeof(t));
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

/// Classifies a recv() return value + errno/WSAGetLastError into
/// {data, timeout, done}. The SO_RCVTIMEO variant uses EAGAIN on POSIX
/// and WSAETIMEDOUT on Windows; anything else ends the loop.
enum class RecvOutcome { kData, kTimeout, kDone };
inline RecvOutcome ClassifyRecv(int n) {
    if (n > 0) return RecvOutcome::kData;
    if (n == 0) return RecvOutcome::kDone;
#if defined(_WIN32)
    const int err = ::WSAGetLastError();
    if (err == WSAETIMEDOUT || err == WSAEINTR) return RecvOutcome::kTimeout;
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return RecvOutcome::kTimeout;
#endif
    return RecvOutcome::kDone;
}

/// Polls one socket for readability with a timeout. Used instead of
/// mutating `listen_sock_` from the dtor to wake a blocked accept() —
/// TSan flags that write/read pair on the fd variable. Here the
/// accepter spins on poll(listen_sock_, 100ms) and checks `stop_`
/// between timeouts, so the dtor only needs to flip the flag.
/// Returns >0 on ready, 0 on timeout, <0 on error.
inline int PollOneReadable(sock_t s, int timeout_ms) {
#if defined(_WIN32)
    WSAPOLLFD pfd{};
    pfd.fd = s;
    pfd.events = POLLRDNORM;
    return ::WSAPoll(&pfd, 1, timeout_ms);
#else
    struct pollfd pfd{};
    pfd.fd = s;
    pfd.events = POLLIN;
    return ::poll(&pfd, 1, timeout_ms);
#endif
}

/// Minimal TCP listener — binds to 127.0.0.1:0 (ephemeral), reports
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
        // Accepter spins on poll(listen_sock_, 100ms) and the reader
        // part uses SO_RCVTIMEO; both consult `stop_` between waits.
        // The dtor only flips the flag + joins, then closes the
        // listen socket AFTER the accepter is gone. No cross-thread
        // fd mutation → no TSan race on listen_sock_.
        stop_.store(true, std::memory_order_relaxed);
        if (accepter_.joinable()) accepter_.join();
        if (listen_sock_ != ULOG_INVALID_SOCK) {
            ULOG_CLOSE_SOCK(listen_sock_);
            listen_sock_ = ULOG_INVALID_SOCK;
        }
    }

    std::uint16_t port() const noexcept { return port_; }

    std::string Received() const {
        std::lock_guard lock(mu_);
        return buffer_;
    }

private:
    void AcceptAndRead() {
        // Wait for a pending accept with a 100ms timeout so the thread
        // can notice a `stop_` flip without help from the dtor.
        sock_t c = ULOG_INVALID_SOCK;
        while (!stop_.load(std::memory_order_relaxed)) {
            const int r = PollOneReadable(listen_sock_, 100);
            if (r < 0) return;
            if (r == 0) continue;
            c = ::accept(listen_sock_, nullptr, nullptr);
            if (c == ULOG_INVALID_SOCK) return;
            break;
        }
        if (c == ULOG_INVALID_SOCK) return;
        SetRecvTimeoutMs(c, 100);
        char buf[4096];
        while (!stop_.load(std::memory_order_relaxed)) {
#if defined(_WIN32)
            const int n = ::recv(c, buf, sizeof(buf), 0);
#else
            const int n = static_cast<int>(::recv(c, buf, sizeof(buf), 0));
#endif
            const auto outcome = ClassifyRecv(n);
            if (outcome == RecvOutcome::kDone) break;
            if (outcome == RecvOutcome::kTimeout) continue;
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
        // Accepter spins on poll(listen_sock_, 100ms); readers use
        // SO_RCVTIMEO. Both consult `stop_` between waits. Dtor only
        // flips the flag, joins, and closes the listen socket AFTER
        // all threads are gone — no cross-thread fd mutation.
        stop_.store(true, std::memory_order_relaxed);
        if (accepter_.joinable()) accepter_.join();

        std::vector<std::thread> readers;
        {
            std::lock_guard lock(readers_mu_);
            readers = std::move(readers_);
        }
        for (auto& t : readers) if (t.joinable()) t.join();

        if (listen_sock_ != ULOG_INVALID_SOCK) {
            ULOG_CLOSE_SOCK(listen_sock_);
            listen_sock_ = ULOG_INVALID_SOCK;
        }
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
            const int r = PollOneReadable(listen_sock_, 100);
            if (r < 0) return;
            if (r == 0) continue;
            sock_t c = ::accept(listen_sock_, nullptr, nullptr);
            if (c == ULOG_INVALID_SOCK) return;
            accepted_.fetch_add(1, std::memory_order_relaxed);
            std::thread reader([this, c] { ReadLoop(c); });
            std::lock_guard lock(readers_mu_);
            readers_.push_back(std::move(reader));
        }
    }

    void ReadLoop(sock_t c) {
        SetRecvTimeoutMs(c, 100);
        char buf[4096];
        while (!stop_.load(std::memory_order_relaxed)) {
#if defined(_WIN32)
            const int n = ::recv(c, buf, sizeof(buf), 0);
#else
            const int n = static_cast<int>(::recv(c, buf, sizeof(buf), 0));
#endif
            const auto outcome = ClassifyRecv(n);
            if (outcome == RecvOutcome::kDone) break;
            if (outcome == RecvOutcome::kTimeout) continue;
            std::lock_guard lock(buf_mu_);
            buffer_.append(buf, static_cast<std::size_t>(n));
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
};

}  // namespace

TEST(TcpSocketSink, DeliversRecordsToListener) {
    LocalListener listener;

    {
        auto sink = std::make_shared<ulog::sinks::TcpSocketSink>("127.0.0.1", listener.port());
        auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
        logger->SetLevel(ulog::Level::kTrace);
        logger->AddSink(sink);
        ulog::impl::SetDefaultLoggerRef(*logger);

        LOG_INFO() << "over-tcp-1";
        LOG_ERROR() << "over-tcp-2";
        ulog::LogFlush();

        ulog::SetNullDefaultLogger();
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
    ulog::impl::SetDefaultLoggerRef(*logger);

    LOG_INFO() << "before-reopen";
    ulog::LogFlush();
    // Give the listener a moment to accept + read the first message.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    sink->Reopen(ulog::sinks::ReopenMode::kAppend);

    LOG_INFO() << "after-reopen";
    ulog::LogFlush();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ulog::SetNullDefaultLogger();

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
    ulog::impl::SetDefaultLoggerRef(*logger);

    LOG_INFO() << "before-reopen";
    ulog::LogFlush();
    EXPECT_NO_THROW(sink->Reopen(ulog::sinks::ReopenMode::kAppend));

    ulog::SetNullDefaultLogger();
}
