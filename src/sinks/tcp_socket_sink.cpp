#include <ulog/sinks/tcp_socket_sink.hpp>

#include <utility>

namespace ulog::sinks {

TcpSocketSink::TcpSocketSink(std::string host, std::uint16_t port)
    : host_(std::move(host)), port_(port) {}

void TcpSocketSink::EnsureConnected() {
    if (socket_.IsOpen()) return;
    socket_.Connect(host_, port_);
}

void TcpSocketSink::Write(std::string_view record) {
    std::lock_guard lock(mu_);
    try {
        EnsureConnected();
        socket_.Send(record);
    } catch (...) {
        socket_.Close();
        throw;
    }
}

void TcpSocketSink::Reopen(ReopenMode /*mode*/) {
    std::lock_guard lock(mu_);
    socket_.Close();
}

}  // namespace ulog::sinks
