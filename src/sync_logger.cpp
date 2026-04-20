#include <ulog/sync_logger.hpp>

#include <utility>

#include <ulog/impl/formatters/text_item.hpp>

namespace ulog {

void SyncLogger::AddSink(sinks::SinkPtr sink) {
    if (!sink) return;
    const auto idx = RegisterSinkFormat(std::nullopt);
    std::lock_guard lock(sinks_mu_);
    sinks_.push_back({std::move(sink), idx});
}

void SyncLogger::AddSink(sinks::SinkPtr sink, Format format_override) {
    if (!sink) return;
    const auto idx = RegisterSinkFormat(format_override);
    std::lock_guard lock(sinks_mu_);
    sinks_.push_back({std::move(sink), idx});
}

void SyncLogger::Log(Level level, std::unique_ptr<impl::LoggerItemBase> item) {
    // Compat path — wraps into a single-item list and dispatches.
    if (!item) return;
    impl::LogItemList items;
    items.push_back(std::move(item));
    LogMulti(level, std::move(items));
}

void SyncLogger::LogMulti(Level level, impl::LogItemList items) {
    if (items.empty()) return;

    std::vector<SinkEntry> snapshot;
    {
        std::lock_guard lock(sinks_mu_);
        snapshot = sinks_;
    }
    for (const auto& entry : snapshot) {
        if (!entry.sink->ShouldLog(level)) continue;
        // `items[format_idx]` is the payload rendered for this sink's
        // registered format. On the single-format hot path `format_idx`
        // is 0 for every sink and we touch items[0] only.
        const std::size_t idx = entry.format_idx < items.size() ? entry.format_idx : 0;
        auto* text = static_cast<impl::formatters::TextLogItem*>(items[idx].get());
        if (!text) continue;
        try {
            entry.sink->Write(text->payload.view());
        } catch (...) {
            // Sinks must not bring down the application; swallow.
        }
    }
}

void SyncLogger::Flush() {
    std::vector<SinkEntry> snapshot;
    {
        std::lock_guard lock(sinks_mu_);
        snapshot = sinks_;
    }
    for (const auto& entry : snapshot) {
        try {
            entry.sink->Flush();
        } catch (...) {
        }
    }
}

}  // namespace ulog
