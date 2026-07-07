/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "libpldm/firmware_update.h"

#include "common/instance_id.hpp"
#include "common/utils.hpp"
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "fw-update/device_updater.hpp"
#include "fw-update/package_parser.hpp"
#include "fw-update/update_manager.hpp"
#include "mocked_firmware_update_function.hpp"
#include "requester/handler.hpp"
#include "test/test_instance_id.hpp"

#include <libpldm/firmware_update.h>

#include <sdbusplus/bus.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>
#include <sdeventplus/test/sdevent.hpp>
#include <xyz/openbmc_project/Software/ApplyTime/server.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::_;
using namespace pldm;
using namespace pldm::fw_update;
using namespace std::chrono;

class DeviceUpdaterTest : public testing::Test
{
  protected:
    DeviceUpdaterTest() :
        package("./test_pkg", std::ios::binary | std::ios::in | std::ios::ate),
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                      componentInfoMap, componentNameMap, true, nullptr),
        deviceUpdater(0, package, fwDeviceIDRecord, compImageInfos, compInfo,
                      compIdNameInfo, 512, &updateManager)
    {
        fwDeviceIDRecord = {
            1,
            {0x00},
            "VersionString2",
            {{PLDM_FWUP_UUID,
              std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41,
                                   0x15, 0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49,
                                   0xD6, 0x75}}},
            {}};
        compImageInfos = {
            {10, 100, 0xFFFFFFFF, 0, 0, 139, 1024, "VersionString3"}};
        compInfo = {
            {std::make_pair(10, 100),
             std::make_tuple(1, "comp1Version", static_cast<uint16_t>(0))}};
        compIdNameInfo = {{11, "ComponentName1"},
                          {55555, "ComponentName2"},
                          {12, "ComponentName3"},
                          {66666, "ComponentName4"}};
    }

    void TearDown() override
    {
        drainPendingAsyncWork();
        finalizeAsyncHandle(deviceUpdater.deviceUpdaterHandle);
    }

    void runEvent(uint64_t timeoutUsec = 200000)
    {
        EXPECT_GE(sd_event_run(event.get(), timeoutUsec), 0);
    }

    bool requesterPending() const
    {
        if (!reqHandler.handlers.empty() ||
            !reqHandler.removeRequestContainer.empty())
        {
            return true;
        }

        for (const auto& [eid, queue] : reqHandler.endpointMessageQueues)
        {
            static_cast<void>(eid);
            if (queue->activeRequest || !queue->requestQueue.empty())
            {
                return true;
            }
        }

        return false;
    }

    void expireOutstandingRequests()
    {
        std::vector<requester::RequestKey> keys;
        keys.reserve(reqHandler.handlers.size());
        for (const auto& [key, value] : reqHandler.handlers)
        {
            static_cast<void>(value);
            if (!reqHandler.removeRequestContainer.contains(key))
            {
                keys.push_back(key);
            }
        }

        for (const auto& key : keys)
        {
            if (reqHandler.handlers.contains(key) &&
                !reqHandler.removeRequestContainer.contains(key))
            {
                reqHandler.instanceIdExpiryCallBack(key);
            }
        }
    }

    void flushReadyEvents(int iterations = 8)
    {
        for (int i = 0; i < iterations; ++i)
        {
            runEvent(0);
        }
    }

    void drainPendingAsyncWork()
    {
        auto settleAsyncHandle = [](auto& handle) {
            if (!handle.has_value())
            {
                return false;
            }

            auto& [scope, rcOpt] = *handle;
            if (!rcOpt.has_value())
            {
                return true;
            }

            stdexec::sync_wait(scope.on_empty());
            handle.reset();
            return false;
        };

        for (int i = 0; i < 64; ++i)
        {
            flushReadyEvents();

            if (requesterPending())
            {
                expireOutstandingRequests();
            }

            const bool handlePending =
                settleAsyncHandle(deviceUpdater.deviceUpdaterHandle);

            flushReadyEvents();

            if (!requesterPending() && !handlePending &&
                !deviceUpdater.deviceUpdaterHandle.has_value())
            {
                flushReadyEvents();
                if (!requesterPending() &&
                    !deviceUpdater.deviceUpdaterHandle.has_value())
                {
                    return;
                }
            }
        }
    }

    template <typename Handle>
    void finalizeAsyncHandle(Handle& handle)
    {
        if (!handle.has_value())
        {
            return;
        }

        auto& [scope, rcOpt] = *handle;
        if (rcOpt.has_value())
        {
            stdexec::sync_wait(scope.on_empty());
        }

        handle.reset();
    }

    std::ifstream package;
    mctp_eid_t eid{0};
    FirmwareDeviceIDRecord fwDeviceIDRecord;
    ComponentImageInfos compImageInfos;
    ComponentInfo compInfo;
    ComponentIdNameMap compIdNameInfo;
    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    requester::Handler<requester::Request> reqHandler;
    DescriptorMap descriptorMap;
    ComponentInfoMap componentInfoMap;
    ComponentNameMap componentNameMap;
    UpdateManager updateManager;
    DeviceUpdater deviceUpdater;
};

TEST_F(DeviceUpdaterTest, validatePackage)
{
    constexpr uintmax_t testPkgSize = 1163;
    uintmax_t packageSize = package.tellg();
    EXPECT_EQ(packageSize, testPkgSize);

    package.seekg(0);
    std::vector<uint8_t> packageHeader(testPkgSize);
    package.read(new (packageHeader.data()) char, testPkgSize);

    auto parser = parsePkgHeader(packageHeader.data(), packageHeader.size());
    EXPECT_NE(parser, nullptr);

    package.seekg(0);

    parser->parse(packageHeader.data(), packageSize);
    const auto& fwDeviceIDRecords = parser->getFwDeviceIDRecords();
    const auto& testPkgCompImageInfos = parser->getComponentImageInfos();

    EXPECT_EQ(fwDeviceIDRecords.size(), 1);
    EXPECT_EQ(compImageInfos.size(), 1);
    EXPECT_EQ(fwDeviceIDRecords[0], fwDeviceIDRecord);
    EXPECT_EQ(testPkgCompImageInfos, compImageInfos);
}

TEST_F(DeviceUpdaterTest, requestUpdate)
{
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_request_firmware_data_req)>
        reqFwDataReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x02, 0x00, 0x00};

    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());
    uint8_t retryCount = 0;

    EXPECT_NO_THROW({
        auto co = deviceUpdater.processRequestUpdateResponse(
            eid, requestMsg, sizeof(struct pldm_request_update_resp),
            retryCount);
        stdexec::sync_wait(std::move(co));
    });
}

TEST_F(DeviceUpdaterTest,
       private_method_sendPassCompTableRequest_PLDM_START_AND_END)
{
    size_t offset = 0;

    EXPECT_NO_THROW({
        auto co = deviceUpdater.sendPassCompTableRequest(offset);
        auto rc = stdexec::sync_wait(std::move(co));
        ASSERT_TRUE(rc.has_value());
    });
}

TEST_F(DeviceUpdaterTest, private_method_sendPassCompTableRequest_PLDM_START)
{
    size_t offset = 0;

    FirmwareDeviceIDRecord fwDeviceIDRecord2;
    fwDeviceIDRecord2 = {
        1,
        {0x00, 0x01, 0x02},
        "VersionString2",
        {{PLDM_FWUP_UUID,
          std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                               0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                               0x75}}},
        {}};

    EXPECT_NO_THROW({
        auto co = deviceUpdater.sendPassCompTableRequest(offset);
        auto rc = stdexec::sync_wait(std::move(co));
        ASSERT_TRUE(rc.has_value());
    });
}

TEST_F(DeviceUpdaterTest, passCompTable)
{
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_request_firmware_data_req)>
        reqFwDataReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x02, 0x00, 0x00};

    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());
    uint8_t retryCount = 0;

    EXPECT_NO_THROW({
        auto co = deviceUpdater.processPassCompTableResponse(
            eid, requestMsg, sizeof(struct pldm_pass_component_table_resp),
            retryCount);
        auto rc = stdexec::sync_wait(std::move(co));
        ASSERT_TRUE(rc.has_value());
    });
}

TEST_F(DeviceUpdaterTest, sendActivateFirmwareRequest)
{
    EXPECT_NO_THROW({
        [[maybe_unused]] auto co = deviceUpdater.sendActivateFirmwareRequest();
    });
}

TEST_F(DeviceUpdaterTest, activateFirmware)
{
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_activate_firmware_resp)>
        activateFirmwareReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00};

    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(activateFirmwareReq.data());
    uint8_t retryCount = 0;

    EXPECT_NO_THROW({
        auto co = deviceUpdater.processActivateFirmwareResponse(
            eid, requestMsg, sizeof(struct pldm_activate_firmware_resp),
            retryCount);
        auto rc = stdexec::sync_wait(std::move(co));
        ASSERT_TRUE(rc.has_value());
    });
}

TEST_F(DeviceUpdaterTest, sendCommandNotExpectedResponse)
{
    const pldm_msg pldmmsg{};

    EXPECT_NO_THROW({ sendCommandNotExpectedResponse(&pldmmsg, 0); });
}

TEST_F(DeviceUpdaterTest, requestFwDataRejectedAfterTimeoutCancellation)
{
    const pldm_msg pldmmsg{};
    deviceUpdater.timeoutCancellationRequested = true;

    auto response = deviceUpdater.requestFwData(&pldmmsg, 0);

    ASSERT_GE(response.size(), sizeof(pldm_msg));
    auto responseMsg = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responseMsg->payload[0], PLDM_FWUP_COMMAND_NOT_EXPECTED);
}

TEST_F(DeviceUpdaterTest,
       handleUpdateTimeoutStopsComponentTimersAndDeferredWork)
{
    constexpr size_t componentOffset = 0;
    auto compUpdater = std::make_unique<ComponentUpdater>(
        eid, package, fwDeviceIDRecord, compImageInfos, compInfo,
        compIdNameInfo, 512, &updateManager, &deviceUpdater, componentOffset);
    compUpdater->createRequestFwDataTimer();
    compUpdater->createCompleteCommandsTimeoutTimer();
    compUpdater->pendingPostResponseAction = [] {};
    compUpdater->pldmRequest = std::make_unique<sdeventplus::source::Defer>(
        updateManager.event, [](EventBase&) {});

    auto* compUpdaterPtr = compUpdater.get();
    deviceUpdater.componentUpdaterMap.emplace(
        componentOffset, std::make_pair(std::move(compUpdater), false));

    std::get<ApplicableComponents>(fwDeviceIDRecord).clear();
    deviceUpdater.handleUpdateTimeout();

    EXPECT_TRUE(deviceUpdater.timeoutCancellationRequested);
    EXPECT_EQ(compUpdaterPtr->reqFwDataTimer, nullptr);
    EXPECT_EQ(compUpdaterPtr->completeCommandsTimeoutTimer, nullptr);
    EXPECT_EQ(compUpdaterPtr->pldmRequest, nullptr);
    EXPECT_FALSE(static_cast<bool>(compUpdaterPtr->pendingPostResponseAction));
}

TEST(DeviceUpdaterSequence, command_RequestUpdate)
{
    struct DeviceUpdaterState deviceUpdaterState;

    DeviceUpdaterSequence sequence = deviceUpdaterState.nextState(
        DeviceUpdaterSequence::RequestUpdate, 0, 0);

    EXPECT_EQ(sequence, DeviceUpdaterSequence::PassComponentTable);
}

TEST(DeviceUpdaterSequence, command_PassComponentTable)
{
    struct DeviceUpdaterState deviceUpdaterState;

    DeviceUpdaterSequence sequence = deviceUpdaterState.nextState(
        DeviceUpdaterSequence::PassComponentTable, 0, 0);

    EXPECT_EQ(sequence, DeviceUpdaterSequence::ActivateFirmware);
}

TEST(DeviceUpdaterSequence,
     command_PassComponentTable_compIndex_less_then_numComps)
{
    struct DeviceUpdaterState deviceUpdaterState;

    DeviceUpdaterSequence sequence = deviceUpdaterState.nextState(
        DeviceUpdaterSequence::PassComponentTable, 0, 1);

    EXPECT_EQ(sequence, DeviceUpdaterSequence::PassComponentTable);
}

TEST(DeviceUpdaterSequence, command_Invalid)
{
    struct DeviceUpdaterState deviceUpdaterState;

    DeviceUpdaterSequence sequence =
        deviceUpdaterState.nextState(DeviceUpdaterSequence::Invalid, 0, 0);

    EXPECT_EQ(sequence, DeviceUpdaterSequence::Invalid);
}

TEST(DeviceUpdaterSequence, command_Invalid_state)
{
    struct DeviceUpdaterState deviceUpdaterState;

    DeviceUpdaterSequence sequence =
        deviceUpdaterState.nextState(DeviceUpdaterSequence::Invalid, 0, 0);

    EXPECT_EQ(sequence, DeviceUpdaterSequence::Invalid);
}

TEST(DeviceUpdaterSequence, command_ActivateFirmware)
{
    struct DeviceUpdaterState deviceUpdaterState;

    DeviceUpdaterSequence sequence = deviceUpdaterState.nextState(
        DeviceUpdaterSequence::ActivateFirmware, 0, 0);

    EXPECT_EQ(sequence, DeviceUpdaterSequence::Invalid);
}

TEST(DeviceUpdaterSequence, command_RetryRequest)
{
    struct DeviceUpdaterState deviceUpdaterState;

    DeviceUpdaterSequence sequence =
        deviceUpdaterState.nextState(DeviceUpdaterSequence::Valid, 0, 0);

    EXPECT_EQ(sequence, DeviceUpdaterSequence::RetryRequest);
}

TEST_F(DeviceUpdaterTest, sendcancelUpdateRequest)
{
    EXPECT_NO_THROW({
        [[maybe_unused]] auto co = deviceUpdater.sendCancelUpdateRequest();
    });
}

TEST_F(DeviceUpdaterTest, cancelUpdate_empty_response)
{
    EXPECT_NO_THROW({
        [[maybe_unused]] auto co =
            deviceUpdater.processCancelUpdateResponse(eid, nullptr, 0);
    });
}

TEST_F(DeviceUpdaterTest, cancelUpdate)
{
    const pldm_msg pldmmsg{};

    EXPECT_NO_THROW({
        [[maybe_unused]] auto co =
            deviceUpdater.processCancelUpdateResponse(eid, &pldmmsg, 0);
    });
}

TEST_F(DeviceUpdaterTest, sendRequestUpdate)
{
    EXPECT_NO_THROW({
        [[maybe_unused]] auto co = deviceUpdater.sendRequestUpdate();
    });
}

TEST_F(DeviceUpdaterTest, updateComponentCompletion)
{
    size_t componentOffset = 0;
    std::unique_ptr<ComponentUpdater> compUpdater =
        std::make_unique<ComponentUpdater>(
            eid, package, fwDeviceIDRecord, compImageInfos, compInfo,
            compIdNameInfo, 512, &updateManager, &deviceUpdater,
            componentOffset);
    deviceUpdater.componentUpdaterMap.emplace(
        componentOffset, std::make_pair(std::move(compUpdater), false));
    EXPECT_NO_THROW({
        [[maybe_unused]] auto co = deviceUpdater.updateComponentCompletion(
            0, ComponentUpdateStatus::UpdateFailed);
    });
}

TEST_F(DeviceUpdaterTest, requestFwDataWithoutComponentUpdaterMap)
{
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_request_firmware_data_req)>
        reqFwDataReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x02, 0x00, 0x00};
    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    auto response = deviceUpdater.requestFwData(
        requestMsg, sizeof(pldm_request_firmware_data_req));
    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_FWUP_COMMAND_NOT_EXPECTED);
}

TEST_F(DeviceUpdaterTest, transferCompleteWithoutComponentUpdaterMap)
{
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + sizeof(uint8_t)>
        transferCompleteReq{0x8A, 0x05, 0x16, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(transferCompleteReq.data());

    auto response = deviceUpdater.transferComplete(requestMsg, sizeof(uint8_t));
    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_FWUP_COMMAND_NOT_EXPECTED);
}

TEST_F(DeviceUpdaterTest, verifyCompleteWithoutComponentUpdaterMap)
{
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + sizeof(uint8_t)>
        verifyCompleteReq{0x8A, 0x05, 0x17, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(verifyCompleteReq.data());

    auto response = deviceUpdater.verifyComplete(requestMsg, sizeof(uint8_t));
    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_FWUP_COMMAND_NOT_EXPECTED);
}

TEST_F(DeviceUpdaterTest, applyCompleteWithoutComponentUpdaterMap)
{
    constexpr std::array<uint8_t,
                         sizeof(pldm_msg_hdr) + sizeof(pldm_apply_complete_req)>
        applyCompleteReq{0x00, 0x00, 0x18, 0x00, 0x00, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(applyCompleteReq.data());

    auto response = deviceUpdater.applyComplete(
        requestMsg, sizeof(pldm_apply_complete_req));
    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_FWUP_COMMAND_NOT_EXPECTED);
}

TEST_F(DeviceUpdaterTest, requestFwDataWithComponentUpdaterMapRoutesToComponent)
{
    size_t componentOffset = 0;
    std::unique_ptr<ComponentUpdater> compUpdater =
        std::make_unique<ComponentUpdater>(
            eid, package, fwDeviceIDRecord, compImageInfos, compInfo,
            compIdNameInfo, 512, &updateManager, &deviceUpdater,
            componentOffset);
    compUpdater->createRequestFwDataTimer();
    compUpdater->componentUpdaterState.set(
        ComponentUpdaterSequence::RequestFirmwareData);
    deviceUpdater.componentUpdaterMap.emplace(
        componentOffset, std::make_pair(std::move(compUpdater), false));

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_request_firmware_data_req)>
        reqFwDataReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x02, 0x00, 0x00};
    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    auto response = deviceUpdater.requestFwData(
        requestMsg, sizeof(pldm_request_firmware_data_req));
    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);
}

TEST_F(DeviceUpdaterTest,
       transferCompleteWithComponentUpdaterMapRoutesToComponent)
{
    size_t componentOffset = 0;
    std::unique_ptr<ComponentUpdater> compUpdater =
        std::make_unique<ComponentUpdater>(
            eid, package, fwDeviceIDRecord, compImageInfos, compInfo,
            compIdNameInfo, 512, &updateManager, &deviceUpdater,
            componentOffset);
    compUpdater->componentUpdaterState.prev =
        ComponentUpdaterSequence::TransferComplete;
    compUpdater->componentUpdaterState.current =
        ComponentUpdaterSequence::Invalid;
    deviceUpdater.componentUpdaterMap.emplace(
        componentOffset, std::make_pair(std::move(compUpdater), false));

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + sizeof(uint8_t)>
        transferCompleteReq{0x8A, 0x05, 0x16, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(transferCompleteReq.data());

    auto response = deviceUpdater.transferComplete(requestMsg, sizeof(uint8_t));
    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);
}

TEST_F(DeviceUpdaterTest,
       verifyCompleteWithComponentUpdaterMapRoutesToComponent)
{
    size_t componentOffset = 0;
    std::unique_ptr<ComponentUpdater> compUpdater =
        std::make_unique<ComponentUpdater>(
            eid, package, fwDeviceIDRecord, compImageInfos, compInfo,
            compIdNameInfo, 512, &updateManager, &deviceUpdater,
            componentOffset);
    compUpdater->componentUpdaterState.prev =
        ComponentUpdaterSequence::VerifyComplete;
    compUpdater->componentUpdaterState.current =
        ComponentUpdaterSequence::Invalid;
    deviceUpdater.componentUpdaterMap.emplace(
        componentOffset, std::make_pair(std::move(compUpdater), false));

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + sizeof(uint8_t)>
        verifyCompleteReq{0x8A, 0x05, 0x17, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(verifyCompleteReq.data());

    auto response = deviceUpdater.verifyComplete(requestMsg, sizeof(uint8_t));
    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);
}

TEST_F(DeviceUpdaterTest, applyCompleteWithComponentUpdaterMapRoutesToComponent)
{
    size_t componentOffset = 0;
    std::unique_ptr<ComponentUpdater> compUpdater =
        std::make_unique<ComponentUpdater>(
            eid, package, fwDeviceIDRecord, compImageInfos, compInfo,
            compIdNameInfo, 512, &updateManager, &deviceUpdater,
            componentOffset);
    compUpdater->componentUpdaterState.prev =
        ComponentUpdaterSequence::ApplyComplete;
    compUpdater->componentUpdaterState.current =
        ComponentUpdaterSequence::Invalid;
    deviceUpdater.componentUpdaterMap.emplace(
        componentOffset, std::make_pair(std::move(compUpdater), false));

    constexpr std::array<uint8_t,
                         sizeof(pldm_msg_hdr) + sizeof(pldm_apply_complete_req)>
        applyCompleteReq{0x00, 0x00, 0x18, 0x00, 0x00, 0x00};
    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(applyCompleteReq.data());

    auto response = deviceUpdater.applyComplete(
        requestMsg, sizeof(pldm_apply_complete_req));
    ASSERT_GE(response.size(), sizeof(pldm_msg_hdr) + 1);
    EXPECT_EQ(response[sizeof(pldm_msg_hdr)], PLDM_SUCCESS);
}

TEST_F(DeviceUpdaterTest, deviceUpdaterHandlerReturnsWhenUpdateAlreadyPending)
{
    deviceUpdater.deviceUpdaterHandle.emplace();
    EXPECT_NO_THROW({ deviceUpdater.deviceUpdaterHandler(); });
}

TEST_F(DeviceUpdaterTest, deviceUpdaterHandlerStartsWhenNoExistingHandle)
{
    EXPECT_FALSE(deviceUpdater.deviceUpdaterHandle.has_value());

    EXPECT_NO_THROW({ deviceUpdater.deviceUpdaterHandler(); });
    EXPECT_TRUE(deviceUpdater.deviceUpdaterHandle.has_value());
}

TEST_F(DeviceUpdaterTest, deviceUpdaterHandlerResetsCompletedHandle)
{
    auto& [scope, rcOpt] = deviceUpdater.deviceUpdaterHandle.emplace();
    (void)scope;
    rcOpt.emplace(PLDM_SUCCESS);

    EXPECT_NO_THROW({ deviceUpdater.deviceUpdaterHandler(); });
    EXPECT_TRUE(deviceUpdater.deviceUpdaterHandle.has_value());
}

TEST_F(DeviceUpdaterTest, onResponseSendCompleteWithoutTrackedComponentIsNoop)
{
    EXPECT_NO_THROW({ deviceUpdater.onResponseSendComplete(true); });
}

TEST_F(DeviceUpdaterTest, onResponseSendCompleteWithTrackedComponentIsNoop)
{
    size_t componentOffset = 0;
    std::unique_ptr<ComponentUpdater> compUpdater =
        std::make_unique<ComponentUpdater>(
            eid, package, fwDeviceIDRecord, compImageInfos, compInfo,
            compIdNameInfo, 512, &updateManager, &deviceUpdater,
            componentOffset);
    deviceUpdater.componentUpdaterMap.emplace(
        componentOffset, std::make_pair(std::move(compUpdater), false));

    EXPECT_NO_THROW({ deviceUpdater.onResponseSendComplete(true); });
}

TEST_F(DeviceUpdaterTest, isComponentFailedReturnsFalseWhenMissing)
{
    EXPECT_FALSE(deviceUpdater.isComponentFailed(0));
}

TEST_F(DeviceUpdaterTest, isComponentFailedReturnsFalseWhenComponentSucceeded)
{
    size_t componentOffset = 0;
    std::unique_ptr<ComponentUpdater> compUpdater =
        std::make_unique<ComponentUpdater>(
            eid, package, fwDeviceIDRecord, compImageInfos, compInfo,
            compIdNameInfo, 512, &updateManager, &deviceUpdater,
            componentOffset);
    deviceUpdater.componentUpdaterMap.emplace(
        componentOffset, std::make_pair(std::move(compUpdater), true));

    EXPECT_FALSE(deviceUpdater.isComponentFailed(componentOffset));
}

TEST_F(DeviceUpdaterTest, isComponentFailedReturnsTrueWhenComponentFailed)
{
    size_t componentOffset = 0;
    std::unique_ptr<ComponentUpdater> compUpdater =
        std::make_unique<ComponentUpdater>(
            eid, package, fwDeviceIDRecord, compImageInfos, compInfo,
            compIdNameInfo, 512, &updateManager, &deviceUpdater,
            componentOffset);
    deviceUpdater.componentUpdaterMap.emplace(
        componentOffset, std::make_pair(std::move(compUpdater), false));

    EXPECT_TRUE(deviceUpdater.isComponentFailed(componentOffset));
}

TEST_F(DeviceUpdaterTest,
       isLiveActivationSupportedReturnsFalseWhenCompInfoEmpty)
{
    ComponentInfo emptyCompInfo{};
    DeviceUpdater localUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                               emptyCompInfo, compIdNameInfo, 512,
                               &updateManager);
    EXPECT_FALSE(localUpdater.isLiveActivationSupported());
}

TEST_F(DeviceUpdaterTest,
       isLiveActivationSupportedReturnsFalseWhenComponentMissingInCompInfo)
{
    ComponentInfo mismatchedCompInfo{};
    mismatchedCompInfo[{20, 200}] = std::make_tuple(
        static_cast<uint8_t>(1), std::string("v"),
        static_cast<uint16_t>(1 << PLDM_ACTIVATION_SELF_CONTAINED));
    DeviceUpdater localUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                               mismatchedCompInfo, compIdNameInfo, 512,
                               &updateManager);
    localUpdater.componentActivationModifications.value =
        (1 << PLDM_ACTIVATION_SELF_CONTAINED);

    EXPECT_FALSE(localUpdater.isLiveActivationSupported());
}

TEST_F(DeviceUpdaterTest,
       isLiveActivationSupportedReturnsFalseWhenSelfContainedNotSupported)
{
    ComponentInfo nonSelfContainedCompInfo{};
    nonSelfContainedCompInfo[{10, 100}] = std::make_tuple(
        static_cast<uint8_t>(1), std::string("v"), static_cast<uint16_t>(0));
    DeviceUpdater localUpdater(eid, package, fwDeviceIDRecord, compImageInfos,
                               nonSelfContainedCompInfo, compIdNameInfo, 512,
                               &updateManager);
    localUpdater.componentActivationModifications.value =
        (1 << PLDM_ACTIVATION_SELF_CONTAINED);

    EXPECT_FALSE(localUpdater.isLiveActivationSupported());
}

TEST_F(DeviceUpdaterTest, isLiveActivationSupportedReturnsTrueForImmediate)
{
    using ApplyTimes = sdbusplus::xyz::openbmc_project::Software::server::
        ApplyTime::RequestedApplyTimes;

    updateManager.setRequestedApplyTime(ApplyTimes::Immediate);
    ComponentInfo selfContainedCompInfo{};
    selfContainedCompInfo[{10, 100}] = std::make_tuple(
        static_cast<uint8_t>(1), std::string("v"),
        static_cast<uint16_t>(1 << PLDM_ACTIVATION_SELF_CONTAINED));
    ComponentImageInfos localCompImageInfos = compImageInfos;
    DeviceUpdater localUpdater(eid, package, fwDeviceIDRecord,
                               localCompImageInfos, selfContainedCompInfo,
                               compIdNameInfo, 512, &updateManager);
    localUpdater.componentActivationModifications.value =
        (1 << PLDM_ACTIVATION_SELF_CONTAINED);

    EXPECT_TRUE(localUpdater.isLiveActivationSupported());
}

TEST_F(DeviceUpdaterTest,
       isLiveActivationSupportedReturnsTrueWhenPackageRequestsSelfContained)
{
    using ApplyTimes = sdbusplus::xyz::openbmc_project::Software::server::
        ApplyTime::RequestedApplyTimes;

    updateManager.setRequestedApplyTime(ApplyTimes::OnReset);
    ComponentInfo selfContainedCompInfo{};
    selfContainedCompInfo[{10, 100}] = std::make_tuple(
        static_cast<uint8_t>(1), std::string("v"),
        static_cast<uint16_t>(1 << PLDM_ACTIVATION_SELF_CONTAINED));
    ComponentImageInfos localCompImageInfos = compImageInfos;
    std::get<static_cast<size_t>(
        ComponentImageInfoPos::ReqCompActivationMethodPos)>(
        localCompImageInfos[0])
        .set(PLDM_ACTIVATION_SELF_CONTAINED);

    DeviceUpdater localUpdater(eid, package, fwDeviceIDRecord,
                               localCompImageInfos, selfContainedCompInfo,
                               compIdNameInfo, 512, &updateManager);
    localUpdater.componentActivationModifications.value =
        (1 << PLDM_ACTIVATION_SELF_CONTAINED);

    EXPECT_TRUE(localUpdater.isLiveActivationSupported());
}

TEST_F(DeviceUpdaterTest,
       isLiveActivationSupportedReturnsFalseWhenActivationModBitNotSet)
{
    using ApplyTimes = sdbusplus::xyz::openbmc_project::Software::server::
        ApplyTime::RequestedApplyTimes;

    updateManager.setRequestedApplyTime(ApplyTimes::Immediate);
    ComponentInfo selfContainedCompInfo{};
    selfContainedCompInfo[{10, 100}] = std::make_tuple(
        static_cast<uint8_t>(1), std::string("v"),
        static_cast<uint16_t>(1 << PLDM_ACTIVATION_SELF_CONTAINED));
    ComponentImageInfos localCompImageInfos = compImageInfos;
    std::get<static_cast<size_t>(
        ComponentImageInfoPos::ReqCompActivationMethodPos)>(
        localCompImageInfos[0])
        .set(PLDM_ACTIVATION_SELF_CONTAINED);

    DeviceUpdater localUpdater(eid, package, fwDeviceIDRecord,
                               localCompImageInfos, selfContainedCompInfo,
                               compIdNameInfo, 512, &updateManager);
    localUpdater.componentActivationModifications.value = 0;

    EXPECT_FALSE(localUpdater.isLiveActivationSupported());
}

TEST_F(DeviceUpdaterTest,
       isLiveActivationSupportedReturnsFalseWhenNoImmediateAndNoPackageRequest)
{
    using ApplyTimes = sdbusplus::xyz::openbmc_project::Software::server::
        ApplyTime::RequestedApplyTimes;

    updateManager.setRequestedApplyTime(ApplyTimes::OnReset);
    ComponentInfo selfContainedCompInfo{};
    selfContainedCompInfo[{10, 100}] = std::make_tuple(
        static_cast<uint8_t>(1), std::string("v"),
        static_cast<uint16_t>(1 << PLDM_ACTIVATION_SELF_CONTAINED));
    ComponentImageInfos localCompImageInfos = compImageInfos;
    std::get<static_cast<size_t>(
        ComponentImageInfoPos::ReqCompActivationMethodPos)>(
        localCompImageInfos[0])
        .reset();

    DeviceUpdater localUpdater(eid, package, fwDeviceIDRecord,
                               localCompImageInfos, selfContainedCompInfo,
                               compIdNameInfo, 512, &updateManager);
    localUpdater.componentActivationModifications.value =
        (1 << PLDM_ACTIVATION_SELF_CONTAINED);

    EXPECT_FALSE(localUpdater.isLiveActivationSupported());
}

TEST_F(DeviceUpdaterTest, staleComponentCompletionIsIgnored)
{
    // A stale timer can replay component 0's completion after the device
    // updater has already advanced to component 1; it must not advance
    // componentIndex a second time.
    deviceUpdater.componentIndex = 1;

    auto co = deviceUpdater.updateComponentCompletion(
        0, ComponentUpdateStatus::UpdateFailed);
    auto rc = stdexec::sync_wait(std::move(co));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(std::get<0>(*rc), PLDM_SUCCESS);
    EXPECT_EQ(deviceUpdater.componentIndex, 1u);
    EXPECT_TRUE(deviceUpdater.componentUpdaterMap.empty());
}

TEST_F(DeviceUpdaterTest, duplicateComponentCompletionIsIgnored)
{
    deviceUpdater.completedComponentIndices.insert(0);

    auto co = deviceUpdater.updateComponentCompletion(
        0, ComponentUpdateStatus::UpdateFailed);
    auto rc = stdexec::sync_wait(std::move(co));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(std::get<0>(*rc), PLDM_SUCCESS);
    EXPECT_EQ(deviceUpdater.componentIndex, 0u);
    EXPECT_TRUE(deviceUpdater.componentUpdaterMap.empty());
}
