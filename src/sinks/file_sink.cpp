#include <ulog/sinks/file_sink.hpp>

namespace ulog::sinks {

FileSink::FileSink(std::string path, bool truncate_on_start) : path_(std::move(path)) {
    file_.Open(path_, truncate_on_start ? "wb" : "ab");
}

void FileSink::Write(std::string_view record) {
    std::lock_guard lock(mu_);
    file_.Write(record);
}

void FileSink::Flush() {
    std::lock_guard lock(mu_);
    file_.Flush();
}

void FileSink::Reopen(ReopenMode mode) {
    std::lock_guard lock(mu_);
    file_.Reopen(path_, mode == ReopenMode::kTruncate ? "wb" : "ab");
}

}  // namespace ulog::sinks
