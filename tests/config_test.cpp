#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include <gtest/gtest.h>

#include <ulog/config.hpp>
#include <ulog/log.hpp>
#include <ulog/sinks/fd_sink.hpp>

namespace fs = std::filesystem;

namespace {
void RemoveQuiet(const fs::path& p) {
    std::error_code ec;
    fs::remove(p, ec);
}
}  // namespace

TEST(Config, MakeSinkNullAccepts) {
    auto sink = ulog::MakeSinkFromSpec("@null");
    sink->Write("ignored");
    SUCCEED();
}

TEST(Config, MakeSinkStdoutStderrBuild) {
    EXPECT_NO_THROW(ulog::MakeSinkFromSpec("@stdout"));
    EXPECT_NO_THROW(ulog::MakeSinkFromSpec("@stderr"));
}

TEST(Config, MakeSinkFileCreatesFile) {
    auto tmp = fs::temp_directory_path() / "ulog_cfg_file.log";
    RemoveQuiet(tmp);

    {
        auto sink = ulog::MakeSinkFromSpec(tmp.string(), /*truncate=*/true);
        sink->Write("hello\n");
        sink->Flush();
    }

    std::ifstream f(tmp, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    EXPECT_EQ(ss.str(), "hello\n");

    RemoveQuiet(tmp);
}

TEST(Config, MakeSyncLoggerFromConfig) {
    auto tmp = fs::temp_directory_path() / "ulog_cfg_sync.log";
    RemoveQuiet(tmp);

    {
        ulog::LoggerConfig cfg;
        cfg.file_path = tmp.string();
        cfg.format = ulog::Format::kTskv;
        cfg.level = ulog::Level::kTrace;
        cfg.truncate_on_start = true;

        auto logger = ulog::MakeSyncLogger(cfg);
        ulog::impl::SetDefaultLoggerRef(*logger);
        LOG_INFO() << "via-cfg";
        ulog::LogFlush();
        ulog::SetNullDefaultLogger();
    }

    std::ifstream f(tmp, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    EXPECT_NE(ss.str().find("text=via-cfg"), std::string::npos);

    RemoveQuiet(tmp);
}

TEST(Config, EmitLocationFalsePropagatesToSync) {
    auto tmp = fs::temp_directory_path() / "ulog_cfg_no_loc.log";
    RemoveQuiet(tmp);

    {
        ulog::LoggerConfig cfg;
        cfg.file_path = tmp.string();
        cfg.format = ulog::Format::kTskv;
        cfg.level = ulog::Level::kTrace;
        cfg.truncate_on_start = true;
        cfg.emit_location = false;

        auto logger = ulog::MakeSyncLogger(cfg);
        ulog::impl::SetDefaultLoggerRef(*logger);
        LOG_INFO() << "no-loc";
        ulog::LogFlush();
        ulog::SetNullDefaultLogger();
    }

    std::ifstream f(tmp, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const auto out = ss.str();
    EXPECT_NE(out.find("text=no-loc"), std::string::npos);
    EXPECT_EQ(out.find("module="), std::string::npos) << out;

    RemoveQuiet(tmp);
}

TEST(Config, InitDefaultLoggerAsync) {
    auto tmp = fs::temp_directory_path() / "ulog_cfg_async.log";
    RemoveQuiet(tmp);

    {
        ulog::LoggerConfig cfg;
        cfg.file_path = tmp.string();
        cfg.level = ulog::Level::kTrace;
        cfg.truncate_on_start = true;
        auto logger = ulog::InitDefaultLogger(cfg);

        LOG_INFO() << "async-cfg";
        ulog::LogFlush();
        ulog::SetNullDefaultLogger();
    }

    std::ifstream f(tmp, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    EXPECT_NE(ss.str().find("text=async-cfg"), std::string::npos);

    RemoveQuiet(tmp);
}
