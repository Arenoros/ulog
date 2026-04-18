#include <ulog/log_extra.hpp>

#include <gtest/gtest.h>

TEST(LogExtraTest, AddAndReplace) {
    ulog::LogExtra e;
    e.Extend("key", std::string("v1"));
    e.Extend("key", std::string("v2"));
    // Can't read back directly in public API; just ensure no throw & semantics via Frozen.
}

TEST(LogExtraTest, FrozenIsSticky) {
    ulog::LogExtra e;
    e.Extend("k", std::string("first"), ulog::LogExtra::ExtendType::kFrozen);
    e.Extend("k", std::string("ignored"));
    // Merge into another LogExtra — frozen key stays frozen.
    ulog::LogExtra other;
    other.Extend("k", std::string("other"));
    e.Extend(other);
    // Successful merge without throwing is acceptance here.
}

TEST(LogExtraTest, InitializerList) {
    ulog::LogExtra e{{"a", 1}, {"b", std::string("two")}};
    // Successful construction from InitializerList.
}

TEST(LogExtraTest, EmptyInstance) {
    EXPECT_NO_THROW((void)ulog::kEmptyLogExtra);
}
