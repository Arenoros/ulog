#include <ulog/sinks/fd_sink.hpp>

namespace ulog::sinks {

FdSink::FdSink(std::FILE* file) {
    file_.Adopt(file, /*owned=*/false);
}

void FdSink::Write(std::string_view record) {
    std::lock_guard lock(mu_);
    file_.Write(record);
}

void FdSink::Flush() {
    std::lock_guard lock(mu_);
    file_.Flush();
}

std::shared_ptr<FdSink> StdoutSink() { return std::make_shared<FdSink>(stdout); }
std::shared_ptr<FdSink> StderrSink() { return std::make_shared<FdSink>(stderr); }

}  // namespace ulog::sinks
