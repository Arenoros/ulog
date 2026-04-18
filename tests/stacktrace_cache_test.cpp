#include <string>

#include <boost/stacktrace.hpp>
#include <gtest/gtest.h>

#include <ulog/stacktrace_cache.hpp>

TEST(StacktraceCache, CurrentReturnsNonEmptyWhenEnabled) {
    ulog::ClearStacktraceCache();
    const auto s = ulog::CurrentStacktrace();
    EXPECT_FALSE(s.empty());
}

TEST(StacktraceCache, DisabledReturnsEmpty) {
    ulog::StacktraceGuard off(false);
    EXPECT_TRUE(ulog::CurrentStacktrace().empty());
}

TEST(StacktraceCache, GuardRestoresPreviousState) {
    const bool before = ulog::IsStacktraceEnabled();
    {
        ulog::StacktraceGuard off(!before);
        EXPECT_EQ(ulog::IsStacktraceEnabled(), !before);
    }
    EXPECT_EQ(ulog::IsStacktraceEnabled(), before);
}

TEST(StacktraceCache, ExplicitCaptureSameFrames) {
    ulog::ClearStacktraceCache();
    const boost::stacktrace::stacktrace st;
    const auto first = ulog::StacktraceToString(st);
    const auto second = ulog::StacktraceToString(st);
    EXPECT_FALSE(first.empty());
    EXPECT_EQ(first, second);
}
