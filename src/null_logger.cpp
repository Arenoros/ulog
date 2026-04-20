#include <ulog/null_logger.hpp>

#include <ulog/format.hpp>
#include <ulog/impl/logger_base.hpp>

namespace ulog {

namespace {

class NullLogger final : public impl::LoggerBase {
public:
    NullLogger() noexcept {
        SetLevel(Level::kNone);
        SetFlushOn(Level::kNone);
    }
    void Log(Level, std::unique_ptr<impl::LoggerItemBase>) override {}
    void Flush() override {}
    impl::formatters::BasePtr MakeFormatterInto(void*, std::size_t,
                                                Level,
                                                const LogRecordLocation&) override {
        return impl::formatters::BasePtr{};
    }
};

NullLogger& GetNullInstance() noexcept {
    static NullLogger instance;
    return instance;
}

}  // namespace

LoggerRef GetNullLogger() noexcept { return GetNullInstance(); }

LoggerPtr MakeNullLogger() {
    return std::shared_ptr<impl::LoggerBase>(&GetNullInstance(), [](impl::LoggerBase*) {});
}

}  // namespace ulog
