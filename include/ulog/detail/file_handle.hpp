#pragma once

/// @file ulog/detail/file_handle.hpp
/// @brief Cross-platform RAII file descriptor / FILE* wrappers for sinks.

#include <cstdio>
#include <string>
#include <string_view>

namespace ulog::detail {

/// RAII wrapper around ::FILE*. Uses fopen/fwrite/fflush/fclose for portability.
/// Supports reopen (log rotation via freopen).
class CFileHandle {
public:
    CFileHandle() = default;
    explicit CFileHandle(const std::string& path, const char* mode = "ab");
    CFileHandle(CFileHandle&& other) noexcept;
    CFileHandle& operator=(CFileHandle&& other) noexcept;
    ~CFileHandle();

    CFileHandle(const CFileHandle&) = delete;
    CFileHandle& operator=(const CFileHandle&) = delete;

    void Open(const std::string& path, const char* mode = "ab");
    void Reopen(const std::string& path, const char* mode = "ab");
    void Close() noexcept;

    /// Adopts an existing FILE* (e.g. stdout/stderr). Does NOT take ownership
    /// (destructor will not close it) when `owned = false`.
    void Adopt(std::FILE* file, bool owned) noexcept;

    void Write(std::string_view data);
    void Flush();

    bool IsOpen() const noexcept { return file_ != nullptr; }
    std::FILE* Raw() const noexcept { return file_; }

private:
    std::FILE* file_{nullptr};
    bool owned_{false};
};

/// Low-level file descriptor wrapper (POSIX `open`/`write` or Windows `_open`/`_write`).
/// Used where a raw fd is needed (e.g. unowned stdout FD).
class FdHandle {
public:
    FdHandle() = default;
    explicit FdHandle(int fd, bool owned) noexcept;
    FdHandle(FdHandle&& other) noexcept;
    FdHandle& operator=(FdHandle&& other) noexcept;
    ~FdHandle();

    FdHandle(const FdHandle&) = delete;
    FdHandle& operator=(const FdHandle&) = delete;

    static FdHandle OpenAppend(const std::string& path);

    void Close() noexcept;
    void Write(std::string_view data);
    void Flush();

    bool IsOpen() const noexcept { return fd_ >= 0; }
    int Raw() const noexcept { return fd_; }

private:
    int fd_{-1};
    bool owned_{false};
};

}  // namespace ulog::detail
