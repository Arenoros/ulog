#include <ulog/impl/logger_base.hpp>

#include <chrono>

#include <ulog/impl/formatters/json.hpp>
#include <ulog/impl/formatters/ltsv.hpp>
#include <ulog/impl/formatters/raw.hpp>
#include <ulog/impl/formatters/tskv.hpp>

namespace ulog::impl {

LoggerBase::~LoggerBase() = default;

formatters::BasePtr TextLoggerBase::MakeFormatter(Level level,
                                                  std::string_view module_function,
                                                  std::string_view module_file,
                                                  int module_line) {
    const auto now = std::chrono::system_clock::now();
    switch (format_) {
        case Format::kTskv:
            return std::make_unique<formatters::TskvFormatter>(
                level, module_function, module_file, module_line, now);
        case Format::kLtsv:
            return std::make_unique<formatters::LtsvFormatter>(
                level, module_function, module_file, module_line, now);
        case Format::kRaw:
            return std::make_unique<formatters::RawFormatter>();
        case Format::kJson:
            return std::make_unique<formatters::JsonFormatter>(
                level, module_function, module_file, module_line, now,
                formatters::JsonFormatter::Variant::kStandard);
        case Format::kJsonYaDeploy:
            return std::make_unique<formatters::JsonFormatter>(
                level, module_function, module_file, module_line, now,
                formatters::JsonFormatter::Variant::kYaDeploy);
    }
    return nullptr;
}

}  // namespace ulog::impl
