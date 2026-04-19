// AF_UNIX sink smoke test. Compiled on POSIX always; on Windows only when
// ULOG_HAVE_AFUNIX is defined (requires Win10 1803+ SDK with <afunix.h>).
#if !defined(_WIN32) || defined(ULOG_HAVE_AFUNIX)

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <afunix.h>
using sock_t = SOCKET;
#define ULOG_CLOSE_SOCK(s) ::closesocket(s)
#define ULOG_INVALID_SOCK INVALID_SOCKET
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
using sock_t = int;
#define ULOG_CLOSE_SOCK(s) ::close(s)
#define ULOG_INVALID_SOCK (-1)
#endif

#include <gtest/gtest.h>

#include <ulog/detail/socket.hpp>
#include <ulog/log.hpp>
#include <ulog/sinks/unix_socket_sink.hpp>
#include <ulog/sync_logger.hpp>

namespace fs = std::filesystem;

namespace {

/// Cross-platform AF_UNIX SOCK_STREAM listener. On POSIX uses the normal
/// BSD sockets path; on Windows opens the same AF via <afunix.h> / Winsock.
class UnixListener {
public:
    // Split-phase to keep gtest ASSERT_* off of the ctor path — those
    // macros expand to `return;` which MSVC rejects in a constructor.
    explicit UnixListener(const std::string& path) : path_(path) { Init(); }

    void Init() {
        ulog::detail::EnsureSocketSubsystem();
        // A leftover socket file from a prior crashed run would break
        // bind(). On Windows the file is created by bind too, so we
        // `remove` instead of `unlink` to stay portable.
        std::error_code ec;
        fs::remove(path_, ec);

        listen_sock_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        ASSERT_NE(listen_sock_, ULOG_INVALID_SOCK);

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        // sun_path is fixed-size (~108 on Linux, ~108 on Windows too);
        // strncpy leaves room for the terminator.
        std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);

        ASSERT_EQ(::bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        ASSERT_EQ(::listen(listen_sock_, 1), 0);

        accepter_ = std::thread([this] { AcceptAndRead(); });
    }

    ~UnixListener() {
        stop_.store(true, std::memory_order_relaxed);
        // Force-close the accepted peer so its recv() unblocks. On
        // Windows `shutdown` is reliable; on POSIX too.
        {
            std::lock_guard lock(client_mu_);
            if (client_sock_ != ULOG_INVALID_SOCK) {
                ULOG_CLOSE_SOCK(client_sock_);
                client_sock_ = ULOG_INVALID_SOCK;
            }
        }
        if (listen_sock_ != ULOG_INVALID_SOCK) ULOG_CLOSE_SOCK(listen_sock_);
        if (accepter_.joinable()) accepter_.join();
        std::error_code ec;
        fs::remove(path_, ec);
    }

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

    std::string path_;
    sock_t listen_sock_{ULOG_INVALID_SOCK};
    std::thread accepter_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mu_;
    std::string buffer_;
    mutable std::mutex client_mu_;
    sock_t client_sock_{ULOG_INVALID_SOCK};
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

#endif  // !_WIN32 || ULOG_HAVE_AFUNIX
