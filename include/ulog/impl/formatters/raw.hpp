#pragma once

/// @file ulog/impl/formatters/raw.hpp
/// @brief RAW formatter — TSKV shape minus the default timestamp/level/module
///        header. Intended for custom loggers that supply their own framing.

#include <string_view>

#include <ulog/impl/formatters/base.hpp>
#include <ulog/impl/formatters/text_item.hpp>

namespace ulog::impl::formatters {

class RawFormatter final : public Base {
public:
    RawFormatter() = default;

    void AddTag(std::string_view key, std::string_view value) override;
    void AddJsonTag(std::string_view key, const JsonString& value) override;
    void SetText(std::string_view text) override;
    std::unique_ptr<LoggerItemBase> ExtractLoggerItem() override;

private:
    std::unique_ptr<TextLogItem> item_{std::make_unique<TextLogItem>()};
    bool has_fields_{false};
    bool finalized_{false};
};

}  // namespace ulog::impl::formatters
