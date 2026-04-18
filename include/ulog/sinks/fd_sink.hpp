#pragma once

/// @file ulog/sinks/fd_sink.hpp
/// @brief Sink wrapping an existing FILE* (stdout / stderr / adopted handle).

#include <cstdio>
#include <mutex>

#include <ulog/detail/file_handle.hpp>
#include <ulog/sinks/base_sink.hpp>

namespace ulog::sinks {

/// Writes to an adopted std::FILE*. Does not take ownership (destructor will
/// not close). Suitable for stdout/stderr.
class FdSink final : public BaseSink {
public:
    explicit FdSink(std::FILE* file);

    void Write(std::string_view record) override;
    void Flush() override;

private:
    std::mutex mu_;
    detail::CFileHandle file_;
};

/// Convenience factories.
std::shared_ptr<FdSink> StdoutSink();
std::shared_ptr<FdSink> StderrSink();

}  // namespace ulog::sinks
