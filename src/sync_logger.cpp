#include <ulog/sync_logger.hpp>

#include <utility>

#include <ulog/impl/formatters/text_item.hpp>

namespace ulog {

void SyncLogger::AddSink(sinks::SinkPtr sink) {
    if (!sink) return;
    std::lock_guard lock(sinks_mu_);
    sinks_.push_back(std::move(sink));
}

void SyncLogger::Log(Level level, impl::LoggerItemRef item) {
    auto& text_item = static_cast<impl::formatters::TextLogItem&>(item);
    const auto view = text_item.payload.view();

    std::vector<sinks::SinkPtr> snapshot;
    {
        std::lock_guard lock(sinks_mu_);
        snapshot = sinks_;
    }
    for (const auto& s : snapshot) {
        if (!s->ShouldLog(level)) continue;
        try {
            s->Write(view);
        } catch (...) {
            // Sinks must not bring down the application; swallow.
        }
    }
}

void SyncLogger::Flush() {
    std::vector<sinks::SinkPtr> snapshot;
    {
        std::lock_guard lock(sinks_mu_);
        snapshot = sinks_;
    }
    for (const auto& s : snapshot) {
        try {
            s->Flush();
        } catch (...) {
        }
    }
}

}  // namespace ulog
