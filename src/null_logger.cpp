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
    void Log(Level, impl::LoggerItemRef) override {}
    void Flush() override {}
    impl::formatters::BasePtr MakeFormatter(Level, std::string_view) override { return nullptr; }
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
