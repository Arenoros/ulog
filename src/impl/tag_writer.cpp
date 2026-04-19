#include <ulog/impl/tag_writer.hpp>

#include <type_traits>
#include <variant>

#include <fmt/format.h>

#include <ulog/log_extra.hpp>

namespace ulog::impl {

void TagWriter::PutTag(std::string_view key, std::string_view value) {
    if (formatter_) formatter_->AddTag(key, value);
}

void TagWriter::PutJsonTag(std::string_view key, const JsonString& value) {
    if (formatter_) formatter_->AddJsonTag(key, value);
}

void TagWriter::PutLogExtra(const LogExtra& extra) {
    if (!formatter_) return;
    for (const auto& [k, v] : extra.extra_) {
        std::visit(
            [&](const auto& x) {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    formatter_->AddTag(k, x);
                } else if constexpr (std::is_same_v<T, JsonString>) {
                    formatter_->AddJsonTag(k, x);
                } else if constexpr (std::is_same_v<T, bool>) {
                    formatter_->AddTagBool(k, x);
                } else if constexpr (std::is_floating_point_v<T>) {
                    formatter_->AddTagDouble(k, static_cast<double>(x));
                } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
                    formatter_->AddTagInt64(k, static_cast<std::int64_t>(x));
                } else if constexpr (std::is_integral_v<T>) {
                    formatter_->AddTagUInt64(k, static_cast<std::uint64_t>(x));
                } else {
                    formatter_->AddTag(k, fmt::format("{}", x));
                }
            },
            v.GetValue());
    }
}

}  // namespace ulog::impl
