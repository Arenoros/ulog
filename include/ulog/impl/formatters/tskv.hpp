#pragma once

/// @file ulog/impl/formatters/tskv.hpp
/// @brief TSKV formatter. Writes `key=value\tkey=value\t...\n`.

#include <chrono>
#include <string_view>

#include <ulog/format.hpp>
#include <ulog/impl/formatters/base.hpp>
#include <ulog/impl/formatters/text_item.hpp>
#include <ulog/level.hpp>
#include <ulog/log_helper.hpp>

namespace ulog::impl::formatters {

class TskvFormatter final : public Base {
public:
    /// Emits the TSKV header (timestamp, level, optional module) eagerly.
    TskvFormatter(Level level,
                  const LogRecordLocation& location,
                  std::chrono::system_clock::time_point tp,
                  TimestampFormat ts_fmt = TimestampFormat::kIso8601Micro);

    void AddTag(std::string_view key, std::string_view value) override;
    void AddJsonTag(std::string_view key, const JsonString& value) override;
    void SetText(std::string_view text) override;
    std::unique_ptr<LoggerItemBase> ExtractLoggerItem() override;

private:
    std::unique_ptr<TextLogItem> item_{std::make_unique<TextLogItem>()};
    bool finalized_{false};
};

}  // namespace ulog::impl::formatters
