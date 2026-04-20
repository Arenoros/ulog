#pragma once

/// @file ulog/mem_logger.hpp
/// @brief In-memory logger that captures records for testing.

#include <mutex>
#include <string>
#include <vector>

#include <ulog/format.hpp>
#include <ulog/impl/formatters/text_item.hpp>
#include <ulog/impl/logger_base.hpp>

namespace ulog {

/// Captures every record it receives as a string. Thread-safe.
class MemLogger final : public impl::TextLoggerBase {
public:
    explicit MemLogger(Format format = Format::kTskv,
                       bool emit_location = true,
                       TimestampFormat ts_fmt = TimestampFormat::kIso8601Micro)
        : impl::TextLoggerBase(format, emit_location, ts_fmt) {
        SetLevel(Level::kTrace);
        SetFlushOn(Level::kNone);
    }

    void Log(Level, impl::LoggerItemPtr item) override {
        if (!item) return;
        auto& text = static_cast<impl::formatters::TextLogItem&>(*item);
        std::lock_guard lock(mu_);
        records_.emplace_back(text.payload.view());
    }

    void Flush() override {}

    std::vector<std::string> GetRecords() const {
        std::lock_guard lock(mu_);
        return records_;
    }

    void Clear() {
        std::lock_guard lock(mu_);
        records_.clear();
    }

private:
    mutable std::mutex mu_;
    std::vector<std::string> records_;
};

}  // namespace ulog
