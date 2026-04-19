#pragma once

/// @file ulog/impl/formatters/json.hpp
/// @brief JSON formatter — emits one JSON object per record with a trailing newline.

#include <chrono>
#include <memory>
#include <string_view>

#include <ulog/detail/small_string.hpp>
#include <ulog/impl/formatters/base.hpp>
#include <ulog/impl/formatters/text_item.hpp>
#include <ulog/level.hpp>

namespace ulog::impl::formatters {

/// Direct-emit JSON formatter: tags are escaped and appended into the
/// TextLogItem buffer as they arrive, matching the TSKV formatter's
/// streaming pattern. The only deferred field is `text`, which is
/// written on ExtractLoggerItem() so it appears last in the object
/// regardless of when SetText() was called.
class JsonFormatter final : public Base {
public:
    enum class Variant { kStandard, kYaDeploy };

    JsonFormatter(Level level,
                  std::string_view module_function,
                  std::string_view module_file,
                  int module_line,
                  std::chrono::system_clock::time_point tp,
                  Variant variant = Variant::kStandard);

    void AddTag(std::string_view key, std::string_view value) override;
    void AddJsonTag(std::string_view key, const JsonString& value) override;
    void AddTagInt64(std::string_view key, std::int64_t value) override;
    void AddTagUInt64(std::string_view key, std::uint64_t value) override;
    void AddTagDouble(std::string_view key, double value) override;
    void AddTagBool(std::string_view key, bool value) override;
    void SetText(std::string_view text) override;
    std::unique_ptr<LoggerItemBase> ExtractLoggerItem() override;

private:
    void EmitField(std::string_view key, std::string_view value, bool value_is_json);
    std::string_view TranslateKey(std::string_view key) const noexcept;

    std::unique_ptr<TextLogItem> item_{std::make_unique<TextLogItem>()};
    /// Free-form message body. Short messages (≤64 bytes — the bulk of
    /// production records) live entirely in this SSO slab; longer ones
    /// spill to the heap transparently via `boost::small_vector`.
    detail::SmallString<64> text_;
    Variant variant_;
    bool finalized_{false};
};

}  // namespace ulog::impl::formatters
