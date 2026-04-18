#pragma once

/// @file ulog/impl/formatters/json.hpp
/// @brief JSON formatter — emits one JSON object per record with a trailing newline.

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include <ulog/impl/formatters/base.hpp>
#include <ulog/impl/formatters/text_item.hpp>
#include <ulog/level.hpp>

namespace ulog::impl::formatters {

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
    void SetText(std::string_view text) override;
    LoggerItemRef ExtractLoggerItem() override;

private:
    struct Field {
        std::string key;
        std::string value;
        bool is_json{false};  // true => value is raw JSON, emit as-is
    };

    void Finalize();

    std::vector<Field> fields_;
    std::string text_;
    TextLogItem item_;
    Variant variant_;
    bool finalized_{false};
};

}  // namespace ulog::impl::formatters
