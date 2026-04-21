// Example: asynchronous file logger with rotation-friendly configuration.
#include <ulog/config.hpp>
#include <ulog/log.hpp>

int main() {
    ulog::LoggerConfig cfg;
    cfg.file_path = "async_example.log";
    cfg.format = ulog::Format::kJson;
    cfg.level = ulog::Level::kDebug;
    cfg.queue_capacity = 8192;
    cfg.overflow = ulog::OverflowBehavior::kBlock;
    auto logger = ulog::InitDefaultLogger(cfg);

    for (int i = 0; i < 10; ++i) {
        LOG_INFO() << "tick " << i << ulog::LogExtra{{"iter", i}};
    }

    ulog::LogFlush();
    ulog::SetNullDefaultLogger();
    return 0;
}
