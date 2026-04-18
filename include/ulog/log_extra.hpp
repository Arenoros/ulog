#pragma once

/// @file ulog/log_extra.hpp
/// @brief Extra key-value fields attached to a log record.

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <boost/container/small_vector.hpp>

#include <ulog/fwd.hpp>
#include <ulog/json_string.hpp>

namespace ulog {

class LogHelper;

namespace impl {
class TagWriter;
class LogExtraFormatter;
}  // namespace impl

/// Structured key-value fields attached to log messages.
class LogExtra final {
public:
    using Value = std::variant<
        std::string,
        int,
        bool,
        long,
        long long,
        unsigned int,
        unsigned long,
        unsigned long long,
        float,
        double,
        JsonString>;
    using Key = std::string;
    using Pair = std::pair<Key, Value>;

    /// Replacement policy for newly added values.
    enum class ExtendType {
        kNormal,  ///< Value can be replaced later
        kFrozen,  ///< Subsequent writes to the same key are silently ignored
    };

    LogExtra() noexcept;
    LogExtra(const LogExtra&);
    LogExtra(LogExtra&&) noexcept;
    ~LogExtra();
    LogExtra& operator=(const LogExtra&);
    LogExtra& operator=(LogExtra&&) noexcept;

    LogExtra(std::initializer_list<Pair> initial, ExtendType extend_type = ExtendType::kNormal);

    void Extend(std::string key, Value value, ExtendType extend_type = ExtendType::kNormal);
    void Extend(Pair extra, ExtendType extend_type = ExtendType::kNormal);
    void Extend(std::initializer_list<Pair> extra, ExtendType extend_type = ExtendType::kNormal);
    void Extend(const LogExtra& extra);
    void Extend(LogExtra&& extra);

    template <typename Iterator>
    void ExtendRange(Iterator first, Iterator last, ExtendType extend_type = ExtendType::kNormal) {
        for (auto it = first; it != last; ++it) Extend(*it, extend_type);
    }

    /// Marks an existing value as frozen.
    void SetFrozen(std::string_view key);

    /// Creates a LogExtra capturing the current thread's stacktrace.
    static LogExtra Stacktrace() noexcept;
    static LogExtra StacktraceNocache() noexcept;

    friend class LogHelper;
    friend class impl::TagWriter;
    friend class impl::LogExtraFormatter;

private:
    class ProtectedValue {
    public:
        ProtectedValue() = default;
        ProtectedValue(Value value, bool frozen) : value_(std::move(value)), frozen_(frozen) {}

        bool IsFrozen() const noexcept { return frozen_; }
        void SetFrozen() noexcept { frozen_ = true; }
        Value& GetValue() noexcept { return value_; }
        const Value& GetValue() const noexcept { return value_; }
        void AssignIgnoringFrozenness(ProtectedValue other) {
            value_ = std::move(other.value_);
            frozen_ = other.frozen_;
        }

    private:
        Value value_;
        bool frozen_{false};
    };

    static constexpr std::size_t kSmallVectorSize = 16;
    using MapItem = std::pair<Key, ProtectedValue>;
    using Map = boost::container::small_vector<MapItem, kSmallVectorSize>;

    MapItem* Find(std::string_view key);
    const MapItem* Find(std::string_view key) const;

    Map extra_;
};

extern const LogExtra kEmptyLogExtra;

}  // namespace ulog
