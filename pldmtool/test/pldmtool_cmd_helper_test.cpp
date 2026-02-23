// Override pldm_instance_db_init_default via --wrap linker flag
// to use a temp file instead of /usr/share/libpldm/instance-db/default
#include <libpldm/instance-id.h>
#include <libpldm/transport.h>
#include <libpldm/transport/af-mctp.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
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

namespace
{

struct WrappedSendRecvResult
{
    pldm_requester_rc_t rc;
    std::vector<uint8_t> response;
};

std::deque<WrappedSendRecvResult> wrappedSendRecvResults{};

void clearWrappedSendRecvResults()
{
    wrappedSendRecvResults.clear();
}

void pushWrappedSendRecvResult(pldm_requester_rc_t rc,
                               const std::vector<uint8_t>& response = {})
{
    wrappedSendRecvResults.emplace_back(WrappedSendRecvResult{rc, response});
}

std::vector<uint8_t> makeWrappedResponse(size_t payloadSize)
{
    return std::vector<uint8_t>(sizeof(pldm_msg_hdr) + payloadSize, 0x5A);
}

} // namespace

extern "C" pldm_requester_rc_t __wrap_pldm_transport_send_recv_msg(
    struct pldm_transport*, pldm_tid_t, const void*, size_t,
    void** pldm_resp_msg, size_t* resp_msg_len)
{
    if (pldm_resp_msg == nullptr || resp_msg_len == nullptr)
    {
        return PLDM_REQUESTER_INVALID_SETUP;
    }

    if (wrappedSendRecvResults.empty())
    {
        *pldm_resp_msg = nullptr;
        *resp_msg_len = 0;
        return PLDM_REQUESTER_RECV_FAIL;
    }

    auto result = wrappedSendRecvResults.front();
    wrappedSendRecvResults.pop_front();

    if (result.rc != PLDM_REQUESTER_SUCCESS)
    {
        *pldm_resp_msg = nullptr;
        *resp_msg_len = 0;
        return result.rc;
    }

    auto* response = static_cast<uint8_t*>(
        malloc(static_cast<size_t>(result.response.size())));
    if (response == nullptr)
    {
        return PLDM_REQUESTER_INVALID_SETUP;
    }

    memcpy(response, result.response.data(), result.response.size());
    *pldm_resp_msg = response;
    *resp_msg_len = result.response.size();
    return PLDM_REQUESTER_SUCCESS;
}

extern "C" pldm_requester_rc_t pldm_transport_send_recv_msg(
    struct pldm_transport* transport, pldm_tid_t tid, const void* tx,
    size_t tx_len, void** pldm_resp_msg, size_t* resp_msg_len)
{
    return __wrap_pldm_transport_send_recv_msg(transport, tid, tx, tx_len,
                                               pldm_resp_msg, resp_msg_len);
}

extern "C" int __wrap_pldm_transport_af_mctp_init(
    struct pldm_transport_af_mctp** ctx)
{
    if (ctx == nullptr)
    {
        return -EINVAL;
    }

    *ctx = static_cast<pldm_transport_af_mctp*>(malloc(1));
    return *ctx == nullptr ? -ENOMEM : 0;
}

extern "C" int pldm_transport_af_mctp_init(struct pldm_transport_af_mctp** ctx)
{
    return __wrap_pldm_transport_af_mctp_init(ctx);
}

extern "C" void __wrap_pldm_transport_af_mctp_destroy(
    struct pldm_transport_af_mctp* ctx)
{
    free(ctx);
}

extern "C" void pldm_transport_af_mctp_destroy(
    struct pldm_transport_af_mctp* ctx)
{
    __wrap_pldm_transport_af_mctp_destroy(ctx);
}

extern "C" struct pldm_transport* __wrap_pldm_transport_af_mctp_core(
    struct pldm_transport_af_mctp* ctx)
{
    return reinterpret_cast<pldm_transport*>(ctx);
}

extern "C" struct pldm_transport* pldm_transport_af_mctp_core(
    struct pldm_transport_af_mctp* ctx)
{
    return __wrap_pldm_transport_af_mctp_core(ctx);
}

extern "C" int __wrap_pldm_transport_af_mctp_init_pollfd(struct pldm_transport*,
                                                         struct pollfd* pollfd)
{
    if (pollfd == nullptr)
    {
        return -EINVAL;
    }

    pollfd->fd = -1;
    pollfd->events = 0;
    pollfd->revents = 0;
    return 0;
}

extern "C" int pldm_transport_af_mctp_init_pollfd(struct pldm_transport* t,
                                                  struct pollfd* pollfd)
{
    return __wrap_pldm_transport_af_mctp_init_pollfd(t, pollfd);
}

extern "C" int __wrap_pldm_transport_af_mctp_map_tid(
    struct pldm_transport_af_mctp*, pldm_tid_t, mctp_eid_t)
{
    return 0;
}

extern "C" int pldm_transport_af_mctp_map_tid(
    struct pldm_transport_af_mctp* ctx, pldm_tid_t tid, mctp_eid_t eid)
{
    return __wrap_pldm_transport_af_mctp_map_tid(ctx, tid, eid);
}

extern "C" int __wrap_pldm_transport_af_mctp_bind(
    struct pldm_transport_af_mctp*, const struct sockaddr_mctp*, size_t)
{
    return 0;
}

extern "C" int pldm_transport_af_mctp_bind(
    struct pldm_transport_af_mctp* transport, const struct sockaddr_mctp* smctp,
    size_t len)
{
    return __wrap_pldm_transport_af_mctp_bind(transport, smctp, len);
}

// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../pldm_cmd_helper.cpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldmtool::helper;

// ===== fillCompletionCode Tests =====

TEST(FillCompletionCode, GenericSuccess)
{
    ordered_json data;
    fillCompletionCode(PLDM_SUCCESS, data, PLDM_BASE);
    EXPECT_EQ(data["CompletionCode"], "SUCCESS");
}

TEST(FillCompletionCode, GenericError)
{
    ordered_json data;
    fillCompletionCode(PLDM_ERROR, data, PLDM_BASE);
    EXPECT_EQ(data["CompletionCode"], "ERROR");
}

TEST(FillCompletionCode, GenericInvalidData)
{
    ordered_json data;
    fillCompletionCode(PLDM_ERROR_INVALID_DATA, data, PLDM_BASE);
    EXPECT_EQ(data["CompletionCode"], "ERROR_INVALID_DATA");
}

TEST(FillCompletionCode, GenericInvalidLength)
{
    ordered_json data;
    fillCompletionCode(PLDM_ERROR_INVALID_LENGTH, data, PLDM_BASE);
    EXPECT_EQ(data["CompletionCode"], "ERROR_INVALID_LENGTH");
}

TEST(FillCompletionCode, GenericNotReady)
{
    ordered_json data;
    fillCompletionCode(PLDM_ERROR_NOT_READY, data, PLDM_BASE);
    EXPECT_EQ(data["CompletionCode"], "ERROR_NOT_READY");
}

TEST(FillCompletionCode, GenericUnsupportedCmd)
{
    ordered_json data;
    fillCompletionCode(PLDM_ERROR_UNSUPPORTED_PLDM_CMD, data, PLDM_BASE);
    EXPECT_EQ(data["CompletionCode"], "ERROR_UNSUPPORTED_PLDM_CMD");
}

TEST(FillCompletionCode, GenericInvalidPldmType)
{
    ordered_json data;
    fillCompletionCode(PLDM_ERROR_INVALID_PLDM_TYPE, data, PLDM_BASE);
    EXPECT_EQ(data["CompletionCode"], "ERROR_INVALID_PLDM_TYPE");
}

TEST(FillCompletionCode, GenericUnexpectedTransferFlag)
{
    ordered_json data;
    fillCompletionCode(PLDM_ERROR_UNEXPECTED_TRANSFER_FLAG_OPERATION, data,
                       PLDM_BASE);
    EXPECT_EQ(data["CompletionCode"],
              "ERROR_UNEXPECTED_TRANSFER_FLAG_OPERATION");
}

TEST(FillCompletionCode, GenericCodeWithFwupType)
{
    // Generic codes should still work with FWUP type
    ordered_json data;
    fillCompletionCode(PLDM_SUCCESS, data, PLDM_FWUP);
    EXPECT_EQ(data["CompletionCode"], "SUCCESS");
}

TEST(FillCompletionCode, FwupNotInUpdateMode)
{
    ordered_json data;
    fillCompletionCode(PLDM_FWUP_NOT_IN_UPDATE_MODE, data, PLDM_FWUP);
    EXPECT_EQ(data["CompletionCode"], "NOT_IN_UPDATE_MODE");
}

TEST(FillCompletionCode, FwupAlreadyInUpdateMode)
{
    ordered_json data;
    fillCompletionCode(PLDM_FWUP_ALREADY_IN_UPDATE_MODE, data, PLDM_FWUP);
    EXPECT_EQ(data["CompletionCode"], "ALREADY_IN_UPDATE_MODE");
}

TEST(FillCompletionCode, FwupDataOutOfRange)
{
    ordered_json data;
    fillCompletionCode(PLDM_FWUP_DATA_OUT_OF_RANGE, data, PLDM_FWUP);
    EXPECT_EQ(data["CompletionCode"], "DATA_OUT_OF_RANGE");
}

TEST(FillCompletionCode, FwupInvalidTransferLength)
{
    ordered_json data;
    fillCompletionCode(PLDM_FWUP_INVALID_TRANSFER_LENGTH, data, PLDM_FWUP);
    EXPECT_EQ(data["CompletionCode"], "INVALID_TRANSFER_LENGTH");
}

TEST(FillCompletionCode, FwupInvalidStateForCommand)
{
    ordered_json data;
    fillCompletionCode(PLDM_FWUP_INVALID_STATE_FOR_COMMAND, data, PLDM_FWUP);
    EXPECT_EQ(data["CompletionCode"], "INVALID_STATE_FOR_COMMAND");
}

TEST(FillCompletionCode, FwupIncompleteUpdate)
{
    ordered_json data;
    fillCompletionCode(PLDM_FWUP_INCOMPLETE_UPDATE, data, PLDM_FWUP);
    EXPECT_EQ(data["CompletionCode"], "INCOMPLETE_UPDATE");
}

TEST(FillCompletionCode, FwupBusyInBackground)
{
    ordered_json data;
    fillCompletionCode(PLDM_FWUP_BUSY_IN_BACKGROUND, data, PLDM_FWUP);
    EXPECT_EQ(data["CompletionCode"], "BUSY_IN_BACKGROUND");
}

TEST(FillCompletionCode, FwupCancelPending)
{
    ordered_json data;
    fillCompletionCode(PLDM_FWUP_CANCEL_PENDING, data, PLDM_FWUP);
    EXPECT_EQ(data["CompletionCode"], "CANCEL_PENDING");
}

TEST(FillCompletionCode, FwupCommandNotExpected)
{
    ordered_json data;
    fillCompletionCode(PLDM_FWUP_COMMAND_NOT_EXPECTED, data, PLDM_FWUP);
    EXPECT_EQ(data["CompletionCode"], "COMMAND_NOT_EXPECTED");
}

TEST(FillCompletionCode, FwupRetryRequestFwData)
{
    ordered_json data;
    fillCompletionCode(PLDM_FWUP_RETRY_REQUEST_FW_DATA, data, PLDM_FWUP);
    EXPECT_EQ(data["CompletionCode"], "RETRY_REQUEST_FW_DATA");
}

TEST(FillCompletionCode, FwupUnableToInitiateUpdate)
{
    ordered_json data;
    fillCompletionCode(PLDM_FWUP_UNABLE_TO_INITIATE_UPDATE, data, PLDM_FWUP);
    EXPECT_EQ(data["CompletionCode"], "UNABLE_TO_INITIATE_UPDATE");
}

TEST(FillCompletionCode, FwupActivationNotRequired)
{
    ordered_json data;
    fillCompletionCode(PLDM_FWUP_ACTIVATION_NOT_REQUIRED, data, PLDM_FWUP);
    EXPECT_EQ(data["CompletionCode"], "ACTIVATION_NOT_REQUIRED");
}

TEST(FillCompletionCode, FwupSelfContainedActivNotPermitted)
{
    ordered_json data;
    fillCompletionCode(PLDM_FWUP_SELF_CONTAINED_ACTIVATION_NOT_PERMITTED, data,
                       PLDM_FWUP);
    EXPECT_EQ(data["CompletionCode"],
              "SELF_CONTAINED_ACTIVATION_NOT_PERMITTED");
}

TEST(FillCompletionCode, FwupNoDeviceMetadata)
{
    ordered_json data;
    fillCompletionCode(PLDM_FWUP_NO_DEVICE_METADATA, data, PLDM_FWUP);
    EXPECT_EQ(data["CompletionCode"], "NO_DEVICE_METADATA");
}

TEST(FillCompletionCode, FwupRetryRequestUpdate)
{
    ordered_json data;
    fillCompletionCode(PLDM_FWUP_RETRY_REQUEST_UPDATE, data, PLDM_FWUP);
    EXPECT_EQ(data["CompletionCode"], "RETRY_REQUEST_UPDATE");
}

TEST(FillCompletionCode, FwupNoPackageData)
{
    ordered_json data;
    fillCompletionCode(PLDM_FWUP_NO_PACKAGE_DATA, data, PLDM_FWUP);
    EXPECT_EQ(data["CompletionCode"], "NO_PACKAGE_DATA");
}

TEST(FillCompletionCode, FwupInvalidTransferHandle)
{
    ordered_json data;
    fillCompletionCode(PLDM_FWUP_INVALID_TRANSFER_HANDLE, data, PLDM_FWUP);
    EXPECT_EQ(data["CompletionCode"], "INVALID_TRANSFER_HANDLE");
}

TEST(FillCompletionCode, FwupInvalidTransferOperationFlag)
{
    ordered_json data;
    fillCompletionCode(PLDM_FWUP_INVALID_TRANSFER_OPERATION_FLAG, data,
                       PLDM_FWUP);
    EXPECT_EQ(data["CompletionCode"], "INVALID_TRANSFER_OPERATION_FLAG");
}

TEST(FillCompletionCode, FwupActivatePendingImageNotPermitted)
{
    ordered_json data;
    fillCompletionCode(PLDM_FWUP_ACTIVATE_PENDING_IMAGE_NOT_PERMITTED, data,
                       PLDM_FWUP);
    EXPECT_EQ(data["CompletionCode"], "ACTIVATE_PENDING_IMAGE_NOT_PERMITTED");
}

TEST(FillCompletionCode, FwupPackageDataError)
{
    ordered_json data;
    fillCompletionCode(PLDM_FWUP_PACKAGE_DATA_ERROR, data, PLDM_FWUP);
    EXPECT_EQ(data["CompletionCode"], "PACKAGE_DATA_ERROR");
}

TEST(FillCompletionCode, UnknownCodeNonFwup)
{
    ordered_json data;
    fillCompletionCode(0xFE, data, PLDM_BASE);
    EXPECT_EQ(data["CompletionCode"], "UNKNOWN_COMPLETION_CODE");
}

TEST(FillCompletionCode, UnknownCodeFwup)
{
    // A code that is not in generic or fwup maps
    ordered_json data;
    fillCompletionCode(0xFE, data, PLDM_FWUP);
    EXPECT_EQ(data["CompletionCode"], "UNKNOWN_COMPLETION_CODE");
}

TEST(FillCompletionCode, FwupCodeWithBaseType)
{
    // FWUP-specific code but with BASE type should be unknown
    ordered_json data;
    fillCompletionCode(PLDM_FWUP_NOT_IN_UPDATE_MODE, data, PLDM_BASE);
    EXPECT_EQ(data["CompletionCode"], "UNKNOWN_COMPLETION_CODE");
}

TEST(FillCompletionCode, GenericCodeWithPlatformType)
{
    // Generic codes work with any type
    ordered_json data;
    fillCompletionCode(PLDM_ERROR, data, PLDM_PLATFORM);
    EXPECT_EQ(data["CompletionCode"], "ERROR");
}

TEST(FillCompletionCode, UnknownCodeWithPlatformType)
{
    ordered_json data;
    fillCompletionCode(0xFE, data, PLDM_PLATFORM);
    EXPECT_EQ(data["CompletionCode"], "UNKNOWN_COMPLETION_CODE");
}

// ===== Logger Tests =====

TEST(Logger, VerboseTrue)
{
    testing::internal::CaptureStdout();
    Logger(true, "Test: ", 42);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("Test: 42"), std::string::npos);
}

TEST(Logger, VerboseFalse)
{
    testing::internal::CaptureStdout();
    Logger(false, "Test: ", 42);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_TRUE(output.empty());
}

TEST(Logger, StringData)
{
    testing::internal::CaptureStdout();
    Logger(true, "Msg: ", std::string("hello"));
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("Msg: hello"), std::string::npos);
}

// ===== DisplayInJson Tests =====

TEST(DisplayInJson, BasicOutput)
{
    ordered_json data;
    data["key"] = "value";
    testing::internal::CaptureStdout();
    DisplayInJson(data);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("\"key\": \"value\""), std::string::npos);
}

TEST(DisplayInJson, NumericOutput)
{
    ordered_json data;
    data["num"] = 123;
    testing::internal::CaptureStdout();
    DisplayInJson(data);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("\"num\": 123"), std::string::npos);
}

// ===== CommandInterface Tests =====

TEST(CommandInterface, GetMCTPEID)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");

    // Create a concrete subclass for testing
    class TestCmd : public CommandInterface
    {
      public:
        using CommandInterface::CommandInterface;
        std::pair<int, std::vector<uint8_t>> createRequestMsg() override
        {
            return {PLDM_SUCCESS, {}};
        }
        void parseResponseMsg(pldm_msg*, size_t) override {}
    };

    TestCmd cmd("base", "test", sub);
    // Default MCTP EID is PLDM_ENTITY_ID (8)
    EXPECT_EQ(cmd.getMCTPEID(), PLDM_ENTITY_ID);
}

TEST(CommandInterface, GetPLDMType)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");

    class TestCmd : public CommandInterface
    {
      public:
        using CommandInterface::CommandInterface;
        std::pair<int, std::vector<uint8_t>> createRequestMsg() override
        {
            return {PLDM_SUCCESS, {}};
        }
        void parseResponseMsg(pldm_msg*, size_t) override {}
    };

    TestCmd cmd("platform", "testCmd", sub);
    EXPECT_EQ(cmd.getPLDMType(), "platform");
}

TEST(CommandInterface, GetCommandName)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");

    class TestCmd : public CommandInterface
    {
      public:
        using CommandInterface::CommandInterface;
        std::pair<int, std::vector<uint8_t>> createRequestMsg() override
        {
            return {PLDM_SUCCESS, {}};
        }
        void parseResponseMsg(pldm_msg*, size_t) override {}
    };

    TestCmd cmd("base", "GetTID", sub);
    EXPECT_EQ(cmd.getCommandName(), "GetTID");
}

TEST(CommandInterface, GetPLDMTypeMultiple)
{
    CLI::App app{"test"};

    class TestCmd : public CommandInterface
    {
      public:
        using CommandInterface::CommandInterface;
        std::pair<int, std::vector<uint8_t>> createRequestMsg() override
        {
            return {PLDM_SUCCESS, {}};
        }
        void parseResponseMsg(pldm_msg*, size_t) override {}
    };

    auto sub1 = app.add_subcommand("t1", "t1");
    TestCmd cmd1("fw_update", "GetStatus", sub1);
    EXPECT_EQ(cmd1.getPLDMType(), "fw_update");

    auto sub2 = app.add_subcommand("t2", "t2");
    TestCmd cmd2("bios", "GetDateTime", sub2);
    EXPECT_EQ(cmd2.getPLDMType(), "bios");
}

TEST(CommandInterface, ExecRequestEncodeFailure)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");

    class TestCmd : public CommandInterface
    {
      public:
        using CommandInterface::CommandInterface;
        std::pair<int, std::vector<uint8_t>> createRequestMsg() override
        {
            return {PLDM_ERROR_INVALID_DATA, {}};
        }
        void parseResponseMsg(pldm_msg*, size_t) override {}
    };

    TestCmd cmd("base", "GetTID", sub);
    testing::internal::CaptureStderr();
    cmd.exec();
    std::string errOutput = testing::internal::GetCapturedStderr();

    EXPECT_NE(errOutput.find("Failed to encode request message"),
              std::string::npos);
}

TEST(CommandInterface, ExecSendRecvFailure)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");

    class TestCmd : public CommandInterface
    {
      public:
        using CommandInterface::CommandInterface;
        bool parsed = false;

        std::pair<int, std::vector<uint8_t>> createRequestMsg() override
        {
            std::vector<uint8_t> requestMsg(sizeof(pldm_msg_hdr), 0);
            auto* request = reinterpret_cast<pldm_msg*>(requestMsg.data());
            auto rc = encode_pldm_header_only(PLDM_REQUEST, 0, PLDM_BASE,
                                              PLDM_GET_TID, request);
            return {rc, requestMsg};
        }
        void parseResponseMsg(pldm_msg*, size_t) override
        {
            parsed = true;
        }
    };

    TestCmd cmd("base", "GetTID", sub);
    testing::internal::CaptureStderr();
    try
    {
        cmd.exec();
    }
    catch (const std::exception&)
    {
        testing::internal::GetCapturedStderr();
        SUCCEED();
        return;
    }
    std::string errOutput = testing::internal::GetCapturedStderr();

    EXPECT_FALSE(cmd.parsed);
    EXPECT_NE(errOutput.find("pldmSendRecv"), std::string::npos);
}

TEST(CommandInterface, PldmSendRecvRawTypeVerbosePath)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");

    class TestCmd : public CommandInterface
    {
      public:
        using CommandInterface::CommandInterface;
        std::pair<int, std::vector<uint8_t>> createRequestMsg() override
        {
            return {PLDM_SUCCESS, {}};
        }
        void parseResponseMsg(pldm_msg*, size_t) override {}
    };

    TestCmd cmd("raw", "raw", sub);
    std::vector<uint8_t> requestMsg(sizeof(pldm_msg_hdr), 0);
    std::vector<uint8_t> responseMsg;

    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    int rc = PLDM_ERROR;
    try
    {
        rc = cmd.pldmSendRecv(requestMsg, responseMsg);
    }
    catch (const std::exception&)
    {
        testing::internal::GetCapturedStdout();
        testing::internal::GetCapturedStderr();
        SUCCEED();
        return;
    }
    auto out = testing::internal::GetCapturedStdout();
    auto err = testing::internal::GetCapturedStderr();

    EXPECT_NE(out.find("pldmSendRecv"), std::string::npos);
    EXPECT_NE(rc, PLDM_SUCCESS);
    EXPECT_NE(err.find("failed to pldm send recv"), std::string::npos);
}

namespace
{

class WrappedCommandInterfaceTestCmd : public CommandInterface
{
  public:
    using CommandInterface::CommandInterface;

    void setRetryCount(uint8_t retries)
    {
        numRetries = retries;
    }

    std::pair<int, std::vector<uint8_t>> createRequestMsg() override
    {
        return {PLDM_SUCCESS, std::vector<uint8_t>(sizeof(pldm_msg_hdr), 0x11)};
    }

    void parseResponseMsg(pldm_msg*, size_t payloadLength) override
    {
        parsed = true;
        parsedPayloadLength = payloadLength;
    }

    bool parsed = false;
    size_t parsedPayloadLength = 0;
};

template <typename Tag, typename Tag::type member>
struct PrivateMethodAccessor
{
    friend typename Tag::type get(Tag)
    {
        return member;
    }
};

struct GetMctpServicesTag
{
    using type = std::set<pldm::dbus::Service> (CommandInterface::*)() const;
    friend type get(GetMctpServicesTag);
};

struct GetMctpManagedObjectsTag
{
    using type = pldm::dbus::ObjectValueTree (CommandInterface::*)(
        const std::string&) const noexcept;
    friend type get(GetMctpManagedObjectsTag);
};

template struct PrivateMethodAccessor<GetMctpServicesTag,
                                      &CommandInterface::getMctpServices>;
template struct PrivateMethodAccessor<GetMctpManagedObjectsTag,
                                      &CommandInterface::getMctpManagedObjects>;

bool defaultBusAvailable()
{
    try
    {
        (void)pldm::utils::DBusHandler::getBus();
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

} // namespace

TEST(CommandInterface, ExecSendRecvSuccessInvokesParseResponse)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");

    WrappedCommandInterfaceTestCmd cmd("base", "GetTID", sub);
    clearWrappedSendRecvResults();
    pushWrappedSendRecvResult(PLDM_REQUESTER_SUCCESS, makeWrappedResponse(3));

    try
    {
        cmd.exec();
    }
    catch (const std::exception&)
    {
        GTEST_SKIP() << "Transport init is unavailable in this environment";
    }

    EXPECT_TRUE(cmd.parsed);
    EXPECT_EQ(cmd.parsedPayloadLength, 3u);
}

TEST(CommandInterface, PldmSendRecvRetriesThenSucceeds)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");

    WrappedCommandInterfaceTestCmd cmd("base", "GetTID", sub);
    cmd.setRetryCount(1);

    std::vector<uint8_t> requestMsg(sizeof(pldm_msg_hdr), 0x01);
    std::vector<uint8_t> responseMsg;
    auto expectedResponse = makeWrappedResponse(2);

    clearWrappedSendRecvResults();
    pushWrappedSendRecvResult(PLDM_REQUESTER_RECV_FAIL);
    pushWrappedSendRecvResult(PLDM_REQUESTER_SUCCESS, expectedResponse);

    testing::internal::CaptureStderr();
    int rc = PLDM_ERROR;
    try
    {
        rc = cmd.pldmSendRecv(requestMsg, responseMsg);
    }
    catch (const std::exception&)
    {
        testing::internal::GetCapturedStderr();
        GTEST_SKIP() << "Transport init is unavailable in this environment";
    }
    auto errOutput = testing::internal::GetCapturedStderr();

    EXPECT_EQ(rc, PLDM_REQUESTER_SUCCESS);
    EXPECT_EQ(responseMsg, expectedResponse);
    EXPECT_NE(errOutput.find("pldm_send_recv error rc"), std::string::npos);
}

TEST(CommandInterface, PrivateGetMctpServicesPath)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");

    if (!defaultBusAvailable())
    {
        GTEST_SKIP() << "D-Bus is unavailable in this environment";
    }

    WrappedCommandInterfaceTestCmd cmd("base", "GetTID", sub);
    auto accessor = get(GetMctpServicesTag{});
    auto services = (cmd.*accessor)();

    EXPECT_GE(services.size(), 0u);
}

TEST(CommandInterface, PrivateGetMctpManagedObjectsPath)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");

    if (!defaultBusAvailable())
    {
        GTEST_SKIP() << "D-Bus is unavailable in this environment";
    }

    WrappedCommandInterfaceTestCmd cmd("base", "GetTID", sub);
    auto accessor = get(GetMctpManagedObjectsTag{});
    auto objects = (cmd.*accessor)("invalid.service.for.coverage");

    EXPECT_TRUE(objects.empty());
}
