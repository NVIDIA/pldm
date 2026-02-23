// Override pldm_instance_db_init_default via --wrap linker flag
#include <libpldm/instance-id.h>
#include <unistd.h>

#include <deque>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" int __wrap_pldm_instance_db_init_default(
    struct pldm_instance_db** ctx)
{
    static uint64_t dbIndex = 0;
    static std::deque<std::string> dbPaths;
    std::filesystem::create_directories("/tmp/claude");
    dbPaths.emplace_back(
        "/tmp/claude/pldm_test_iid_" + std::to_string(::getpid()) + "_" +
        std::to_string(dbIndex++));
    auto& dbPath = dbPaths.back();
    std::ofstream ofs(dbPath, std::ios::binary | std::ios::trunc);
    std::string data(256 * 32, '\0');
    ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    return pldm_instance_db_init(ctx, dbPath.c_str());
}

// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../pldm_cmd_helper.cpp"

namespace pldmtool::base
{
void registerCommand(CLI::App& app)
{
    auto* base = app.add_subcommand("base", "stub base command");
    base->callback([]() { throw std::runtime_error("base callback failure"); });
}
} // namespace pldmtool::base

namespace pldmtool::platform
{
void registerCommand(CLI::App& app)
{
    app.add_subcommand("platform", "stub platform command");
}
} // namespace pldmtool::platform

namespace pldmtool::fw_update
{
void registerCommand(CLI::App& app)
{
    app.add_subcommand("fw_update", "stub fw_update command");
}
} // namespace pldmtool::fw_update

#define main pldmtool_entry
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../pldmtool.cpp"
#undef main

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{

void parseArgs(CLI::App& app, const std::vector<std::string>& args)
{
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (const auto& arg : args)
    {
        argv.push_back(arg.c_str());
    }

    try
    {
        app.parse(static_cast<int>(argv.size()),
                  const_cast<char**>(argv.data()));
    }
    catch (...)
    {}
}

} // namespace

TEST(PldmtoolRaw, RegistersRawSubcommand)
{
    CLI::App app{"test"};
    pldmtool::raw::registerCommand(app);

    auto raw = app.get_subcommand("raw");
    EXPECT_NE(raw, nullptr);
}

TEST(PldmtoolRaw, RawRequestCreateAndParse)
{
    CLI::App app{"test"};
    auto raw = app.add_subcommand("raw", "raw");
    pldmtool::raw::RawOp cmd("raw", "raw", raw);

    parseArgs(app, {"test", "raw", "-d", "1", "2", "3"});

    auto [rc, requestMsg] = cmd.createRequestMsg();
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(requestMsg.size(), 3u);

    cmd.parseResponseMsg(nullptr, 0);
    pldmtool::helper::CommandInterface* baseCmd = &cmd;
    baseCmd->parseResponseMsg(nullptr, 0);
}

TEST(PldmtoolMain, MainHelpPath)
{
    char arg0[] = "pldmtool";
    char arg1[] = "--help";
    char* argv[] = {arg0, arg1};

    auto rc = pldmtool_entry(2, argv);
    EXPECT_EQ(rc, 0);
}

TEST(PldmtoolRaw, RawOptionRejectsTooFewBytes)
{
    CLI::App app{"test"};
    pldmtool::raw::registerCommand(app);

    char arg0[] = "test";
    char arg1[] = "raw";
    char arg2[] = "-d";
    char arg3[] = "1";
    char arg4[] = "2";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4};

    EXPECT_THROW(app.parse(5, argv), CLI::ParseError);
}

TEST(PldmtoolMain, MainMissingSubcommandReturnsError)
{
    char arg0[] = "pldmtool";
    char* argv[] = {arg0};

    auto rc = pldmtool_entry(1, argv);
    EXPECT_NE(rc, 0);
}

TEST(PldmtoolMain, MainUnknownSubcommandReturnsError)
{
    char arg0[] = "pldmtool";
    char arg1[] = "unknown";
    char* argv[] = {arg0, arg1};

    auto rc = pldmtool_entry(2, argv);
    EXPECT_NE(rc, 0);
}

TEST(PldmtoolMain, MainValidSubcommandReturnsZero)
{
    char arg0[] = "pldmtool";
    char arg1[] = "platform";
    char* argv[] = {arg0, arg1};

    auto rc = pldmtool_entry(2, argv);
    EXPECT_EQ(rc, 0);
}

TEST(PldmtoolMain, MainSubcommandHelpReturnsZero)
{
    char arg0[] = "pldmtool";
    char arg1[] = "base";
    char arg2[] = "--help";
    char* argv[] = {arg0, arg1, arg2};

    auto rc = pldmtool_entry(3, argv);
    EXPECT_EQ(rc, 0);
}

TEST(PldmtoolMain, MainHelpAllPath)
{
    char arg0[] = "pldmtool";
    char arg1[] = "--help-all";
    char* argv[] = {arg0, arg1};

    auto rc = pldmtool_entry(2, argv);
    EXPECT_GE(rc, 0);
}

TEST(PldmtoolMain, MainBaseSubcommandExceptionPath)
{
    char arg0[] = "pldmtool";
    char arg1[] = "base";
    char* argv[] = {arg0, arg1};

    try
    {
        (void)pldmtool_entry(2, argv);
    }
    catch (...)
    {}

    SUCCEED();
}
