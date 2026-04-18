#include <ulog/log_extra.hpp>

#include <algorithm>

#include <boost/stacktrace.hpp>

#include <ulog/stacktrace_cache.hpp>

namespace ulog {

const LogExtra kEmptyLogExtra{};

LogExtra::LogExtra() noexcept = default;
LogExtra::LogExtra(const LogExtra&) = default;
LogExtra::LogExtra(LogExtra&&) noexcept = default;
LogExtra::~LogExtra() = default;
LogExtra& LogExtra::operator=(const LogExtra&) = default;
LogExtra& LogExtra::operator=(LogExtra&&) noexcept = default;

LogExtra::LogExtra(std::initializer_list<Pair> initial, ExtendType extend_type) {
    Extend(initial, extend_type);
}

void LogExtra::Extend(std::string key, Value value, ExtendType extend_type) {
    auto* existing = Find(key);
    if (existing) {
        if (!existing->second.IsFrozen()) {
            existing->second.AssignIgnoringFrozenness(
                ProtectedValue(std::move(value), extend_type == ExtendType::kFrozen));
        }
        return;
    }
    extra_.emplace_back(std::move(key), ProtectedValue(std::move(value), extend_type == ExtendType::kFrozen));
}

void LogExtra::Extend(Pair extra, ExtendType extend_type) {
    Extend(std::move(extra.first), std::move(extra.second), extend_type);
}

void LogExtra::Extend(std::initializer_list<Pair> extra, ExtendType extend_type) {
    for (auto& p : extra) {
        // initializer_list holds const Pair — copy is necessary.
        Extend(p.first, p.second, extend_type);
    }
}

void LogExtra::Extend(const LogExtra& extra) {
    for (const auto& [k, v] : extra.extra_) {
        auto* existing = Find(k);
        if (existing) {
            if (!existing->second.IsFrozen()) existing->second.AssignIgnoringFrozenness(v);
        } else {
            extra_.emplace_back(k, v);
        }
    }
}

void LogExtra::Extend(LogExtra&& extra) {
    for (auto& item : extra.extra_) {
        auto* existing = Find(item.first);
        if (existing) {
            if (!existing->second.IsFrozen())
                existing->second.AssignIgnoringFrozenness(std::move(item.second));
        } else {
            extra_.emplace_back(std::move(item));
        }
    }
    extra.extra_.clear();
}

void LogExtra::SetFrozen(std::string_view key) {
    if (auto* p = Find(key)) p->second.SetFrozen();
}

LogExtra::MapItem* LogExtra::Find(std::string_view key) {
    for (auto& item : extra_)
        if (item.first == key) return &item;
    return nullptr;
}
const LogExtra::MapItem* LogExtra::Find(std::string_view key) const {
    for (const auto& item : extra_)
        if (item.first == key) return &item;
    return nullptr;
}

LogExtra LogExtra::StacktraceNocache() noexcept {
    try {
        const auto st = boost::stacktrace::stacktrace();
        LogExtra e;
        e.Extend("stacktrace", boost::stacktrace::to_string(st));
        return e;
    } catch (...) {
        return {};
    }
}

LogExtra LogExtra::Stacktrace() noexcept {
    try {
        auto s = CurrentStacktrace();
        if (s.empty()) return {};
        LogExtra e;
        e.Extend("stacktrace", std::move(s));
        return e;
    } catch (...) {
        return {};
    }
}

}  // namespace ulog
