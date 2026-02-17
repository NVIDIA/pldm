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
    StatusValue deviceStatusValue{};
    std::map<std::string, std::vector<std::string>> mapperResponse{
        {"xyz.openbmc_project.Logging", {"com.nvidia.State.DeviceState"}}};

    FakeMethodCall new_method_call(const char*, const char*, const char*,
                                   const char*)
    {
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

    template <typename Callback, typename... Args>
    void async_method_call(Callback&& cb, Args&&...)
    {
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
        bus.deviceStatusValue = pldm::fw_update::DeviceStatusMap{};
        bus.mapperResponse = {
            {"xyz.openbmc_project.Logging", {"com.nvidia.State.DeviceState"}}};

        auto& conn = pldm::utils::DBusUtilMockHandler::fakeConn();
        conn->nextError.clear();
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

} // namespace
