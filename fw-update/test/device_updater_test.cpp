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
        [[maybe_unused]] auto co =
            deviceUpdater.sendPassCompTableRequest(offset);
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
        [[maybe_unused]] auto co =
            deviceUpdater.sendPassCompTableRequest(offset);
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
        [[maybe_unused]] auto co = deviceUpdater.processPassCompTableResponse(
            eid, requestMsg, sizeof(struct pldm_pass_component_table_resp),
            retryCount);
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
        [[maybe_unused]] auto co =
            deviceUpdater.processActivateFirmwareResponse(
                eid, requestMsg, sizeof(struct pldm_activate_firmware_resp),
                retryCount);
    });
}

TEST_F(DeviceUpdaterTest, sendCommandNotExpectedResponse)
{
    const pldm_msg pldmmsg{};

    EXPECT_NO_THROW({ sendCommandNotExpectedResponse(&pldmmsg, 0); });
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
