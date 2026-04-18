#include <ulog/log.hpp>
#include <ulog/sinks/fd_sink.hpp>
#include <ulog/sync_logger.hpp>

int main() {
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kDebug);
    logger->AddSink(ulog::sinks::StderrSink());
    ulog::SetDefaultLogger(logger);

    LOG_INFO() << "hello from ulog";
    LOG_WARNING() << "value=" << 42;
    LOG_DEBUG() << "debug line";

    ulog::LogFlush();
    return 0;
}
