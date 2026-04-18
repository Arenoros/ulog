// POSIX-only: AF_UNIX sink + local listener via SOCK_STREAM unix socket.
#if !defined(_WIN32)

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <ulog/log.hpp>
#include <ulog/sinks/unix_socket_sink.hpp>
#include <ulog/sync_logger.hpp>

namespace fs = std::filesystem;

namespace {

class UnixListener {
public:
    explicit UnixListener(const std::string& path) : path_(path) {
        ::unlink(path_.c_str());
        listen_sock_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        ASSERT_GE(listen_sock_, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
        ASSERT_EQ(::bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        ASSERT_EQ(::listen(listen_sock_, 1), 0);
        accepter_ = std::thread([this] { AcceptAndRead(); });
    }

    ~UnixListener() {
        stop_.store(true, std::memory_order_relaxed);
        {
            std::lock_guard lock(client_mu_);
            if (client_sock_ >= 0) ::shutdown(client_sock_, SHUT_RDWR);
        }
        if (listen_sock_ >= 0) ::close(listen_sock_);
        if (accepter_.joinable()) accepter_.join();
        ::unlink(path_.c_str());
    }

    std::string Received() const {
        std::lock_guard lock(mu_);
        return buffer_;
    }

private:
    void AcceptAndRead() {
        const int c = ::accept(listen_sock_, nullptr, nullptr);
        if (c < 0) return;
        {
            std::lock_guard lock(client_mu_);
            client_sock_ = c;
        }
        char buf[4096];
        while (!stop_.load(std::memory_order_relaxed)) {
            const auto n = ::recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;
            std::lock_guard lock(mu_);
            buffer_.append(buf, static_cast<std::size_t>(n));
        }
        {
            std::lock_guard lock(client_mu_);
            client_sock_ = -1;
        }
        ::close(c);
    }

    std::string path_;
    int listen_sock_{-1};
    std::thread accepter_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mu_;
    std::string buffer_;
    mutable std::mutex client_mu_;
    int client_sock_{-1};
};

}  // namespace

TEST(UnixSocketSink, DeliversRecordsToListener) {
    const auto path = (fs::temp_directory_path() / "ulog_unix.sock").string();
    UnixListener listener(path);

    {
        auto sink = std::make_shared<ulog::sinks::UnixSocketSink>(path);
        auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
        logger->SetLevel(ulog::Level::kTrace);
        logger->AddSink(sink);
        ulog::SetDefaultLogger(logger);

        LOG_INFO() << "over-unix-1";
        LOG_ERROR() << "over-unix-2";
        ulog::LogFlush();

        ulog::SetDefaultLogger(nullptr);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto got = listener.Received();
    EXPECT_NE(got.find("text=over-unix-1"), std::string::npos);
    EXPECT_NE(got.find("text=over-unix-2"), std::string::npos);
}

#endif  // !_WIN32
