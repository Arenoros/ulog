#pragma once

/// @file ulog/detail/small_string.hpp
/// @brief SSO-backed char buffer for log formatting.

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

#include <boost/container/small_vector.hpp>

namespace ulog::detail {

/// Fixed-capacity SSO-backed char buffer. Falls back to heap beyond Capacity.
/// Used to keep typical log records (<= Capacity bytes) entirely on the stack.
template <std::size_t Capacity>
class SmallString {
public:
    SmallString() = default;

    /// Replace contents with `sv`. Reuses inline storage when possible;
    /// drops the heap spill if the new value fits back into the SSO slab.
    SmallString& assign(std::string_view sv) {
        buf_.clear();
        append(sv);
        return *this;
    }

    void append(std::string_view sv) {
        const auto old = buf_.size();
        buf_.resize(old + sv.size());
        std::memcpy(buf_.data() + old, sv.data(), sv.size());
    }
    void append(char c) { buf_.push_back(c); }
    void append(const char* s) { append(std::string_view(s)); }

    SmallString& operator+=(std::string_view sv) {
        append(sv);
        return *this;
    }
    SmallString& operator+=(const char* s) {
        append(std::string_view(s));
        return *this;
    }
    SmallString& operator+=(char c) {
        append(c);
        return *this;
    }

    const char* data() const noexcept { return buf_.data(); }
    char* data() noexcept { return buf_.data(); }
    std::size_t size() const noexcept { return buf_.size(); }
    bool empty() const noexcept { return buf_.empty(); }
    void clear() noexcept { buf_.clear(); }
    void reserve(std::size_t n) { buf_.reserve(n); }
    void resize(std::size_t n) { buf_.resize(n); }
    void pop_back() { buf_.pop_back(); }

    std::string_view view() const noexcept { return std::string_view(buf_.data(), buf_.size()); }
    operator std::string_view() const noexcept { return view(); }  // NOLINT(google-explicit-constructor)

    std::string ToString() const { return std::string(buf_.data(), buf_.size()); }

private:
    boost::container::small_vector<char, Capacity> buf_;
};

}  // namespace ulog::detail
