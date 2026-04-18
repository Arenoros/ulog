#pragma once

/// @file ulog/sinks/null_sink.hpp
/// @brief Sink that discards everything.

#include <ulog/sinks/base_sink.hpp>

namespace ulog::sinks {

class NullSink final : public BaseSink {
public:
    void Write(std::string_view /*record*/) override {}
};

}  // namespace ulog::sinks
