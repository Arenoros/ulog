#pragma once

/// @file ulog/detail/socket.hpp
/// @brief Cross-platform (POSIX / Winsock2) blocking socket wrapper for sinks.

#include <cstdint>
#include <string>
#include <string_view>

namespace ulog::detail {

#if defined(_WIN32)
using SocketHandle = std::uintptr_t;  // SOCKET is UINT_PTR on Windows
inline constexpr SocketHandle kInvalidSocket = static_cast<SocketHandle>(~static_cast<std::uintptr_t>(0));
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

/// Initializes Winsock on Windows (no-op on POSIX). Safe to call multiple times;
/// cleanup happens at program exit via a static guard.
void EnsureSocketSubsystem();

/// Cross-platform blocking TCP client socket. Reconnects on demand.
class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket();
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    void Connect(std::string_view host, std::uint16_t port);
    void Close() noexcept;
    bool IsOpen() const noexcept { return sock_ != kInvalidSocket; }

    /// Blocking send; throws on error. Returns number of bytes written (always == data.size() on success).
    std::size_t Send(std::string_view data);

private:
    SocketHandle sock_{kInvalidSocket};
};

#if !defined(_WIN32) || (defined(_WIN32) && defined(ULOG_HAVE_AFUNIX))
/// Cross-platform Unix-domain client socket.
/// Available on POSIX always and on Windows 10 (1803+) when ULOG_HAVE_AFUNIX is defined.
class UnixSocket {
public:
    UnixSocket() = default;
    ~UnixSocket();
    UnixSocket(const UnixSocket&) = delete;
    UnixSocket& operator=(const UnixSocket&) = delete;
    UnixSocket(UnixSocket&& other) noexcept;
    UnixSocket& operator=(UnixSocket&& other) noexcept;

    void Connect(const std::string& path);
    void Close() noexcept;
    bool IsOpen() const noexcept { return sock_ != kInvalidSocket; }

    std::size_t Send(std::string_view data);

private:
    SocketHandle sock_{kInvalidSocket};
};
#endif

}  // namespace ulog::detail
