#include <ulog/sinks/unix_socket_sink.hpp>

#if !defined(_WIN32) || defined(ULOG_HAVE_AFUNIX)

#include <utility>

namespace ulog::sinks {

UnixSocketSink::UnixSocketSink(std::string path) : path_(std::move(path)) {}

void UnixSocketSink::EnsureConnected() {
    if (socket_.IsOpen()) return;
    socket_.Connect(path_);
}

void UnixSocketSink::Write(std::string_view record) {
    std::lock_guard lock(mu_);
    try {
        EnsureConnected();
        socket_.Send(record);
    } catch (...) {
        socket_.Close();
        throw;
    }
}

void UnixSocketSink::Reopen(ReopenMode /*mode*/) {
    std::lock_guard lock(mu_);
    socket_.Close();
}

}  // namespace ulog::sinks

#endif  // !_WIN32 || ULOG_HAVE_AFUNIX
