#include <ulog/detail/file_handle.hpp>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fmt/format.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace ulog::detail {

namespace {

[[noreturn]] void ThrowErrno(const std::string& what) {
    const int err = errno;
    throw std::runtime_error(fmt::format("{}: {} (errno={})", what, std::strerror(err), err));
}

#if defined(_WIN32)

/// Opens a file on Windows with FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE
/// and hands back a std::FILE*. The DELETE share flag lets log-rotation tools
/// rename or remove the file while ulog keeps the handle open — without it,
/// `fs::rename` and `MoveFileEx` would fail with ERROR_SHARING_VIOLATION.
std::FILE* OpenShared(const std::string& path, bool append) {
    const DWORD access = append ? FILE_APPEND_DATA : GENERIC_WRITE;
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const DWORD creation = append ? OPEN_ALWAYS : CREATE_ALWAYS;

    const HANDLE h = ::CreateFileA(
        path.c_str(), access, share, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const auto err = ::GetLastError();
        throw std::runtime_error(fmt::format("CreateFileA('{}') failed: Windows error {}", path, err));
    }

    const int osfh_flags = append ? (_O_WRONLY | _O_APPEND | _O_BINARY) : (_O_WRONLY | _O_BINARY);
    const int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(h), osfh_flags);
    if (fd < 0) {
        ::CloseHandle(h);
        throw std::runtime_error(fmt::format("_open_osfhandle('{}') failed", path));
    }

    std::FILE* f = ::_fdopen(fd, append ? "ab" : "wb");
    if (!f) {
        ::_close(fd);  // closes the underlying HANDLE too
        ThrowErrno(fmt::format("_fdopen('{}') failed", path));
    }
    return f;
}

#endif  // _WIN32

std::FILE* DoFopen(const std::string& path, const char* mode) {
#if defined(_WIN32)
    const bool append = (mode && mode[0] == 'a');
    const bool write = (mode && mode[0] == 'w');
    if (append || write) return OpenShared(path, append);
    std::FILE* f = nullptr;
    if (::fopen_s(&f, path.c_str(), mode) != 0) return nullptr;
    return f;
#else
    return std::fopen(path.c_str(), mode);
#endif
}

}  // namespace

// ---------------- CFileHandle ----------------

CFileHandle::CFileHandle(const std::string& path, const char* mode) { Open(path, mode); }

CFileHandle::CFileHandle(CFileHandle&& other) noexcept : file_(other.file_), owned_(other.owned_) {
    other.file_ = nullptr;
    other.owned_ = false;
}

CFileHandle& CFileHandle::operator=(CFileHandle&& other) noexcept {
    if (this != &other) {
        Close();
        file_ = other.file_;
        owned_ = other.owned_;
        other.file_ = nullptr;
        other.owned_ = false;
    }
    return *this;
}

CFileHandle::~CFileHandle() { Close(); }

void CFileHandle::Open(const std::string& path, const char* mode) {
    Close();
    file_ = DoFopen(path, mode);
    if (!file_) ThrowErrno(fmt::format("Failed to open log file '{}'", path));
    owned_ = true;
}

void CFileHandle::Reopen(const std::string& path, const char* mode) {
    // freopen on Windows does not always handle files opened via CreateFile +
    // _open_osfhandle cleanly (and cannot re-apply FILE_SHARE_DELETE). Close
    // the current handle and reopen fresh — the expected use is log rotation,
    // where the on-disk file has already been renamed by an external tool.
    Close();
    Open(path, mode);
}

void CFileHandle::Close() noexcept {
    if (file_ && owned_) std::fclose(file_);
    file_ = nullptr;
    owned_ = false;
}

void CFileHandle::Adopt(std::FILE* file, bool owned) noexcept {
    Close();
    file_ = file;
    owned_ = owned;
}

void CFileHandle::Write(std::string_view data) {
    if (!file_) throw std::runtime_error("Write on closed file");
    const std::size_t n = std::fwrite(data.data(), 1, data.size(), file_);
    if (n != data.size()) ThrowErrno("fwrite failed");
}

void CFileHandle::Flush() {
    if (file_ && std::fflush(file_) != 0) ThrowErrno("fflush failed");
}

// ---------------- FdHandle ----------------

FdHandle::FdHandle(int fd, bool owned) noexcept : fd_(fd), owned_(owned) {}

FdHandle::FdHandle(FdHandle&& other) noexcept : fd_(other.fd_), owned_(other.owned_) {
    other.fd_ = -1;
    other.owned_ = false;
}

FdHandle& FdHandle::operator=(FdHandle&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        owned_ = other.owned_;
        other.fd_ = -1;
        other.owned_ = false;
    }
    return *this;
}

FdHandle::~FdHandle() { Close(); }

FdHandle FdHandle::OpenAppend(const std::string& path) {
#if defined(_WIN32)
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const HANDLE h = ::CreateFileA(
        path.c_str(), FILE_APPEND_DATA, share, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(
            fmt::format("Failed to open log file '{}': Windows error {}", path, ::GetLastError()));
    }
    const int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(h), _O_WRONLY | _O_APPEND | _O_BINARY);
    if (fd < 0) {
        ::CloseHandle(h);
        throw std::runtime_error(fmt::format("_open_osfhandle('{}') failed", path));
    }
    return FdHandle(fd, true);
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) ThrowErrno(fmt::format("Failed to open log file '{}'", path));
    return FdHandle(fd, true);
#endif
}

void FdHandle::Close() noexcept {
    if (fd_ >= 0 && owned_) {
#if defined(_WIN32)
        ::_close(fd_);
#else
        ::close(fd_);
#endif
    }
    fd_ = -1;
    owned_ = false;
}

void FdHandle::Write(std::string_view data) {
    if (fd_ < 0) throw std::runtime_error("Write on closed fd");
    std::size_t written = 0;
    while (written < data.size()) {
#if defined(_WIN32)
        const int n = ::_write(fd_, data.data() + written, static_cast<unsigned>(data.size() - written));
#else
        const auto n = ::write(fd_, data.data() + written, data.size() - written);
#endif
        if (n < 0) {
#if !defined(_WIN32)
            if (errno == EINTR) continue;
#endif
            ThrowErrno("write failed");
        }
        written += static_cast<std::size_t>(n);
    }
}

void FdHandle::Flush() {
#if defined(_WIN32)
    if (fd_ >= 0) ::_commit(fd_);
#else
    if (fd_ >= 0) ::fsync(fd_);
#endif
}

}  // namespace ulog::detail
