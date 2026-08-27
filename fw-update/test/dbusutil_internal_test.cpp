/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "common/utils.hpp"

#include <boost/system/error_code.hpp>

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace dbusutil_test
{

using StatusValue = std::variant<pldm::fw_update::DeviceStatusMap>;

struct FakeMethodCall
{
    template <typename... Args>
    void append(Args&&...)
    {}
};

struct FakeReply
{
    StatusValue deviceStatusValue{};
    std::map<std::string, std::vector<std::string>> mapperResponse{
        {"xyz.openbmc_project.Logging", {"com.nvidia.State.DeviceState"}}};

    template <typename T>
    void read(T& out)
    {
        if constexpr (std::is_same_v<T, StatusValue>)
        {
            out = deviceStatusValue;
        }
        else if constexpr (std::is_same_v<
                               T,
                               std::map<std::string, std::vector<std::string>>>)
        {
            out = mapperResponse;
        }
    }
};

struct FakeBus
{
    bool throwOnCall = false;
    bool throwOnNewMethodCall = false;
    StatusValue deviceStatusValue{};
    std::map<std::string, std::vector<std::string>> mapperResponse{
        {"xyz.openbmc_project.Logging", {"com.nvidia.State.DeviceState"}}};

    FakeMethodCall new_method_call(const char*, const char*, const char*,
                                   const char*)
    {
        if (throwOnNewMethodCall)
        {
            throw std::runtime_error("FakeBus new_method_call failure");
        }
        return {};
    }

    FakeReply call(const FakeMethodCall&)
    {
        if (throwOnCall)
        {
            throw std::runtime_error("FakeBus call failure");
        }

        FakeReply reply{};
        reply.deviceStatusValue = deviceStatusValue;
        reply.mapperResponse = mapperResponse;
        return reply;
    }

    void call_noreply(const FakeMethodCall&) {}
};

struct FakeAsioConnection
{
    boost::system::error_code nextError{};
    std::string lastMessageId{};
    std::string lastSeverity{};
    std::map<std::string, std::string> lastAddData{};

    template <typename Callback>
    void async_method_call(Callback&& cb, const char*, const char*, const char*,
                           const char*, const std::string& messageId,
                           const std::string& severity,
                           const std::map<std::string, std::string>& addData)
    {
        lastMessageId = messageId;
        lastSeverity = severity;
        lastAddData = addData;
        cb(nextError);
    }
};

} // namespace dbusutil_test

namespace pldm::utils
{

class DBusUtilMockHandler : public DBusHandler
{
  public:
    static dbusutil_test::FakeBus& fakeBus()
    {
        static dbusutil_test::FakeBus bus{};
        return bus;
    }

    static std::shared_ptr<dbusutil_test::FakeAsioConnection>& fakeConn()
    {
        static auto conn =
            std::make_shared<dbusutil_test::FakeAsioConnection>();
        return conn;
    }

    static auto& getBus()
    {
        return fakeBus();
    }

    static auto& getAsioConnection()
    {
        return fakeConn();
    }
};

} // namespace pldm::utils

#define DBusHandler DBusUtilMockHandler
#include "fw-update/dbusutil.hpp"
#undef DBusHandler

namespace
{

using DeviceState = pldm::fw_update::DeviceState;

pldm::fw_update::DeviceStatusMap makeStatusMap(
    DeviceState::DeviceHealth health,
    std::vector<
        std::tuple<pldm::fw_update::DeviceStatusErrorCode,
                   DeviceState::ErrorClass, pldm::fw_update::AdditionalData>>
        errors)
{
    return {{DeviceState::StatusType::Communication,
             std::make_tuple(health, std::move(errors))}};
}

class DBusUtilInternalTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        auto& bus = pldm::utils::DBusUtilMockHandler::fakeBus();
        bus.throwOnCall = false;
        bus.throwOnNewMethodCall = false;
        bus.deviceStatusValue = pldm::fw_update::DeviceStatusMap{};
        bus.mapperResponse = {
            {"xyz.openbmc_project.Logging", {"com.nvidia.State.DeviceState"}}};

        auto& conn = pldm::utils::DBusUtilMockHandler::fakeConn();
        conn->nextError.clear();
        conn->lastMessageId.clear();
        conn->lastSeverity.clear();
        conn->lastAddData.clear();
    }
};

TEST_F(DBusUtilInternalTest, createLogEntrySeverityCallbackErrorPath)
{
    auto& conn = pldm::utils::DBusUtilMockHandler::fakeConn();
    conn->nextError =
        boost::system::errc::make_error_code(boost::system::errc::io_error);

    createLogEntry("Update.1.0.TransferFailed", "comp,1.0", "Retry", "FWUpdate",
                   sdbusplus::xyz::openbmc_project::Logging::server::Entry::
                       Level::Critical);
}

TEST_F(DBusUtilInternalTest, createLogEntryMessageIdBranchCoverage)
{
    createLogEntry(targetDetermined, "C0", "1.0", "");
    createLogEntry(transferFailed, "C0", "1.0", "Retry");
    createLogEntry(transferringToComponent, "C0", "1.0", "");
    createLogEntry(resourceErrorDetected, "Service", "Timeout", "Retry",
                   "FWUpdate", false);
    createLogEntry(resourceErrorDetected, "Service", "Timeout", "Retry",
                   "FWUpdate", true);
    createLogEntry("Vendor.Custom.Unknown", "A", "B", "Retry");
}

TEST_F(DBusUtilInternalTest, createLogEntryNvidiaMessageIdsBranchCoverage)
{
    createLogEntry(componentUpdateTime, "GPU0", "1250", "");
    createLogEntry(activateSuccessful, "GPU0", "1.2.3", "");
    createLogEntry(debugTokenEraseFailed, "GPU0", "1.2.3", "Retry");
    createLogEntry(awaitToActivate, "GPU0", "1.2.3", "");
    createLogEntry(applyFailed, "GPU0", "1.2.3", "Retry");
    createLogEntry(activateFailed, "GPU0", "1.2.3", "Retry");
}

TEST_F(DBusUtilInternalTest,
       createLogEntryPreUpdateValidationMessageIdsBranchCoverage)
{
    constexpr auto critical =
        "xyz.openbmc_project.Logging.Entry.Level.Critical";
    auto& conn = pldm::utils::DBusUtilMockHandler::fakeConn();

    createLogEntry(firmwarePackageComponentImageMissing, "HGX_FW_CPLD_0", "",
                   "");
    EXPECT_EQ(conn->lastMessageId, firmwarePackageComponentImageMissing);
    EXPECT_EQ(conn->lastSeverity, critical);
    EXPECT_EQ(conn->lastAddData["REDFISH_MESSAGE_ARGS"], "HGX_FW_CPLD_0");

    createLogEntry(preUpdateValidationFailed, "", "", "");
    EXPECT_EQ(conn->lastMessageId, preUpdateValidationFailed);
    EXPECT_EQ(conn->lastSeverity, critical);
    EXPECT_EQ(conn->lastAddData["REDFISH_MESSAGE_ARGS"], "");
}

TEST_F(DBusUtilInternalTest, queryDeviceStatusErrorReturnsEmptyOnCallFailure)
{
    auto& bus = pldm::utils::DBusUtilMockHandler::fakeBus();
    bus.throwOnCall = true;

    EXPECT_TRUE(queryDeviceStatusError(8).empty());
}

TEST_F(DBusUtilInternalTest, queryDeviceStatusErrorReturnsEmptyWhenMissingComm)
{
    auto& bus = pldm::utils::DBusUtilMockHandler::fakeBus();
    bus.deviceStatusValue = pldm::fw_update::DeviceStatusMap{};

    EXPECT_TRUE(queryDeviceStatusError(8).empty());
}

TEST_F(DBusUtilInternalTest, queryDeviceStatusErrorReturnsEmptyWhenHealthy)
{
    auto& bus = pldm::utils::DBusUtilMockHandler::fakeBus();
    bus.deviceStatusValue = makeStatusMap(
        DeviceState::DeviceHealth::Healthy,
        {{1,
          DeviceState::ErrorClass::MCTP,
          {{"REDFISH_MESSAGE_ID", "Update.1.0.TransferFailed"}}}});

    EXPECT_TRUE(queryDeviceStatusError(8).empty());
}

TEST_F(DBusUtilInternalTest, queryDeviceStatusErrorReturnsEmptyWhenNoErrors)
{
    auto& bus = pldm::utils::DBusUtilMockHandler::fakeBus();
    bus.deviceStatusValue =
        makeStatusMap(DeviceState::DeviceHealth::Degraded, {});

    EXPECT_TRUE(queryDeviceStatusError(8).empty());
}

TEST_F(DBusUtilInternalTest, queryDeviceStatusErrorParsesAdditionalData)
{
    auto& bus = pldm::utils::DBusUtilMockHandler::fakeBus();
    bus.deviceStatusValue = makeStatusMap(
        DeviceState::DeviceHealth::Degraded,
        {{7,
          DeviceState::ErrorClass::Power,
          {{"REDFISH_MESSAGE_ID", "Update.1.0.TransferFailed"},
           {"REDFISH_MESSAGE_ARGS", "GPU0, 1.0 "},
           {"REDFISH_RESOLUTION", "Retry"}}},
         {8,
          DeviceState::ErrorClass::Recovery,
          {{"REDFISH_MESSAGE_ID", "Update.1.0.AwaitToActivate"},
           {"REDFISH_MESSAGE_ARGS", "OnlyOneArg"}}},
         {9, DeviceState::ErrorClass::MCTP, {{"IGNORED", "X"}}}});

    const auto infos = queryDeviceStatusError(8);
    ASSERT_EQ(infos.size(), 2);

    EXPECT_EQ(infos[0].messageId, "Update.1.0.TransferFailed");
    EXPECT_EQ(infos[0].arg0, "GPU0");
    EXPECT_EQ(infos[0].arg1, "1.0");
    EXPECT_EQ(infos[0].resolution, "Retry");

    EXPECT_EQ(infos[1].messageId, "Update.1.0.AwaitToActivate");
    EXPECT_EQ(infos[1].arg0, "");
    EXPECT_EQ(infos[1].arg1, "OnlyOneArg");
}

TEST_F(DBusUtilInternalTest, queryDeviceStatusAndLogAndBooleanHelpers)
{
    auto& bus = pldm::utils::DBusUtilMockHandler::fakeBus();
    bus.deviceStatusValue = makeStatusMap(
        DeviceState::DeviceHealth::Degraded,
        {{1,
          DeviceState::ErrorClass::MCTP,
          {{"REDFISH_MESSAGE_ID", "Update.1.0.TransferFailed"},
           {"REDFISH_MESSAGE_ARGS", "GPU0,1.0"}}}});

    EXPECT_TRUE(queryDeviceStatus(8));
    EXPECT_TRUE(queryDeviceStatusAndLog(8));

    bus.deviceStatusValue =
        makeStatusMap(DeviceState::DeviceHealth::Healthy, {});
    EXPECT_FALSE(queryDeviceStatus(8));
    EXPECT_FALSE(queryDeviceStatusAndLog(8));
}

TEST_F(DBusUtilInternalTest,
       queryDeviceStatusErrorParsesFirstCommaAndTrimsOnlySecondArgument)
{
    auto& bus = pldm::utils::DBusUtilMockHandler::fakeBus();
    bus.deviceStatusValue = makeStatusMap(
        DeviceState::DeviceHealth::Degraded,
        {{11,
          DeviceState::ErrorClass::MCTP,
          {{"REDFISH_MESSAGE_ID", "Update.1.0.TransferFailed"},
           {"REDFISH_MESSAGE_ARGS", "GPU0,  retry, later  "}}}});

    const auto infos = queryDeviceStatusError(8);
    ASSERT_EQ(infos.size(), 1u);
    EXPECT_EQ(infos[0].arg0, "GPU0");
    EXPECT_EQ(infos[0].arg1, "retry, later");
    EXPECT_TRUE(infos[0].resolution.empty());
}

TEST_F(DBusUtilInternalTest, queryDeviceStatusErrorTrimsBlankSecondArgument)
{
    auto& bus = pldm::utils::DBusUtilMockHandler::fakeBus();
    bus.deviceStatusValue = makeStatusMap(
        DeviceState::DeviceHealth::Degraded,
        {{12,
          DeviceState::ErrorClass::Power,
          {{"REDFISH_MESSAGE_ID", "Update.1.0.TransferFailed"},
           {"REDFISH_MESSAGE_ARGS", "GPU0,   "},
           {"REDFISH_RESOLUTION", "Retry"}}}});

    const auto infos = queryDeviceStatusError(8);
    ASSERT_EQ(infos.size(), 1u);
    EXPECT_EQ(infos[0].arg0, "GPU0");
    EXPECT_TRUE(infos[0].arg1.empty());
    EXPECT_EQ(infos[0].resolution, "Retry");
}

TEST_F(DBusUtilInternalTest,
       queryDeviceStatusErrorHandlesPresentButEmptyMessageArgs)
{
    auto& bus = pldm::utils::DBusUtilMockHandler::fakeBus();
    bus.deviceStatusValue = makeStatusMap(
        DeviceState::DeviceHealth::Degraded,
        {{15,
          DeviceState::ErrorClass::Recovery,
          {{"REDFISH_MESSAGE_ID", "Update.1.0.AwaitToActivate"},
           {"REDFISH_MESSAGE_ARGS", ""}}}});

    const auto infos = queryDeviceStatusError(8);
    ASSERT_EQ(infos.size(), 1u);
    EXPECT_EQ(infos[0].messageId, "Update.1.0.AwaitToActivate");
    EXPECT_TRUE(infos[0].arg0.empty());
    EXPECT_TRUE(infos[0].arg1.empty());
    EXPECT_TRUE(infos[0].resolution.empty());
}

TEST_F(DBusUtilInternalTest,
       queryDeviceStatusErrorKeepsOnlyEntriesWithMessageIdsAcrossMultipleErrors)
{
    auto& bus = pldm::utils::DBusUtilMockHandler::fakeBus();
    const std::vector<
        std::tuple<pldm::fw_update::DeviceStatusErrorCode,
                   DeviceState::ErrorClass, pldm::fw_update::AdditionalData>>
        errors{
            {16,
             DeviceState::ErrorClass::Recovery,
             {{"REDFISH_MESSAGE_ARGS", "ignored"},
              {"REDFISH_RESOLUTION", "IgnoredResolution"}}},
            {17,
             DeviceState::ErrorClass::Power,
             {{"REDFISH_MESSAGE_ID", "Update.1.0.TransferFailed"},
              {"REDFISH_MESSAGE_ARGS", "GPU17"},
              {"REDFISH_RESOLUTION", "RetryPower"}}},
            {18,
             DeviceState::ErrorClass::MCTP,
             {{"REDFISH_MESSAGE_ID", "Update.1.0.ComponentUpdateTime"},
              {"REDFISH_MESSAGE_ARGS", "GPU18, 1200"}}},
        };
    bus.deviceStatusValue =
        makeStatusMap(DeviceState::DeviceHealth::Degraded, errors);

    const auto infos = queryDeviceStatusError(8);
    ASSERT_EQ(infos.size(), 2u);
    EXPECT_EQ(infos[0].messageId, "Update.1.0.TransferFailed");
    EXPECT_EQ(infos[0].arg0, "");
    EXPECT_EQ(infos[0].arg1, "GPU17");
    EXPECT_EQ(infos[0].resolution, "RetryPower");
    EXPECT_EQ(infos[1].messageId, "Update.1.0.ComponentUpdateTime");
    EXPECT_EQ(infos[1].arg0, "GPU18");
    EXPECT_EQ(infos[1].arg1, "1200");
    EXPECT_TRUE(infos[1].resolution.empty());
}

TEST_F(DBusUtilInternalTest,
       queryDeviceStatusAndBooleanHelpersReturnFalseOnCallFailure)
{
    auto& bus = pldm::utils::DBusUtilMockHandler::fakeBus();
    bus.throwOnCall = true;

    EXPECT_FALSE(queryDeviceStatus(8));
    EXPECT_FALSE(queryDeviceStatusAndLog(8));
}

TEST_F(DBusUtilInternalTest,
       queryDeviceStatusAndBooleanHelpersReturnFalseOnMethodCreationFailure)
{
    auto& bus = pldm::utils::DBusUtilMockHandler::fakeBus();
    bus.throwOnNewMethodCall = true;

    EXPECT_FALSE(queryDeviceStatus(8));
    EXPECT_FALSE(queryDeviceStatusAndLog(8));
}

TEST_F(DBusUtilInternalTest,
       queryDeviceStatusAndLogProcessesMultipleErrorsFromOneDevice)
{
    auto& bus = pldm::utils::DBusUtilMockHandler::fakeBus();
    bus.deviceStatusValue = makeStatusMap(
        DeviceState::DeviceHealth::Degraded,
        {{21,
          DeviceState::ErrorClass::Power,
          {{"REDFISH_MESSAGE_ID", "Update.1.0.TransferFailed"},
           {"REDFISH_MESSAGE_ARGS", "GPU0, 1.0 "},
           {"REDFISH_RESOLUTION", "Retry"}}},
         {22,
          DeviceState::ErrorClass::Recovery,
          {{"REDFISH_MESSAGE_ID", "Update.1.0.AwaitToActivate"},
           {"REDFISH_MESSAGE_ARGS", "GPU1, 2.0 "}}}});

    EXPECT_TRUE(queryDeviceStatusAndLog(8));
}

TEST_F(DBusUtilInternalTest,
       queryDeviceStatusAndLogForceCriticalOverridesInformationalSeverity)
{
    using Level =
        sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

    auto& bus = pldm::utils::DBusUtilMockHandler::fakeBus();
    bus.deviceStatusValue = makeStatusMap(
        DeviceState::DeviceHealth::Degraded,
        {{22,
          DeviceState::ErrorClass::Recovery,
          {{"REDFISH_MESSAGE_ID", "Update.1.0.AwaitToActivate"},
           {"REDFISH_MESSAGE_ARGS", "GPU1, 2.0 "},
           {"REDFISH_RESOLUTION", "Wait"}}}});

    auto& conn = pldm::utils::DBusUtilMockHandler::fakeConn();

    EXPECT_TRUE(queryDeviceStatusAndLog(8, true));
    EXPECT_EQ(conn->lastMessageId, "Update.1.0.AwaitToActivate");
    EXPECT_EQ(
        conn->lastSeverity,
        sdbusplus::xyz::openbmc_project::Logging::server::convertForMessage(
            Level::Critical));
    EXPECT_EQ(conn->lastAddData["REDFISH_MESSAGE_ARGS"], "2.0,GPU1");
}

TEST_F(DBusUtilInternalTest, createLogEntryReplayedIdHonorsSeverityOverride)
{
    using Level =
        sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;
    auto& conn = pldm::utils::DBusUtilMockHandler::fakeConn();

    for (const auto& messageId :
         {deviceDriverErrorsDetected, bmcDriverErrorsDetected})
    {
        createLogEntry(messageId, "SetEndpointID", "GPU0,unreachable",
                       "Power cycle.", "FWUpdate", true);
        EXPECT_EQ(
            conn->lastSeverity,
            sdbusplus::xyz::openbmc_project::Logging::server::convertForMessage(
                Level::Informational));
        EXPECT_EQ(conn->lastAddData["REDFISH_MESSAGE_ARGS"],
                  "SetEndpointID,GPU0,unreachable");

        createLogEntry(messageId, "SetEndpointID", "GPU0,unreachable",
                       "Power cycle.", "FWUpdate", false);
        EXPECT_EQ(
            conn->lastSeverity,
            sdbusplus::xyz::openbmc_project::Logging::server::convertForMessage(
                Level::Critical));
    }
}

TEST_F(DBusUtilInternalTest, createLogEntryUnknownIdHonorsSeverityOverride)
{
    using Level =
        sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;
    auto& conn = pldm::utils::DBusUtilMockHandler::fakeConn();

    // Catch-all: an ID from a registry pldm does not model at all.
    createLogEntry("SomeOther.1.0.MessageId", "GPU0", "detail", "Retry.",
                   "FWUpdate", true);
    EXPECT_EQ(
        conn->lastSeverity,
        sdbusplus::xyz::openbmc_project::Logging::server::convertForMessage(
            Level::Informational));

    createLogEntry("SomeOther.1.0.MessageId", "GPU0", "detail", "Retry.",
                   "FWUpdate", false);
    EXPECT_EQ(
        conn->lastSeverity,
        sdbusplus::xyz::openbmc_project::Logging::server::convertForMessage(
            Level::Critical));
}

TEST_F(DBusUtilInternalTest, createLogEntryRecoveryKeepsSeverityOverride)
{
    using Level =
        sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;
    auto& conn = pldm::utils::DBusUtilMockHandler::fakeConn();

    // Recovery entries are committed at Critical by their producer; a device
    // that is not an update target must still report Informational.
    createLogEntry(firmwareInRecovery, "", "GPU0", "Perform device FW recovery",
                   "FWUpdate", true);
    EXPECT_EQ(
        conn->lastSeverity,
        sdbusplus::xyz::openbmc_project::Logging::server::convertForMessage(
            Level::Informational));
}

TEST_F(DBusUtilInternalTest, createLogEntryCoversRemainingKnownMessageIds)
{
    createLogEntry(updateSuccessful, "GPU0", "1.2.3", "");
    createLogEntry(componentUpdateSkipped, "GPU0", "AlreadyCurrent", "");
    createLogEntry(verificationFailed, "GPU0", "DigestMismatch", "Retry");
}

TEST_F(DBusUtilInternalTest,
       queryDeviceStatusErrorHandlesLeadingCommaAndResolutionCoverage)
{
    auto& bus = pldm::utils::DBusUtilMockHandler::fakeBus();
    bus.deviceStatusValue = makeStatusMap(
        DeviceState::DeviceHealth::Degraded,
        {{13,
          DeviceState::ErrorClass::Power,
          {{"REDFISH_MESSAGE_ID", "Update.1.0.TransferFailed"},
           {"REDFISH_MESSAGE_ARGS", ",  delayed retry  "},
           {"REDFISH_RESOLUTION", "RetryLater"}}}});

    const auto infos = queryDeviceStatusError(8);
    ASSERT_EQ(infos.size(), 1u);
    EXPECT_TRUE(infos[0].arg0.empty());
    EXPECT_EQ(infos[0].arg1, "delayed retry");
    EXPECT_EQ(infos[0].resolution, "RetryLater");
}

TEST_F(DBusUtilInternalTest,
       queryDeviceStatusErrorHandlesResolutionWithoutMessageArgsCoverage)
{
    auto& bus = pldm::utils::DBusUtilMockHandler::fakeBus();
    bus.deviceStatusValue = makeStatusMap(
        DeviceState::DeviceHealth::Degraded,
        {{14,
          DeviceState::ErrorClass::Recovery,
          {{"REDFISH_MESSAGE_ID", "Update.1.0.AwaitToActivate"},
           {"REDFISH_RESOLUTION", "WaitForActivation"}}}});

    const auto infos = queryDeviceStatusError(8);
    ASSERT_EQ(infos.size(), 1u);
    EXPECT_EQ(infos[0].messageId, "Update.1.0.AwaitToActivate");
    EXPECT_TRUE(infos[0].arg0.empty());
    EXPECT_TRUE(infos[0].arg1.empty());
    EXPECT_EQ(infos[0].resolution, "WaitForActivation");
}

} // namespace
