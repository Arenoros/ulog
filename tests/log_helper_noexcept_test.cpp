#include <memory>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include <ulog/log.hpp>
#include <ulog/log_extra.hpp>
#include <ulog/log_helper.hpp>
#include <ulog/mem_logger.hpp>
#include <ulog/null_logger.hpp>

namespace {

// Static verification that every user-facing streaming overload is
// `noexcept`. Catches accidental removal of the try/catch guards added
// in Phase 2. These checks are compile-time — no runtime cost.
void StaticNoexceptChecks() {
    using LH = ulog::LogHelper;
    LH* p = nullptr;
    (void)p;

    // Template operator<< must propagate noexcept from the public contract.
    // We use `std::declval<T>()` for construct-involving types — libc++
    // (C++17) does not mark `std::string_view(const char*)` `noexcept`,
    // so a literal `Quoted{"q"}` or `std::string_view{"v"}` here would
    // fold construction non-noexcept into the checked expression and
    // make the test assert a property of `string_view`'s ctor rather
    // than `operator<<` itself.
    static_assert(noexcept(std::declval<LH&>() << 42));
    static_assert(noexcept(std::declval<LH&>() << 3.14));
    static_assert(noexcept(std::declval<LH&>() << 'c'));
    static_assert(noexcept(std::declval<LH&>() << true));
    static_assert(noexcept(std::declval<LH&>() << "lit"));
    static_assert(noexcept(std::declval<LH&>() << std::declval<std::string_view>()));

    // Typed overloads.
    static_assert(noexcept(std::declval<LH&>() << ulog::Hex{0}));
    static_assert(noexcept(std::declval<LH&>() << ulog::HexShort{0}));
    static_assert(noexcept(std::declval<LH&>() << std::declval<ulog::Quoted>()));
    static_assert(noexcept(std::declval<LH&>() << std::declval<const ulog::LogExtra&>()));

    // WithException and rvalue variants.
    static_assert(noexcept(std::declval<LH&>().WithException(std::declval<std::exception&>())));
    static_assert(noexcept(std::declval<LH&&>() << 42));
}

}  // namespace

// Compile-time: if the above file compiles, noexcept contract holds.
TEST(LogHelperNoexcept, StaticContractHolds) {
    SUCCEED() << "Static checks compile-verified — see StaticNoexceptChecks.";
}

// A destructor that logs — only valid if LOG_* stays noexcept, otherwise
// an exception in dtor scope triggers std::terminate under C++11+
// implicit-noexcept rules. This test runs the dtor path; if logging were
// throwy the process would abort.
namespace {
struct LoggingDtor {
    ~LoggingDtor() noexcept {
        LOG_INFO() << "dtor log id=" << id << " tag";
    }
    int id;
};
}  // namespace

TEST(LogHelperNoexcept, LogsFromNoexceptDestructor) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);
    ulog::SetDefaultLogger(mem);

    {
        LoggingDtor d{7};
        (void)d;
    }

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_NE(recs[0].find("dtor log id=7"), std::string::npos) << recs[0];
    ulog::SetDefaultLogger(nullptr);
}

// `operator<< LogExtra` with a LogExtra that has a null-string-like
// member must still be noexcept and either emit or silently drop.
TEST(LogHelperNoexcept, LogExtraStreamsCleanly) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);
    ulog::SetDefaultLogger(mem);

    ulog::LogExtra e{{"k", std::string("v")}};
    LOG_INFO() << "m" << e;

    EXPECT_EQ(mem->GetRecords().size(), 1u);
    ulog::SetDefaultLogger(nullptr);
}

// Chained streaming of many values — exercises the repeated `broken`
// early-return branch on the hot path. Not a throw test, just
// regression against accidental short-circuit inversion.
TEST(LogHelperNoexcept, ChainedStreamingAllArrive) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);
    ulog::SetDefaultLogger(mem);

    LOG_INFO() << "a=" << 1 << " b=" << 2 << " c=" << 3 << " d=" << ulog::Hex{0x10}
               << " e=" << ulog::HexShort{0x20} << " f=" << ulog::Quoted{"q"};

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_NE(recs[0].find("a=1 b=2 c=3"), std::string::npos) << recs[0];
    ulog::SetDefaultLogger(nullptr);
}
