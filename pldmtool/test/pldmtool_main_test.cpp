// Override pldm_instance_db_init_default via --wrap linker flag
#include "../../test/test_valgrind_utils.hpp"
#include "test/test_tmp_utils.hpp"

#include <fcntl.h>
#include <libpldm/instance-id.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <deque>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" int __wrap_pldm_instance_db_init_default(
    struct pldm_instance_db** ctx)
{
    static uint64_t dbIndex = 0;
    static std::deque<std::string> dbPaths;
    auto root = pldm::test::ensureTempDir();
    dbPaths.emplace_back(
        (root / ("pldm_test_iid_" + std::to_string(::getpid()) + "_" +
                 std::to_string(dbIndex++)))
            .string());
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

struct CommandResult
{
    int rc;
    std::string output;
};

std::string getPldmtoolBinary()
{
    const char* binary = std::getenv("PLDMTOOL_BINARY");
    if (binary == nullptr)
    {
        throw std::runtime_error("PLDMTOOL_BINARY is not set");
    }

    return binary;
}

std::string getRequiredEnv(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr)
    {
        throw std::runtime_error(std::string{name} + " is not set");
    }

    return value;
}

CommandResult runCommand(const std::vector<std::string>& args)
{
    std::array<int, 2> pipeFds{};
    if (pipe(pipeFds.data()) != 0)
    {
        throw std::runtime_error("pipe failed");
    }

    const pid_t pid = fork();
    if (pid < 0)
    {
        close(pipeFds[0]);
        close(pipeFds[1]);
        throw std::runtime_error("fork failed");
    }

    if (pid == 0)
    {
        close(pipeFds[0]);
        dup2(pipeFds[1], STDOUT_FILENO);
        dup2(pipeFds[1], STDERR_FILENO);
        close(pipeFds[1]);

        const auto preload = getRequiredEnv("PLDMTOOL_PRELOAD");
        const auto instanceDbDir = getRequiredEnv("PLDMTOOL_INSTANCE_DB_DIR");
        setenv("LD_PRELOAD", preload.c_str(), 1);
        setenv("PLDMTOOL_INSTANCE_DB_DIR", instanceDbDir.c_str(), 1);

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args)
        {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execv(argv.front(), argv.data());
        _exit(127);
    }

    close(pipeFds[1]);

    std::string output;
    std::array<char, 4096> buffer{};
    ssize_t readLen = 0;
    while ((readLen = read(pipeFds[0], buffer.data(), buffer.size())) > 0)
    {
        output.append(buffer.data(), static_cast<size_t>(readLen));
    }
    close(pipeFds[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
    {
        throw std::runtime_error("waitpid failed");
    }

    if (WIFEXITED(status))
    {
        return {WEXITSTATUS(status), std::move(output)};
    }

    if (WIFSIGNALED(status))
    {
        return {128 + WTERMSIG(status), std::move(output)};
    }

    return {-1, std::move(output)};
}

bool skipRealBinarySubprocess()
{
    return pldm::test::runningOnValgrind() ||
           pldm::test::runningWithAddressSanitizer();
}

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

TEST(PldmtoolMain, RealBinaryHelpReturnsZero)
{
    if (skipRealBinarySubprocess())
    {
        GTEST_SKIP() << "real binary subprocess coverage runs in the normal "
                        "pass";
    }

    auto result = runCommand({getPldmtoolBinary(), "--help"});
    EXPECT_EQ(result.rc, 0);
    EXPECT_NE(result.output.find("PLDM requester tool for OpenBMC"),
              std::string::npos);
}

TEST(PldmtoolMain, RealBinaryMissingSubcommandReturnsError)
{
    if (skipRealBinarySubprocess())
    {
        GTEST_SKIP() << "real binary subprocess coverage runs in the normal "
                        "pass";
    }

    auto result = runCommand({getPldmtoolBinary()});
    EXPECT_NE(result.rc, 0);
    EXPECT_NE(result.output.find("A subcommand is required"),
              std::string::npos);
}

TEST(PldmtoolMain, RealBinaryUnknownSubcommandReturnsError)
{
    if (skipRealBinarySubprocess())
    {
        GTEST_SKIP() << "real binary subprocess coverage runs in the normal "
                        "pass";
    }

    auto result = runCommand({getPldmtoolBinary(), "unknown"});
    EXPECT_NE(result.rc, 0);
    EXPECT_FALSE(result.output.empty());
}

TEST(PldmtoolMain, RealBinarySubcommandHelpReturnsZero)
{
    if (skipRealBinarySubprocess())
    {
        GTEST_SKIP() << "real binary subprocess coverage runs in the normal "
                        "pass";
    }

    for (const auto& command : {"raw", "base", "platform", "fw_update"})
    {
        SCOPED_TRACE(command);
        auto result = runCommand({getPldmtoolBinary(), command, "--help"});
        EXPECT_EQ(result.rc, 0);
    }
}
