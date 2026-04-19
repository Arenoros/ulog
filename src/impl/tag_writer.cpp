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
    // Plain member references instead of structured bindings — C++17
    // forbids lambdas from capturing binding names (gcc 12 enforces it,
    // MSVC / newer gcc are lenient). Converting to `item.first` /
    // `item.second` avoids the substantive complaint without splitting
    // the loop body.
    for (const auto& item : extra.extra_) {
        const auto& k = item.first;
        const auto& v = item.second;
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
