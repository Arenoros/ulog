#include <ulog/detail/socket.hpp>

#include <cerrno>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>

#include <fmt/format.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#if defined(ULOG_HAVE_AFUNIX)
#include <afunix.h>
#endif
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace ulog::detail {

namespace {

[[noreturn]] void ThrowSocketError(const std::string& what) {
#if defined(_WIN32)
    const int err = ::WSAGetLastError();
    throw std::runtime_error(fmt::format("{}: WSA error {}", what, err));
#else
    const int err = errno;
    throw std::runtime_error(fmt::format("{}: {} (errno={})", what, std::strerror(err), err));
#endif
}

void CloseSocketHandle(SocketHandle s) noexcept {
    if (s == kInvalidSocket) return;
#if defined(_WIN32)
    ::closesocket(s);
#else
    ::close(s);
#endif
}

#if defined(_WIN32)
class WinsockGuard {
public:
    WinsockGuard() {
        WSADATA d;
        ::WSAStartup(MAKEWORD(2, 2), &d);
    }
    ~WinsockGuard() { ::WSACleanup(); }
};
#endif

}  // namespace

void EnsureSocketSubsystem() {
#if defined(_WIN32)
    static WinsockGuard guard;
    (void)guard;
#endif
}

// ---------------- TcpSocket ----------------

TcpSocket::~TcpSocket() { Close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : sock_(other.sock_) { other.sock_ = kInvalidSocket; }

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        Close();
        sock_ = other.sock_;
        other.sock_ = kInvalidSocket;
    }
    return *this;
}

void TcpSocket::Connect(std::string_view host, std::uint16_t port) {
    EnsureSocketSubsystem();
    Close();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    const std::string host_s(host);
    const std::string port_s = std::to_string(port);
    const int err = ::getaddrinfo(host_s.c_str(), port_s.c_str(), &hints, &res);
    if (err != 0 || !res) {
        throw std::runtime_error(fmt::format("getaddrinfo({}:{}) failed: {}", host_s, port, err));
    }

    SocketHandle s = kInvalidSocket;
    for (addrinfo* p = res; p; p = p->ai_next) {
#if defined(_WIN32)
        const SOCKET handle = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (handle == INVALID_SOCKET) continue;
        s = static_cast<SocketHandle>(handle);
#else
        s = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s < 0) continue;
#endif
        if (::connect(
#if defined(_WIN32)
                static_cast<SOCKET>(s),
#else
                s,
#endif
                p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) {
            break;
        }
        CloseSocketHandle(s);
        s = kInvalidSocket;
    }
    ::freeaddrinfo(res);

    if (s == kInvalidSocket) ThrowSocketError(fmt::format("Failed to connect to {}:{}", host_s, port));
    sock_ = s;
}

void TcpSocket::Close() noexcept {
    CloseSocketHandle(sock_);
    sock_ = kInvalidSocket;
}

std::size_t TcpSocket::Send(std::string_view data) {
    if (sock_ == kInvalidSocket) throw std::runtime_error("Send on closed TCP socket");
    std::size_t sent = 0;
    while (sent < data.size()) {
#if defined(_WIN32)
        const int n = ::send(static_cast<SOCKET>(sock_), data.data() + sent,
                             static_cast<int>(data.size() - sent), 0);
#else
        const auto n = ::send(sock_, data.data() + sent, data.size() - sent, 0);
#endif
        if (n < 0) {
#if !defined(_WIN32)
            if (errno == EINTR) continue;
#endif
            ThrowSocketError("TCP send failed");
        }
        sent += static_cast<std::size_t>(n);
    }
    return sent;
}

// ---------------- UnixSocket ----------------

#if !defined(_WIN32) || defined(ULOG_HAVE_AFUNIX)

UnixSocket::~UnixSocket() { Close(); }

UnixSocket::UnixSocket(UnixSocket&& other) noexcept : sock_(other.sock_) { other.sock_ = kInvalidSocket; }

UnixSocket& UnixSocket::operator=(UnixSocket&& other) noexcept {
    if (this != &other) {
        Close();
        sock_ = other.sock_;
        other.sock_ = kInvalidSocket;
    }
    return *this;
}

void UnixSocket::Connect(const std::string& path) {
    EnsureSocketSubsystem();
    Close();

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() + 1 > sizeof(addr.sun_path)) {
        throw std::runtime_error(fmt::format("Unix socket path too long: '{}'", path));
    }
    std::memcpy(addr.sun_path, path.data(), path.size());

#if defined(_WIN32)
    const SOCKET handle = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (handle == INVALID_SOCKET) ThrowSocketError("AF_UNIX socket() failed");
    const SocketHandle s = static_cast<SocketHandle>(handle);
    if (::connect(static_cast<SOCKET>(s), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        CloseSocketHandle(s);
        ThrowSocketError(fmt::format("Unix connect to '{}' failed", path));
    }
#else
    const int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) ThrowSocketError("AF_UNIX socket() failed");
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        CloseSocketHandle(s);
        ThrowSocketError(fmt::format("Unix connect to '{}' failed", path));
    }
#endif
    sock_ = s;
}

void UnixSocket::Close() noexcept {
    CloseSocketHandle(sock_);
    sock_ = kInvalidSocket;
}

std::size_t UnixSocket::Send(std::string_view data) {
    if (sock_ == kInvalidSocket) throw std::runtime_error("Send on closed unix socket");
    std::size_t sent = 0;
    while (sent < data.size()) {
#if defined(_WIN32)
        const int n = ::send(static_cast<SOCKET>(sock_), data.data() + sent,
                             static_cast<int>(data.size() - sent), 0);
#else
        const auto n = ::send(sock_, data.data() + sent, data.size() - sent, 0);
#endif
        if (n < 0) {
#if !defined(_WIN32)
            if (errno == EINTR) continue;
#endif
            ThrowSocketError("Unix send failed");
        }
        sent += static_cast<std::size_t>(n);
    }
    return sent;
}

#endif  // !_WIN32 || ULOG_HAVE_AFUNIX

}  // namespace ulog::detail
