#include <ulog/sync_logger.hpp>

#include <utility>

#include <boost/make_shared.hpp>

#include <ulog/impl/formatters/text_item.hpp>

namespace ulog {

namespace {

/// Publishes a fresh vector that differs from `current` by one appended
/// entry. Writers of COW lists call this under their serializing mutex
/// so parallel `AddSink` calls don't lose updates.
template <typename Vec, typename Entry>
boost::shared_ptr<Vec const> AppendCow(
        const boost::shared_ptr<Vec const>& current,
        Entry&& entry) {
    auto next = boost::make_shared<Vec>();
    if (current) {
        next->reserve(current->size() + 1);
        *next = *current;
    }
    next->push_back(std::forward<Entry>(entry));
    return boost::shared_ptr<Vec const>(std::move(next));
}

}  // namespace

void SyncLogger::AddSink(sinks::SinkPtr sink) {
    if (!sink) return;
    const auto idx = RegisterSinkFormat(std::nullopt);
    std::lock_guard lock(sinks_mu_);
    auto next = AppendCow<SinkVec>(sinks_.load(), SinkEntry{std::move(sink), idx});
    sinks_.store(std::move(next));
    SetHasTextSinks(true);
}

void SyncLogger::AddSink(sinks::SinkPtr sink, Format format_override) {
    if (!sink) return;
    const auto idx = RegisterSinkFormat(format_override);
    std::lock_guard lock(sinks_mu_);
    auto next = AppendCow<SinkVec>(sinks_.load(), SinkEntry{std::move(sink), idx});
    sinks_.store(std::move(next));
    SetHasTextSinks(true);
}

void SyncLogger::AddStructuredSink(sinks::StructuredSinkPtr sink) {
    if (!sink) return;
    std::lock_guard lock(struct_sinks_mu_);
    auto next = AppendCow<StructSinkVec>(struct_sinks_.load(), std::move(sink));
    struct_sinks_.store(std::move(next));
    SetHasStructuredSinks(true);
}

void SyncLogger::Log(Level level, std::unique_ptr<impl::LoggerItemBase> item) {
    if (!item) return;
    // Lock-free atomic snapshot of the sink list. Sinks added mid-call
    // publish a new vector; this thread keeps iterating the pinned
    // snapshot it captured on entry.
    auto snap = sinks_.load();
    if (!snap || snap->empty()) return;
    auto& text_item = static_cast<impl::formatters::TextLogItem&>(*item);
    const auto view = text_item.payload.view();
    for (const auto& entry : *snap) {
        // Sinks registered for a non-primary format need the per-sink
        // rendered payload; a single-item Log() call cannot supply one,
        // so skip silently (LogMulti handles the full routing).
        if (entry.format_idx != 0) continue;
        if (!entry.sink->ShouldLog(level)) continue;
        try {
            entry.sink->Write(view);
        } catch (...) {
            // Sinks must not bring down the application; swallow.
        }
    }
}

void SyncLogger::LogMulti(Level level,
                          impl::LogItemList items,
                          std::unique_ptr<sinks::LogRecord> structured) {
    if (structured) {
        // Dispatch the structured path first — text and structured sinks
        // are independent; either may be absent.
        LogStructured(level, std::move(structured));
    }
    if (items.empty()) return;

    auto snap = sinks_.load();
    if (!snap) return;
    for (const auto& entry : *snap) {
        if (!entry.sink->ShouldLog(level)) continue;
        // `items[format_idx]` is the payload rendered for this sink's
        // registered format. On the single-format hot path `format_idx`
        // is 0 for every sink and only items[0] is touched.
        //
        // A sink whose format_idx exceeds items.size() was registered after
        // this record's LogHelper materialized its formatters — the sink is
        // invisible to in-flight records. Skip silently rather than writing
        // items[0] (wrong format) which would hide the race from callers.
        if (entry.format_idx >= items.size()) continue;
        auto* text = static_cast<impl::formatters::TextLogItem*>(items[entry.format_idx].get());
        // nullptr item means ExtractLoggerItem failed upstream (e.g. OOM);
        // drop the write for this sink rather than emit an empty record.
        if (!text) continue;
        try {
            entry.sink->Write(text->payload.view());
        } catch (...) {
            // Sinks must not bring down the application; swallow.
        }
    }
}

void SyncLogger::LogStructured(Level level, std::unique_ptr<sinks::LogRecord> record) {
    if (!record) return;
    auto snap = struct_sinks_.load();
    if (!snap) return;
    for (const auto& sink : *snap) {
        if (!sink || !sink->ShouldLog(level)) continue;
        try {
            sink->Write(*record);
        } catch (...) {
            // Sinks must not bring down the application; swallow.
        }
    }
}

void SyncLogger::Flush() {
    auto snap = sinks_.load();
    if (snap) {
        for (const auto& entry : *snap) {
            try {
                entry.sink->Flush();
            } catch (...) {
            }
        }
    }
    auto s_snap = struct_sinks_.load();
    if (s_snap) {
        for (const auto& sink : *s_snap) {
            try {
                if (sink) sink->Flush();
            } catch (...) {
            }
        }
    }
}

}  // namespace ulog
