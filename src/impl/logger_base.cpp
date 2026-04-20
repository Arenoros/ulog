#include <ulog/impl/logger_base.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

#include <ulog/impl/formatters/json.hpp>
#include <ulog/impl/formatters/ltsv.hpp>
#include <ulog/impl/formatters/otlp_json.hpp>
#include <ulog/impl/formatters/raw.hpp>
#include <ulog/impl/formatters/tskv.hpp>

namespace ulog::impl {

LoggerBase::~LoggerBase() = default;

namespace {

/// Returns true if the caller-provided `scratch` buffer is large enough
/// and suitably aligned to host a `F`. The inline fast path requires
/// both; otherwise we fall back to `new`.
template <typename F>
bool FitsInScratch(void* scratch, std::size_t scratch_size) noexcept {
    if (!scratch) return false;
    if (scratch_size < sizeof(F)) return false;
    const auto addr = reinterpret_cast<std::uintptr_t>(scratch);
    return (addr % alignof(F)) == 0;
}

/// Places `F` in either caller-provided scratch (destroy-only deleter) or
/// on the heap (full deleter).
template <typename F, typename... Args>
formatters::BasePtr PlaceFormatter(void* scratch,
                                   std::size_t scratch_size,
                                   Args&&... args) {
    static_assert(sizeof(F) <= LoggerBase::kInlineFormatterSize,
                  "Formatter exceeds kInlineFormatterSize — bump the budget or "
                  "slim the formatter");
    static_assert(alignof(F) <= LoggerBase::kInlineFormatterAlign,
                  "Formatter alignment exceeds kInlineFormatterAlign — bump "
                  "the budget or realign the formatter");
    if (FitsInScratch<F>(scratch, scratch_size)) {
        auto* p = new (scratch) F(std::forward<Args>(args)...);
        return formatters::BasePtr(p, formatters::BaseDeleter{/*heap=*/false});
    }
    return formatters::BasePtr(new F(std::forward<Args>(args)...),
                               formatters::BaseDeleter{/*heap=*/true});
}

}  // namespace

formatters::BasePtr TextLoggerBase::MakeFormatterInto(
        void* scratch,
        std::size_t scratch_size,
        Level level,
        std::string_view module_function,
        std::string_view module_file,
        int module_line) {
    const auto now = std::chrono::system_clock::now();
    // `emit_location_ == false` suppresses the `module` field by erasing the
    // call-site inputs. Every text formatter already skips the field when both
    // function and file are empty — a single check here covers all of them.
    if (!emit_location_) {
        module_function = {};
        module_file = {};
        module_line = 0;
    }
    switch (format_) {
        case Format::kTskv:
            return PlaceFormatter<formatters::TskvFormatter>(
                scratch, scratch_size,
                level, module_function, module_file, module_line, now);
        case Format::kLtsv:
            return PlaceFormatter<formatters::LtsvFormatter>(
                scratch, scratch_size,
                level, module_function, module_file, module_line, now);
        case Format::kRaw:
            return PlaceFormatter<formatters::RawFormatter>(
                scratch, scratch_size);
        case Format::kJson:
            return PlaceFormatter<formatters::JsonFormatter>(
                scratch, scratch_size,
                level, module_function, module_file, module_line, now,
                formatters::JsonFormatter::Variant::kStandard);
        case Format::kJsonYaDeploy:
            return PlaceFormatter<formatters::JsonFormatter>(
                scratch, scratch_size,
                level, module_function, module_file, module_line, now,
                formatters::JsonFormatter::Variant::kYaDeploy);
        case Format::kOtlpJson:
            return PlaceFormatter<formatters::OtlpJsonFormatter>(
                scratch, scratch_size,
                level, module_function, module_file, module_line, now);
    }
    return formatters::BasePtr{};
}

}  // namespace ulog::impl
