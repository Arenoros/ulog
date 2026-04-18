#pragma once

/// @file ulog/sinks/file_sink.hpp
/// @brief File-backed sink using std::FILE* (buffered stdio). Cross-platform.

#include <mutex>
#include <string>

#include <ulog/detail/file_handle.hpp>
#include <ulog/sinks/base_sink.hpp>

namespace ulog::sinks {

/// Appends records to a file. Thread-safe via an internal mutex. Use Reopen()
/// to rotate (close-and-reopen the same path).
class FileSink final : public BaseSink {
public:
    explicit FileSink(std::string path, bool truncate_on_start = false);

    void Write(std::string_view record) override;
    void Flush() override;
    void Reopen(ReopenMode mode) override;

private:
    std::string path_;
    std::mutex mu_;
    detail::CFileHandle file_;
};

}  // namespace ulog::sinks
