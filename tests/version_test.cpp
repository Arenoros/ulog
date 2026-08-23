#include <gtest/gtest.h>

#include <ulog/version.hpp>

namespace {

TEST(Version, ExportedQueryMatchesHeader) {
  EXPECT_EQ(ulog::GetVersion(), ulog::kVersion);
  EXPECT_EQ(ulog::kVersion.major, 0U);
  EXPECT_EQ(ulog::kVersion.minor, 1U);
  EXPECT_EQ(ulog::kVersion.patch, 0U);
}

}  // namespace
