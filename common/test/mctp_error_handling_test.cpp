#include "../../fw-update/config.hpp"
#include "../../fw-update/dbusutil.hpp"
#include "../mctp_error_handling.hpp"

#include <phosphor-logging/mctp_error_registry.hpp>

#include <cerrno>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace
{

struct LogCall
{
    std::string messageId;
    std::string messageArgs;
    std::string resolution;
    std::string logNamespace;
    sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level level;
};

struct RecvmsgScenario
{
    bool fail = false;
    int errorNo = 0;
    bool includeControl = false;
    int cmsgLevel = 0;
    int cmsgType = 0;
    pldm::transport::MctpError payload{};
};

RecvmsgScenario recvmsgScenario{};
std::optional<std::string> mockedDeviceName{};
mctp_eid_t queriedDeviceNameEid{};
bool mockedDeviceHasErrors = false;
size_t queryDeviceStatusCalls = 0;
std::vector<LogCall> logCalls{};

} // namespace

extern "C" ssize_t test_recvmsg(int fd, struct msghdr* msg, int flags);

namespace pldm::fw_update
{
std::optional<std::string> test_getDeviceNameFromEid(mctp_eid_t eid);
} // namespace pldm::fw_update

namespace pldm::transport
{
bool test_queryDeviceStatus(mctp_eid_t eid);
void test_createLogEntry(
    const std::string& messageID, const std::string& messageArgs,
    const std::string& resolution, const std::string& logNamespace,
    sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level level,
    const std::map<std::string, std::string>& extraData = {});
} // namespace pldm::transport

namespace phosphor::logging::mctp
{
std::optional<RedfishRegistry> test_errorToRedfishRegistry(
    uint32_t errorCode, Direction direction, Binding binding,
    uint8_t endpointid, const std::string& driverOperation,
    const std::optional<std::string>& deviceRedfishName);
} // namespace phosphor::logging::mctp

#define recvmsg test_recvmsg
#define getDeviceNameFromEid test_getDeviceNameFromEid
#define queryDeviceStatus test_queryDeviceStatus
#define createLogEntry test_createLogEntry
#define errorToRedfishRegistry test_errorToRedfishRegistry
#include "../mctp_error_handling.cpp" // NOLINT(bugprone-suspicious-include)
#undef errorToRedfishRegistry
#undef createLogEntry
#undef queryDeviceStatus
#undef getDeviceNameFromEid
#undef recvmsg

namespace
{

bool useMockedRegistry = false;
std::optional<phosphor::logging::mctp::RedfishRegistry> mockedRegistry{};

} // namespace

extern "C" ssize_t test_recvmsg(int fd, struct msghdr* msg, int flags)
{
    (void)fd;
    (void)flags;

    if (recvmsgScenario.fail)
    {
        errno = recvmsgScenario.errorNo;
        return -1;
    }

    auto* error =
        static_cast<pldm::transport::MctpError*>(msg->msg_iov[0].iov_base);
    *error = recvmsgScenario.payload;

    if (!recvmsgScenario.includeControl)
    {
        msg->msg_controllen = 0;
        return static_cast<ssize_t>(sizeof(recvmsgScenario.payload));
    }

    auto* cmsg = reinterpret_cast<struct cmsghdr*>(msg->msg_control);
    cmsg->cmsg_len = CMSG_LEN(0);
    cmsg->cmsg_level = recvmsgScenario.cmsgLevel;
    cmsg->cmsg_type = recvmsgScenario.cmsgType;
    msg->msg_controllen = CMSG_SPACE(0);

    return static_cast<ssize_t>(sizeof(recvmsgScenario.payload));
}

namespace pldm::fw_update
{

std::optional<std::string> test_getDeviceNameFromEid(mctp_eid_t eid)
{
    queriedDeviceNameEid = eid;
    return mockedDeviceName;
}

} // namespace pldm::fw_update

namespace pldm::transport
{

bool test_queryDeviceStatus(mctp_eid_t)
{
    ++queryDeviceStatusCalls;
    return mockedDeviceHasErrors;
}

void test_createLogEntry(
    const std::string& messageID, const std::string& messageArgs,
    const std::string& resolution, const std::string& logNamespace,
    sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level level,
    const std::map<std::string, std::string>&)
{
    logCalls.push_back(
        {messageID, messageArgs, resolution, logNamespace, level});
}

} // namespace pldm::transport

namespace phosphor::logging::mctp
{

std::optional<RedfishRegistry> test_errorToRedfishRegistry(
    uint32_t errorCode, Direction direction, Binding binding,
    uint8_t endpointid, const std::string& driverOperation,
    const std::optional<std::string>& deviceRedfishName)
{
    if (useMockedRegistry)
    {
        return mockedRegistry;
    }

    return errorToRedfishRegistry(errorCode, direction, binding, endpointid,
                                  driverOperation, deviceRedfishName);
}

} // namespace phosphor::logging::mctp

class MctpErrorHandlingTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        recvmsgScenario = {};
        mockedDeviceName.reset();
        queriedDeviceNameEid = 0;
        mockedDeviceHasErrors = false;
        queryDeviceStatusCalls = 0;
        logCalls.clear();
        useMockedRegistry = false;
        mockedRegistry.reset();
    }

    static pldm::transport::MctpError makeError(
        uint8_t payloadLen, uint8_t msgType = MCTP_MSG_TYPE_PLDM)
    {
        pldm::transport::MctpError error{};
        error.direction = MCTP_DIR_TX;
        error.src_eid = 8;
        error.dest_eid = 9;
        error.error_code = EHOSTUNREACH;
        error.payload_len = payloadLen;
        error.msg_type = msgType;
        for (size_t i = 0; i < MCTP_ERROR_PAYLOAD_SIZE; ++i)
        {
            error.payload[i] = static_cast<uint8_t>(i + 1);
        }
        return error;
    }
};

TEST_F(MctpErrorHandlingTest, readQueueReturnsEagainWhenNoQueuedError)
{
    recvmsgScenario.fail = true;
    recvmsgScenario.errorNo = EAGAIN;
    pldm::transport::MctpError error{};

    auto rc = pldm::transport::readMctpErrorQueue(0, error);
    EXPECT_EQ(rc, -EAGAIN);
}

TEST_F(MctpErrorHandlingTest, readQueueReturnsNegativeErrnoOnFailure)
{
    recvmsgScenario.fail = true;
    recvmsgScenario.errorNo = EPERM;
    pldm::transport::MctpError error{};

    auto rc = pldm::transport::readMctpErrorQueue(0, error);
    EXPECT_EQ(rc, -EPERM);
}

TEST_F(MctpErrorHandlingTest, readQueueReturnsEagainWhenControlMessageMissing)
{
    recvmsgScenario.includeControl = false;
    recvmsgScenario.payload = makeError(8);
    pldm::transport::MctpError error{};

    auto rc = pldm::transport::readMctpErrorQueue(0, error);
    EXPECT_EQ(rc, -EAGAIN);
}

TEST_F(MctpErrorHandlingTest, readQueueReturnsEagainForWrongControlType)
{
    recvmsgScenario.includeControl = true;
    recvmsgScenario.cmsgLevel = SOL_MCTP;
    recvmsgScenario.cmsgType = 0;
    recvmsgScenario.payload = makeError(8);
    pldm::transport::MctpError error{};

    auto rc = pldm::transport::readMctpErrorQueue(0, error);
    EXPECT_EQ(rc, -EAGAIN);
}

TEST_F(MctpErrorHandlingTest, readQueueReturnsEagainForWrongControlLevel)
{
    recvmsgScenario.includeControl = true;
    recvmsgScenario.cmsgLevel = 0;
    recvmsgScenario.cmsgType = MCTP_RECVERR;
    recvmsgScenario.payload = makeError(8);
    pldm::transport::MctpError error{};

    auto rc = pldm::transport::readMctpErrorQueue(0, error);
    EXPECT_EQ(rc, -EAGAIN);
}

TEST_F(MctpErrorHandlingTest, readQueueRejectsTooShortPayload)
{
    recvmsgScenario.includeControl = true;
    recvmsgScenario.cmsgLevel = SOL_MCTP;
    recvmsgScenario.cmsgType = MCTP_RECVERR;
    recvmsgScenario.payload = makeError(1);
    pldm::transport::MctpError error{};

    auto rc = pldm::transport::readMctpErrorQueue(0, error);
    EXPECT_EQ(rc, -EINVAL);
}

TEST_F(MctpErrorHandlingTest, readQueueSucceedsWithValidErrorRecord)
{
    recvmsgScenario.includeControl = true;
    recvmsgScenario.cmsgLevel = SOL_MCTP;
    recvmsgScenario.cmsgType = MCTP_RECVERR;
    recvmsgScenario.payload = makeError(64);
    pldm::transport::MctpError error{};

    auto rc = pldm::transport::readMctpErrorQueue(0, error);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(error.payload_len, 64);
    EXPECT_EQ(error.payload[0], 1);
}

TEST_F(MctpErrorHandlingTest, readQueueSucceedsWithSmallValidPayload)
{
    recvmsgScenario.includeControl = true;
    recvmsgScenario.cmsgLevel = SOL_MCTP;
    recvmsgScenario.cmsgType = MCTP_RECVERR;
    recvmsgScenario.payload = makeError(8);
    pldm::transport::MctpError error{};

    auto rc = pldm::transport::readMctpErrorQueue(0, error);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(error.payload_len, 8);
}

TEST_F(MctpErrorHandlingTest, extractPldmTypeReturnsUnknownForNonPldmMessage)
{
    auto error = makeError(4, 0);
    auto type = pldm::transport::extractPldmType(error);
    EXPECT_EQ(type, 0xFF);
}

TEST_F(MctpErrorHandlingTest, extractPldmTypeReturnsUnknownForShortPayload)
{
    auto error = makeError(1);
    auto type = pldm::transport::extractPldmType(error);
    EXPECT_EQ(type, 0xFF);
}

TEST_F(MctpErrorHandlingTest, extractPldmTypeReturnsMaskedType)
{
    auto error = makeError(4);
    error.payload[1] = 0xC5;
    auto type = pldm::transport::extractPldmType(error);
    EXPECT_EQ(type, 0x05);
}

TEST_F(MctpErrorHandlingTest,
       createRedfishEventSkipsSyncLogWhenDeviceAlreadyFaulted)
{
    mockedDeviceName = "GPU_0";
    mockedDeviceHasErrors = true;
    constexpr mctp_eid_t eid = 0x20;

    pldm::transport::createMctpTransportRedfishEvent(
        eid, "RequestUpdate", EHOSTUNREACH, MCTP_BINDING_UNKNOWN, MCTP_DIR_TX,
        "FWUpdate");

    EXPECT_EQ(queriedDeviceNameEid, eid);
    EXPECT_EQ(queryDeviceStatusCalls, 1u);
    EXPECT_TRUE(logCalls.empty());
}

TEST_F(MctpErrorHandlingTest, createRedfishEventLogsSyncErrorWhenDeviceHealthy)
{
    mockedDeviceName = "GPU_1";
    mockedDeviceHasErrors = false;

    pldm::transport::createMctpTransportRedfishEvent(
        0x21, "GetPDR", EHOSTUNREACH, MCTP_BINDING_UNKNOWN, MCTP_DIR_TX,
        "FWUpdate");

    EXPECT_EQ(queryDeviceStatusCalls, 1u);
    ASSERT_EQ(logCalls.size(), 1u);
    EXPECT_EQ(logCalls[0].logNamespace, "FWUpdate");
    EXPECT_EQ(logCalls[0].level, sdbusplus::xyz::openbmc_project::Logging::
                                     server::Entry::Level::Informational);
    EXPECT_NE(logCalls[0].messageArgs.find("GetPDR"), std::string::npos);
}

TEST_F(MctpErrorHandlingTest,
       createRedfishEventDoesNotQueryDeviceStatusForAsync)
{
    mockedDeviceName = "GPU_2";
    mockedDeviceHasErrors = true;

    pldm::transport::createMctpTransportRedfishEvent(
        0x22, "GetPDR", EBUSY,
        static_cast<uint8_t>(phosphor::logging::mctp::Binding::I2C),
        MCTP_DIR_TX, "FWUpdate");

    EXPECT_EQ(queryDeviceStatusCalls, 0u);
    ASSERT_EQ(logCalls.size(), 1u);
}

TEST_F(MctpErrorHandlingTest,
       createRedfishEventDoesNotQueryDeviceStatusForSyncHostControllerError)
{
    mockedDeviceName = "BMC";
    mockedDeviceHasErrors = true;

    pldm::transport::createMctpTransportRedfishEvent(
        0x23, "GetPDR", EBUSY, MCTP_BINDING_UNKNOWN, MCTP_DIR_TX, "FWUpdate");

    EXPECT_EQ(queryDeviceStatusCalls, 0u);
    ASSERT_EQ(logCalls.size(), 1u);
}

TEST_F(MctpErrorHandlingTest,
       createRedfishEventSkipsSyncLogWithMissingDeviceNameWhenFaulted)
{
    mockedDeviceName.reset();
    mockedDeviceHasErrors = true;
    constexpr mctp_eid_t eid = 0x24;

    pldm::transport::createMctpTransportRedfishEvent(
        eid, "RequestUpdate", EHOSTUNREACH, MCTP_BINDING_UNKNOWN, MCTP_DIR_TX,
        "FWUpdate");

    EXPECT_EQ(queriedDeviceNameEid, eid);
    EXPECT_EQ(queryDeviceStatusCalls, 1u);
    EXPECT_TRUE(logCalls.empty());
}

TEST_F(MctpErrorHandlingTest,
       createRedfishEventUsesFallbackDeviceNameWhenMissing)
{
    mockedDeviceName.reset();

    pldm::transport::createMctpTransportRedfishEvent(
        0x2A, "GetPDR", EBUSY,
        static_cast<uint8_t>(phosphor::logging::mctp::Binding::I2C),
        MCTP_DIR_TX, "FWUpdate");

    ASSERT_EQ(logCalls.size(), 1u);
    EXPECT_NE(logCalls[0].messageArgs.find("EID_0x2A"), std::string::npos);
}

TEST_F(MctpErrorHandlingTest, createRedfishEventSkipsWhenRegistryMissing)
{
    useMockedRegistry = true;
    mockedRegistry.reset();

    pldm::transport::createMctpTransportRedfishEvent(
        0x2B, "GetPDR", EHOSTUNREACH, MCTP_BINDING_UNKNOWN, MCTP_DIR_TX,
        "FWUpdate");

    EXPECT_EQ(queryDeviceStatusCalls, 0u);
    EXPECT_TRUE(logCalls.empty());
}

TEST_F(MctpErrorHandlingTest, createMctpErrorObjectSetsAllFields)
{
    constexpr mctp_eid_t destEid = 0x30;
    constexpr int errorCode = EHOSTUNREACH;
    constexpr uint8_t binding = 3;
    std::vector<uint8_t> payload = {0x01, 0x80, 0x02, 0x04};

    auto err = pldm::transport::createMctpErrorObject(destEid, errorCode,
                                                      binding, payload);

    EXPECT_EQ(err.msg_type, MCTP_MSG_TYPE_PLDM);
    EXPECT_EQ(err.direction, MCTP_DIR_TX);
    EXPECT_EQ(err.src_eid, 0);
    EXPECT_EQ(err.dest_eid, destEid);
    EXPECT_EQ(err.error_code, static_cast<uint32_t>(errorCode));
    EXPECT_EQ(err.binding, binding);
    EXPECT_EQ(err.timestamp_ns, 0u);
    EXPECT_EQ(err.payload_len, payload.size());
    EXPECT_EQ(err.payload[0], 0x01);
    EXPECT_EQ(err.payload[1], 0x80);
    EXPECT_EQ(err.payload[2], 0x02);
    EXPECT_EQ(err.payload[3], 0x04);
}

TEST_F(MctpErrorHandlingTest, createMctpErrorObjectTruncatesOversizedPayload)
{
    std::vector<uint8_t> payload(64, 0xAB);

    auto err = pldm::transport::createMctpErrorObject(0x10, EBUSY, 1, payload);

    EXPECT_EQ(err.payload_len, MCTP_ERROR_PAYLOAD_SIZE);
    for (size_t i = 0; i < MCTP_ERROR_PAYLOAD_SIZE; ++i)
    {
        EXPECT_EQ(err.payload[i], 0xAB);
    }
}

TEST_F(MctpErrorHandlingTest, createMctpErrorObjectHandlesEmptyPayload)
{
    std::vector<uint8_t> payload;

    auto err =
        pldm::transport::createMctpErrorObject(0x20, ETIMEDOUT, 2, payload);

    EXPECT_EQ(err.payload_len, 0);
    EXPECT_EQ(err.dest_eid, 0x20);
    EXPECT_EQ(err.error_code, static_cast<uint32_t>(ETIMEDOUT));
    EXPECT_EQ(err.binding, 2);
}
