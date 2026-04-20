#pragma once

/// @file ulog/impl/formatters/ltsv.hpp
/// @brief LTSV formatter. Same as TSKV but with `:` separator instead of `=`.

#include <chrono>
#include <string_view>

#include <ulog/format.hpp>
#include <ulog/impl/formatters/base.hpp>
#include <ulog/impl/formatters/text_item.hpp>
#include <ulog/level.hpp>
#include <ulog/log_helper.hpp>

namespace ulog::impl::formatters {

class LtsvFormatter final : public Base {
public:
    LtsvFormatter(Level level,
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
