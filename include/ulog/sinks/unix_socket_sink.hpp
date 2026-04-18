#pragma once

/// @file ulog/sinks/unix_socket_sink.hpp
/// @brief Sink that ships records over an AF_UNIX socket.
///
/// Available on POSIX always; on Windows only when ULOG_HAVE_AFUNIX is
/// defined (requires Windows 10 1803+ with afunix.h).

#if !defined(_WIN32) || defined(ULOG_HAVE_AFUNIX)

#include <mutex>
#include <string>

#include <ulog/detail/socket.hpp>
#include <ulog/sinks/base_sink.hpp>

namespace ulog::sinks {

class UnixSocketSink final : public BaseSink {
public:
    explicit UnixSocketSink(std::string path);

    void Write(std::string_view record) override;
    void Flush() override {}
    void Reopen(ReopenMode mode) override;

private:
    std::string path_;
    std::mutex mu_;
    detail::UnixSocket socket_;

    void EnsureConnected();
};

}  // namespace ulog::sinks

#endif  // !_WIN32 || ULOG_HAVE_AFUNIX
