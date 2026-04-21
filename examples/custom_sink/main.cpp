// Example: write a sink that delivers records to a user-defined destination.
#include <cstdio>
#include <memory>
#include <mutex>

#include <ulog/log.hpp>
#include <ulog/sinks/base_sink.hpp>
#include <ulog/sync_logger.hpp>

namespace {

class CountingStdoutSink final : public ulog::sinks::BaseSink {
public:
    void Write(std::string_view record) override {
        std::lock_guard lock(mu_);
        ++count_;
        std::fwrite(record.data(), 1, record.size(), stdout);
    }
    std::size_t Count() const {
        std::lock_guard lock(mu_);
        return count_;
    }

private:
    mutable std::mutex mu_;
    std::size_t count_{0};
};

}  // namespace

int main() {
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);
    auto counter = std::make_shared<CountingStdoutSink>();
    logger->AddSink(counter);
    ulog::impl::SetDefaultLoggerRef(*logger);

    LOG_INFO() << "one";
    LOG_INFO() << "two";
    LOG_INFO() << "three";

    std::printf("--- emitted %zu records ---\n", counter->Count());

    ulog::SetNullDefaultLogger();
    return 0;
}
