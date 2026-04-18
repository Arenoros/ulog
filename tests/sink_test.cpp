#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include <ulog/log.hpp>
#include <ulog/sinks/fd_sink.hpp>
#include <ulog/sinks/file_sink.hpp>
#include <ulog/sinks/null_sink.hpp>
#include <ulog/sync_logger.hpp>

namespace fs = std::filesystem;

namespace {

std::string ReadFileAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

TEST(FileSink, WritesAndFlushes) {
    auto tmp = fs::temp_directory_path() / "ulog_file_sink.log";
    fs::remove(tmp);

    {
        auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
        logger->SetLevel(ulog::Level::kTrace);
        logger->AddSink(std::make_shared<ulog::sinks::FileSink>(tmp.string(), /*truncate=*/true));

        ulog::SetDefaultLogger(logger);
        LOG_INFO() << "alpha";
        LOG_ERROR() << "beta";
        ulog::LogFlush();
        ulog::SetDefaultLogger(nullptr);
    }  // logger + sink destroyed -> file closed

    const auto contents = ReadFileAll(tmp);
    EXPECT_NE(contents.find("text=alpha"), std::string::npos);
    EXPECT_NE(contents.find("text=beta"), std::string::npos);
    EXPECT_NE(contents.find("level=INFO"), std::string::npos);
    EXPECT_NE(contents.find("level=ERROR"), std::string::npos);

    fs::remove(tmp);
}

TEST(FileSink, ReopenAfterRotate) {
    auto tmp = fs::temp_directory_path() / "ulog_file_sink_rotate.log";
    auto rotated = tmp;
    rotated += ".1";
    fs::remove(tmp);
    fs::remove(rotated);

    {
        auto sink = std::make_shared<ulog::sinks::FileSink>(tmp.string(), /*truncate=*/true);
        auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
        logger->SetLevel(ulog::Level::kTrace);
        logger->AddSink(sink);

        ulog::SetDefaultLogger(logger);
        LOG_INFO() << "before-rotate";
        ulog::LogFlush();

        // Windows can't rename an open file; close via Reopen-to-self first.
        // Classic rotation flow: rename -> tool moves old path -> SIGUSR1 -> Reopen
        // creates a new file at the original path. On Windows we must release
        // the handle before renaming.
        sink->Reopen(ulog::sinks::ReopenMode::kAppend);  // still points at tmp
        fs::copy_file(tmp, rotated, fs::copy_options::overwrite_existing);
        fs::resize_file(tmp, 0);  // truncate in-place (keeps handle valid)
        sink->Reopen(ulog::sinks::ReopenMode::kAppend);

        LOG_INFO() << "after-rotate";
        ulog::LogFlush();
        ulog::SetDefaultLogger(nullptr);
    }

    const auto first = ReadFileAll(rotated);
    const auto second = ReadFileAll(tmp);
    EXPECT_NE(first.find("text=before-rotate"), std::string::npos);
    EXPECT_NE(second.find("text=after-rotate"), std::string::npos);

    fs::remove(rotated);
    fs::remove(tmp);
}

TEST(NullSink, AcceptsAnything) {
    ulog::sinks::NullSink sink;
    sink.Write("whatever");  // no throw
    SUCCEED();
}

TEST(StdoutSink, Constructs) {
    auto sink = ulog::sinks::StdoutSink();
    ASSERT_NE(sink, nullptr);
    // We don't verify stdout content here; just that construction works.
}
