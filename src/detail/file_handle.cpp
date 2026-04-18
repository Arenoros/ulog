#include <ulog/detail/file_handle.hpp>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fmt/format.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif

namespace ulog::detail {

namespace {

[[noreturn]] void ThrowErrno(const std::string& what) {
    const int err = errno;
    throw std::runtime_error(fmt::format("{}: {} (errno={})", what, std::strerror(err), err));
}

std::FILE* DoFopen(const char* path, const char* mode) {
#if defined(_WIN32)
    std::FILE* f = nullptr;
    if (::fopen_s(&f, path, mode) != 0) return nullptr;
    return f;
#else
    return std::fopen(path, mode);
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
    file_ = DoFopen(path.c_str(), mode);
    if (!file_) ThrowErrno(fmt::format("Failed to open log file '{}'", path));
    owned_ = true;
}

void CFileHandle::Reopen(const std::string& path, const char* mode) {
    if (!file_) {
        Open(path, mode);
        return;
    }
    std::FILE* newf = std::freopen(path.c_str(), mode, file_);
    if (!newf) ThrowErrno(fmt::format("Failed to reopen log file '{}'", path));
    file_ = newf;
    owned_ = true;
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
    int fd = -1;
    const int flags = _O_WRONLY | _O_APPEND | _O_CREAT | _O_BINARY;
    if (::_sopen_s(&fd, path.c_str(), flags, _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0 || fd < 0) {
        ThrowErrno(fmt::format("Failed to open log file '{}'", path));
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
    // _write is unbuffered; nothing to flush. fsync equivalent: _commit.
    if (fd_ >= 0) ::_commit(fd_);
#else
    if (fd_ >= 0) ::fsync(fd_);
#endif
}

}  // namespace ulog::detail
