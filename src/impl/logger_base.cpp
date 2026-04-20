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

namespace {

formatters::BasePtr MakeFormatterImpl(Format fmt,
                                      TimestampFormat ts_fmt,
                                      void* scratch,
                                      std::size_t scratch_size,
                                      Level level,
                                      const LogRecordLocation& location,
                                      std::chrono::system_clock::time_point now) {
    switch (fmt) {
        case Format::kTskv:
            return PlaceFormatter<formatters::TskvFormatter>(
                scratch, scratch_size,
                level, location, now, ts_fmt);
        case Format::kLtsv:
            return PlaceFormatter<formatters::LtsvFormatter>(
                scratch, scratch_size,
                level, location, now, ts_fmt);
        case Format::kRaw:
            return PlaceFormatter<formatters::RawFormatter>(
                scratch, scratch_size);
        case Format::kJson:
            return PlaceFormatter<formatters::JsonFormatter>(
                scratch, scratch_size,
                level, location, now,
                formatters::JsonFormatter::Variant::kStandard, ts_fmt);
        case Format::kJsonYaDeploy:
            return PlaceFormatter<formatters::JsonFormatter>(
                scratch, scratch_size,
                level, location, now,
                formatters::JsonFormatter::Variant::kYaDeploy, ts_fmt);
        case Format::kOtlpJson:
            return PlaceFormatter<formatters::OtlpJsonFormatter>(
                scratch, scratch_size,
                level, location, now);
    }
    return formatters::BasePtr{};
}

}  // namespace

namespace {

// Shared fallback used when `emit_location_ == false` — every text
// formatter already skips `module` when the location is empty.
// One definition reused by both `MakeFormatterInto` and
// `MakeFormatterForFormat`; `static constexpr` gives it internal
// linkage + program-lifetime storage, safe to return via const&.
constexpr LogRecordLocation kEmptyLocation{};

}  // namespace

formatters::BasePtr TextLoggerBase::MakeFormatterInto(
        void* scratch,
        std::size_t scratch_size,
        Level level,
        const LogRecordLocation& location) {
    const auto now = std::chrono::system_clock::now();
    return MakeFormatterImpl(format_, ts_fmt_, scratch, scratch_size,
                             level, emit_location_ ? location : kEmptyLocation, now);
}

formatters::BasePtr TextLoggerBase::MakeFormatterForFormat(
        Format fmt,
        Level level,
        const LogRecordLocation& location) {
    const auto now = std::chrono::system_clock::now();
    // Force heap by passing null scratch.
    return MakeFormatterImpl(fmt, ts_fmt_, /*scratch=*/nullptr, /*size=*/0,
                             level, emit_location_ ? location : kEmptyLocation, now);
}

std::size_t TextLoggerBase::RegisterSinkFormat(std::optional<Format> override) {
    std::lock_guard lk(formats_mu_);
    const Format want = override.value_or(format_);
    for (std::size_t i = 0; i < active_formats_.size(); ++i) {
        if (active_formats_[i] == want) return i;
    }
    active_formats_.push_back(want);
    return active_formats_.size() - 1;
}

std::vector<Format> TextLoggerBase::GetActiveFormats() const {
    std::lock_guard lk(formats_mu_);
    return active_formats_;
}

std::size_t TextLoggerBase::GetActiveFormatCount() const noexcept {
    std::lock_guard lk(formats_mu_);
    return active_formats_.size();
}

}  // namespace ulog::impl
