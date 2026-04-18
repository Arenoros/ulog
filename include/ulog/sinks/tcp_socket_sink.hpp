#pragma once

/// @file ulog/sinks/tcp_socket_sink.hpp
/// @brief Sink that ships records over a TCP connection.

#include <cstdint>
#include <mutex>
#include <string>

#include <ulog/detail/socket.hpp>
#include <ulog/sinks/base_sink.hpp>

namespace ulog::sinks {

class TcpSocketSink final : public BaseSink {
public:
    TcpSocketSink(std::string host, std::uint16_t port);

    void Write(std::string_view record) override;
    void Flush() override {}

    /// Reopens the TCP connection (e.g. after a peer restart).
    void Reopen(ReopenMode mode) override;

private:
    std::string host_;
    std::uint16_t port_;
    std::mutex mu_;
    detail::TcpSocket socket_;

    void EnsureConnected();
};

}  // namespace ulog::sinks
