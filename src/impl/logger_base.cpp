#include <ulog/impl/logger_base.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

#include <boost/make_shared.hpp>

#include <ulog/impl/formatters/json.hpp>
#include <ulog/impl/formatters/ltsv.hpp>
#include <ulog/impl/formatters/otlp_json.hpp>
#include <ulog/impl/formatters/raw.hpp>
#include <ulog/impl/formatters/tskv.hpp>

namespace ulog::impl {

LoggerBase::~LoggerBase() = default;

void LoggerBase::PrependCommonTags(TagSink& sink) const noexcept {
    // Fast path: skip the atomic_shared_ptr refcount RMW when no tags
    // were ever set (or the last one was just cleared). Under 16-thread
    // producer contention the refcount pingpong on the shared control
    // block dominates the per-record cost; the atomic<bool> load is
    // cheap and cache-friendly.
    if (!has_common_tags_.load(std::memory_order_acquire)) return;
    auto snap = common_tags_.load();
    if (!snap) return;
    for (const auto& kv : *snap) {
        // sink.AddTag is not `noexcept`, but the concrete
        // FormatterTagSink / TagRecorder implementations do not throw.
        // Guard with try/catch anyway so a misbehaving override cannot
        // propagate — we are called from LogHelper's `noexcept` dtor.
        try {
            sink.AddTag(kv.first, kv.second);
        } catch (...) {
            // Drop the tag silently. LogHelper's DoLog also swallows.
        }
    }
}

void LoggerBase::SetCommonTag(std::string_view key, std::string_view value) {
    std::lock_guard lk(common_tags_mu_);
    auto current = common_tags_.load();
    auto next = boost::make_shared<CommonTagsVec>();
    if (current) {
        next->reserve(current->size() + 1);
        for (const auto& kv : *current) {
            if (kv.first == key) continue;  // overwrite — skip old entry
            next->push_back(kv);
        }
    }
    next->emplace_back(std::string(key), std::string(value));
    common_tags_.store(boost::shared_ptr<const CommonTagsVec>(std::move(next)));
    // Publish the fast-path flag after the snapshot is visible. A reader
    // that sees the flag = true will load a snapshot at least as new as
    // this one (acquire-release pair). If the reader still holds an
    // older pinned snapshot, that is fine — the old one is also valid.
    has_common_tags_.store(true, std::memory_order_release);
}

void LoggerBase::RemoveCommonTag(std::string_view key) {
    std::lock_guard lk(common_tags_mu_);
    auto current = common_tags_.load();
    if (!current) return;
    auto next = boost::make_shared<CommonTagsVec>();
    next->reserve(current->size());
    bool removed = false;
    for (const auto& kv : *current) {
        if (!removed && kv.first == key) {
            removed = true;
            continue;
        }
        next->push_back(kv);
    }
    if (!removed) return;  // no-op: key not present
    if (next->empty()) {
        common_tags_.store(boost::shared_ptr<const CommonTagsVec>{});
        // Clear fast-path flag — subsequent PrependCommonTags short-circuit.
        // A reader that still sees `true` briefly will load the null
        // snapshot and bail via the null-check — safe, just a missed
        // short-circuit for one record.
        has_common_tags_.store(false, std::memory_order_release);
    } else {
        common_tags_.store(
            boost::shared_ptr<const CommonTagsVec>(std::move(next)));
    }
}

void LoggerBase::ClearCommonTags() noexcept {
    std::lock_guard lk(common_tags_mu_);
    common_tags_.store(boost::shared_ptr<const CommonTagsVec>{});
    has_common_tags_.store(false, std::memory_order_release);
}

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

TextLoggerBase::TextLoggerBase(Format format,
                               bool emit_location,
                               TimestampFormat ts_fmt)
    : format_(format), emit_location_(emit_location), ts_fmt_(ts_fmt) {
    auto initial = boost::make_shared<std::vector<Format>>();
    initial->push_back(format_);  // base format is always index 0
    active_formats_.store(
        boost::shared_ptr<std::vector<Format> const>(std::move(initial)));
    active_format_count_.store(1, std::memory_order_release);
}

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
    // Serialize writers; readers load the atomic shared_ptr below.
    std::lock_guard lk(formats_mu_);
    auto current = active_formats_.load();
    const Format want = override.value_or(format_);
    if (current) {
        for (std::size_t i = 0; i < current->size(); ++i) {
            if ((*current)[i] == want) return i;
        }
    }
    // Copy-on-write: build a fresh vector that includes the new format
    // and publish it. Already-held snapshots keep referencing the old
    // vector until their pin is released — immutable after publish.
    auto next = boost::make_shared<std::vector<Format>>();
    if (current) next->reserve(current->size() + 1);
    if (current) *next = *current;
    next->push_back(want);
    const std::size_t idx = next->size() - 1;
    active_format_count_.store(next->size(), std::memory_order_release);
    active_formats_.store(
        boost::shared_ptr<std::vector<Format> const>(std::move(next)));
    return idx;
}

std::vector<Format> TextLoggerBase::GetActiveFormats() const {
    auto snap = active_formats_.load();
    if (!snap) return {};
    return *snap;
}

boost::shared_ptr<std::vector<Format> const>
TextLoggerBase::GetActiveFormatsSnapshot() const noexcept {
    return active_formats_.load();
}

}  // namespace ulog::impl
