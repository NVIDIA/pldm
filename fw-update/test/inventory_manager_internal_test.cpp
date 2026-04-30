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

#include "common/instance_id.hpp"
#include "common/types.hpp"
#include "fw-update/fw_update_utility.hpp"
#include "requester/handler.hpp"
#include "requester/mctp_endpoint_discovery.hpp"

#include <libpldm/pldm.h>

#include <sdeventplus/event.hpp>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "fw-update/inventory_manager.hpp"
#undef private

#include "requester/test/mock_request.hpp"
#include "test/test_instance_id.hpp"

#include <libpldm/firmware_update.h>

#include <array>
#include <chrono>

#include <gtest/gtest.h>

using namespace pldm;
using namespace pldm::fw_update;
using namespace std::chrono;

namespace
{

constexpr size_t getFwParamsPayloadLen = 119;

const std::array<uint8_t, sizeof(pldm_msg_hdr) + getFwParamsPayloadLen>
    getFirmwareParametersResp{
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x0c,
        0x00, 0x00, 0x44, 0x65, 0x76, 0x69, 0x63, 0x65, 0x56, 0x65, 0x72, 0x31,
        0x2e, 0x30, 0x0a, 0x00, 0x2c, 0x01, 0x14, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x43, 0x6f, 0x6d, 0x70, 0x31, 0x76, 0x32,
        0x2e, 0x30, 0x10, 0x00, 0x2d, 0x01, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x43, 0x6f, 0x6d, 0x70, 0x32, 0x76, 0x33,
        0x2e, 0x30};

} // namespace

class InventoryManagerInternalTest : public testing::Test
{
  protected:
    InventoryManagerInternalTest() :
        event(sdeventplus::Event::get_default()), instanceIdDb(),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(0), 0,
                   milliseconds(1))
    {}

    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    requester::Handler<requester::Request> reqHandler;
    DescriptorMap descriptorMap{};
    DownstreamDescriptorMap downstreamDescriptorMap{};
    ComponentInfoMap componentInfoMap{};
    DeviceInventoryInfo deviceInventoryInfo{};
};

TEST_F(InventoryManagerInternalTest,
       parseGetFWParametersResponseCreatesInventoryForFirstEndpoint)
{
    bool createCallbackCalled = false;
    bool updateCallbackCalled = false;
    const pldm::eid eid = 8;
    const UUID uuid = "00112233445566778899AABBCCDDEEFF";
    const MctpMedium medium =
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe";
    const MctpBinding binding =
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe";

    InventoryManager manager(
        reqHandler, instanceIdDb,
        [&](pldm::eid callbackEid, UUID callbackUuid, dbus::MctpInterfaces&) {
            createCallbackCalled = true;
            EXPECT_EQ(callbackEid, eid);
            EXPECT_EQ(callbackUuid, uuid);
        },
        [&](pldm::eid, UUID, dbus::MctpInterfaces&) {
            updateCallbackCalled = true;
        },
        descriptorMap, downstreamDescriptorMap, componentInfoMap,
        deviceInventoryInfo);

    manager.mctpEidMap[eid] = std::make_tuple(uuid, medium, binding);
    dbus::MctpInterfaces mctpInterfaces{{uuid, {}}};
    std::string messageError;
    std::string resolution;
    auto responseMsg =
        reinterpret_cast<const pldm_msg*>(getFirmwareParametersResp.data());

    auto co = manager.parseGetFWParametersResponse(
        eid, responseMsg, getFwParamsPayloadLen, messageError, resolution,
        mctpInterfaces, false);
    stdexec::sync_wait(std::move(co));

    EXPECT_TRUE(createCallbackCalled);
    EXPECT_FALSE(updateCallbackCalled);
    ASSERT_TRUE(manager.mctpInfoMap.contains(uuid));
    EXPECT_EQ(manager.mctpInfoMap.at(uuid).top().eid, eid);
}

TEST_F(InventoryManagerInternalTest,
       parseGetFWParametersResponseUpdatesInventoryForRediscoveredFastestEid)
{
    bool createCallbackCalled = false;
    bool updateCallbackCalled = false;
    const pldm::eid eid = 8;
    const UUID uuid = "00112233445566778899AABBCCDDEEFF";
    const MctpMedium medium =
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe";
    const MctpBinding binding =
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe";

    InventoryManager manager(
        reqHandler, instanceIdDb,
        [&](pldm::eid, UUID, dbus::MctpInterfaces&) {
            createCallbackCalled = true;
        },
        [&](pldm::eid callbackEid, UUID callbackUuid, dbus::MctpInterfaces&) {
            updateCallbackCalled = true;
            EXPECT_EQ(callbackEid, eid);
            EXPECT_EQ(callbackUuid, uuid);
        },
        descriptorMap, downstreamDescriptorMap, componentInfoMap,
        deviceInventoryInfo);

    manager.mctpEidMap[eid] = std::make_tuple(uuid, medium, binding);
    MCTPEidInfoPriorityQueue queue;
    queue.push({eid, medium, binding});
    manager.mctpInfoMap.emplace(uuid, std::move(queue));
    dbus::MctpInterfaces mctpInterfaces{{uuid, {}}};
    std::string messageError;
    std::string resolution;
    auto responseMsg =
        reinterpret_cast<const pldm_msg*>(getFirmwareParametersResp.data());

    auto co = manager.parseGetFWParametersResponse(
        eid, responseMsg, getFwParamsPayloadLen, messageError, resolution,
        mctpInterfaces, false);
    stdexec::sync_wait(std::move(co));

    EXPECT_FALSE(createCallbackCalled);
    EXPECT_TRUE(updateCallbackCalled);
}

TEST_F(InventoryManagerInternalTest,
       parseGetFWParametersResponseDropsSlowerEndpoint)
{
    const pldm::eid fastEid = 2;
    const pldm::eid slowEid = 7;
    const UUID uuid = "00112233445566778899AABBCCDDEEFF";
    const MctpMedium fastMedium =
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe";
    const MctpBinding fastBinding =
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe";
    const MctpMedium slowMedium =
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus";
    const MctpBinding slowBinding =
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus";

    InventoryManager manager(reqHandler, instanceIdDb, nullptr, nullptr,
                             descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo);

    manager.mctpEidMap[slowEid] =
        std::make_tuple(uuid, slowMedium, slowBinding);
    MCTPEidInfoPriorityQueue queue;
    queue.push({fastEid, fastMedium, fastBinding});
    manager.mctpInfoMap.emplace(uuid, std::move(queue));
    descriptorMap.emplace(
        slowEid, Descriptors{{PLDM_FWUP_IANA_ENTERPRISE_ID,
                              std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04}}});
    componentInfoMap.emplace(slowEid, ComponentInfo{});

    dbus::MctpInterfaces mctpInterfaces{{uuid, {}}};
    std::string messageError;
    std::string resolution;
    auto responseMsg =
        reinterpret_cast<const pldm_msg*>(getFirmwareParametersResp.data());

    auto co = manager.parseGetFWParametersResponse(
        slowEid, responseMsg, getFwParamsPayloadLen, messageError, resolution,
        mctpInterfaces, false);
    stdexec::sync_wait(std::move(co));

    EXPECT_FALSE(descriptorMap.contains(slowEid));
    EXPECT_FALSE(componentInfoMap.contains(slowEid));
    ASSERT_TRUE(manager.mctpInfoMap.contains(uuid));
    EXPECT_EQ(manager.mctpInfoMap.at(uuid).top().eid, fastEid);
}

TEST_F(InventoryManagerInternalTest,
       parseGetFWParametersResponsePromotesFasterEndpoint)
{
    const pldm::eid oldSlowEid = 11;
    const pldm::eid newFastEid = 3;
    const UUID uuid = "00112233445566778899AABBCCDDEEFF";
    const MctpMedium fastMedium =
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe";
    const MctpBinding fastBinding =
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe";
    const MctpMedium slowMedium =
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus";
    const MctpBinding slowBinding =
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus";

    InventoryManager manager(reqHandler, instanceIdDb, nullptr, nullptr,
                             descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo);

    manager.mctpEidMap[newFastEid] =
        std::make_tuple(uuid, fastMedium, fastBinding);
    MCTPEidInfoPriorityQueue queue;
    queue.push({oldSlowEid, slowMedium, slowBinding});
    manager.mctpInfoMap.emplace(uuid, std::move(queue));
    descriptorMap.emplace(
        oldSlowEid,
        Descriptors{{PLDM_FWUP_IANA_ENTERPRISE_ID,
                     std::vector<uint8_t>{0x0A, 0x0B, 0x0C, 0x0D}}});
    componentInfoMap.emplace(oldSlowEid, ComponentInfo{});

    dbus::MctpInterfaces mctpInterfaces{{uuid, {}}};
    std::string messageError;
    std::string resolution;
    auto responseMsg =
        reinterpret_cast<const pldm_msg*>(getFirmwareParametersResp.data());

    auto co = manager.parseGetFWParametersResponse(
        newFastEid, responseMsg, getFwParamsPayloadLen, messageError,
        resolution, mctpInterfaces, false);
    stdexec::sync_wait(std::move(co));

    EXPECT_FALSE(descriptorMap.contains(oldSlowEid));
    EXPECT_FALSE(componentInfoMap.contains(oldSlowEid));
    ASSERT_TRUE(manager.mctpInfoMap.contains(uuid));
    EXPECT_EQ(manager.mctpInfoMap.at(uuid).top().eid, newFastEid);
}

TEST_F(InventoryManagerInternalTest, discoverFDsHandlesEmptyEndpointList)
{
    InventoryManager manager(reqHandler, instanceIdDb, nullptr, nullptr,
                             descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo);

    MctpInfos mctpInfos{};
    dbus::MctpInterfaces mctpInterfaces{};

    manager.discoverFDs(mctpInfos, mctpInterfaces);
    ASSERT_TRUE(manager.discoverFDsTaskHandle.has_value());
    stdexec::sync_wait(manager.discoverFDsTaskHandle->first.on_empty());

    manager.discoverFDs(mctpInfos, mctpInterfaces);
    ASSERT_TRUE(manager.discoverFDsTaskHandle.has_value());
    stdexec::sync_wait(manager.discoverFDsTaskHandle->first.on_empty());
}

TEST_F(InventoryManagerInternalTest, discoverFDsReturnsEarlyWhenTaskPending)
{
    InventoryManager manager(reqHandler, instanceIdDb, nullptr, nullptr,
                             descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo);

    manager.discoverFDsTaskHandle.emplace();
    MctpInfos mctpInfos{};
    dbus::MctpInterfaces mctpInterfaces{};

    manager.discoverFDs(mctpInfos, mctpInterfaces);

    ASSERT_TRUE(manager.discoverFDsTaskHandle.has_value());
    auto& [scope, rcOpt] = *manager.discoverFDsTaskHandle;
    (void)scope;
    EXPECT_FALSE(rcOpt.has_value());
}

TEST_F(InventoryManagerInternalTest, discoverFDsResetsCompletedTaskHandle)
{
    InventoryManager manager(reqHandler, instanceIdDb, nullptr, nullptr,
                             descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo);

    auto& [scope, rcOpt] = manager.discoverFDsTaskHandle.emplace();
    (void)scope;
    rcOpt.emplace(PLDM_SUCCESS);

    MctpInfos mctpInfos{};
    dbus::MctpInterfaces mctpInterfaces{};
    manager.discoverFDs(mctpInfos, mctpInterfaces);

    ASSERT_TRUE(manager.discoverFDsTaskHandle.has_value());
    stdexec::sync_wait(manager.discoverFDsTaskHandle->first.on_empty());
}

TEST_F(InventoryManagerInternalTest, discoverFDsTaskPopsQueuedEntry)
{
    InventoryManager manager(reqHandler, instanceIdDb, nullptr, nullptr,
                             descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo);

    manager.queuedMctpInfos.emplace(MctpInfos{}, dbus::MctpInterfaces{});
    auto co = manager.discoverFDsTask();
    stdexec::sync_wait(std::move(co));

    EXPECT_TRUE(manager.queuedMctpInfos.empty());
}

TEST_F(InventoryManagerInternalTest, cleanUpResourcesErasesTrackedMaps)
{
    const pldm::eid eid = 33;
    const UUID uuid = "00112233445566778899AABBCCDDEEFF";
    const MctpMedium medium =
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe";
    const MctpBinding binding =
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe";

    InventoryManager manager(reqHandler, instanceIdDb, nullptr, nullptr,
                             descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo);

    manager.mctpEidMap[eid] = std::make_tuple(uuid, medium, binding);
    descriptorMap.emplace(
        eid, Descriptors{{PLDM_FWUP_IANA_ENTERPRISE_ID,
                          std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04}}});

    manager.cleanUpResources(eid);

    EXPECT_FALSE(manager.mctpEidMap.contains(eid));
    EXPECT_FALSE(descriptorMap.contains(eid));
}

TEST_F(InventoryManagerInternalTest, logDeviceStatusErrorsReturnsFalse)
{
    InventoryManager manager(reqHandler, instanceIdDb, nullptr, nullptr,
                             descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo);

    EXPECT_FALSE(manager.logDeviceStatusErrors(1));
}

TEST_F(InventoryManagerInternalTest,
       logDiscoveryFailedMessageWithoutMatchKeepsStateUnchanged)
{
    const pldm::eid eid = 42;
    const UUID uuid = "00112233445566778899AABBCCDDEEFF";
    const MctpMedium medium =
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe";
    const MctpBinding binding =
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe";

    InventoryManager manager(reqHandler, instanceIdDb, nullptr, nullptr,
                             descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo);
    manager.mctpEidMap[eid] = std::make_tuple(uuid, medium, binding);
    dbus::MctpInterfaces mctpInterfaces{{uuid, {}}};

    manager.logDiscoveryFailedMessage(
        eid, "test-message",
        "Retry firmware update operation, if problem persists.", mctpInterfaces,
        "FWUpdate", false);

    EXPECT_TRUE(manager.mctpEidMap.contains(eid));
}

TEST_F(InventoryManagerInternalTest,
       logDiscoveryFailedMessageWithInventoryMatchPath)
{
    const pldm::eid eid = 52;
    const UUID uuid = "00112233445566778899AABBCCDDEEFF";
    const MctpMedium medium =
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe";
    const MctpBinding binding =
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe";
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc0"};

    DeviceInventoryInfo matchingDeviceInventoryInfo(
        {{{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}},
          {{objPath,
            {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"}}},
           {}}}});

    InventoryManager manager(reqHandler, instanceIdDb, nullptr, nullptr,
                             descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, matchingDeviceInventoryInfo);
    manager.mctpEidMap[eid] = std::make_tuple(uuid, medium, binding);

    dbus::MctpInterfaces mctpInterfaces{
        {uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};

    EXPECT_NO_THROW({
        manager.logDiscoveryFailedMessage(
            eid, "test-message",
            "Retry firmware update operation, if problem persists.",
            mctpInterfaces, "FWUpdate", true);
    });
}

TEST(InventoryManagerHeaderInternalTest,
     mctpEidInfoOperatorLessUsesBindingPriorityOnEqualMedium)
{
    const MctpMedium medium =
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe";
    MctpEidInfo fastBinding{
        1, medium, "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe"};
    MctpEidInfo slowBinding{
        2, medium, "xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus"};

    EXPECT_TRUE(slowBinding < fastBinding);
    EXPECT_FALSE(fastBinding < slowBinding);
}

TEST_F(InventoryManagerInternalTest, transportWrapperPathsReturnErrors)
{
    InventoryManager manager(reqHandler, instanceIdDb, nullptr, nullptr,
                             descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo);

    uint64_t supportedTypes = 0;
    auto getTypes = manager.getPLDMTypes(1, supportedTypes);
    auto getTypesRc = stdexec::sync_wait(std::move(getTypes));
    ASSERT_TRUE(getTypesRc.has_value());
    EXPECT_NE(std::get<0>(getTypesRc.value()), PLDM_SUCCESS);

    std::string messageError;
    std::string resolution;
    dbus::MctpInterfaces mctpInterfaces;

    auto queryDevice =
        manager.queryDeviceIdentifiers(1, messageError, resolution);
    auto queryDeviceRc = stdexec::sync_wait(std::move(queryDevice));
    ASSERT_TRUE(queryDeviceRc.has_value());
    EXPECT_NE(std::get<0>(queryDeviceRc.value()), PLDM_SUCCESS);

    auto getFwParams = manager.getFirmwareParameters(
        1, messageError, resolution, mctpInterfaces);
    auto getFwParamsRc = stdexec::sync_wait(std::move(getFwParams));
    ASSERT_TRUE(getFwParamsRc.has_value());
    EXPECT_NE(std::get<0>(getFwParamsRc.value()), PLDM_SUCCESS);

    auto startDiscovery = manager.startFirmwareDiscoveryFlow(1, mctpInterfaces);
    auto startDiscoveryRc = stdexec::sync_wait(std::move(startDiscovery));
    ASSERT_TRUE(startDiscoveryRc.has_value());
    EXPECT_NE(std::get<0>(startDiscoveryRc.value()), PLDM_SUCCESS);

    auto queryDownstream = manager.queryDownstreamDevices(1);
    auto queryDownstreamRc = stdexec::sync_wait(std::move(queryDownstream));
    ASSERT_TRUE(queryDownstreamRc.has_value());
    EXPECT_NE(std::get<0>(queryDownstreamRc.value()), PLDM_SUCCESS);

    auto queryDownstreamIds =
        manager.queryDownstreamIdentifiers(1, 0, PLDM_GET_FIRSTPART);
    auto queryDownstreamIdsRc =
        stdexec::sync_wait(std::move(queryDownstreamIds));
    ASSERT_TRUE(queryDownstreamIdsRc.has_value());
    EXPECT_NE(std::get<0>(queryDownstreamIdsRc.value()), PLDM_SUCCESS);

    auto getDownstreamFw =
        manager.getDownstreamFirmwareParameters(1, 0, PLDM_GET_FIRSTPART);
    auto getDownstreamFwRc = stdexec::sync_wait(std::move(getDownstreamFw));
    ASSERT_TRUE(getDownstreamFwRc.has_value());
    EXPECT_NE(std::get<0>(getDownstreamFwRc.value()), PLDM_SUCCESS);
}

TEST_F(InventoryManagerInternalTest, activeVersionAndRefreshPathsReturnErrors)
{
    const pldm::eid eid = 21;
    const UUID uuid = "00112233445566778899AABBCCDDEEFF";
    const MctpMedium medium =
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe";
    const MctpBinding binding =
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe";

    InventoryManager manager(reqHandler, instanceIdDb, nullptr, nullptr,
                             descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo);
    manager.mctpEidMap[eid] = std::make_tuple(uuid, medium, binding);
    descriptorMap.emplace(
        eid, Descriptors{{PLDM_FWUP_IANA_ENTERPRISE_ID,
                          std::vector<uint8_t>{0x0A, 0x0B, 0x0C, 0x0D}}});
    componentInfoMap.emplace(eid, ComponentInfo{});

    bool callbackCalled = false;
    dbus::MctpInterfaces mctpInterfaces;
    auto getActive = manager.getActiveFirmwareVersion(
        eid, mctpInterfaces, [&](pldm::eid) { callbackCalled = true; });
    auto getActiveRc = stdexec::sync_wait(std::move(getActive));
    ASSERT_TRUE(getActiveRc.has_value());
    EXPECT_NE(std::get<0>(getActiveRc.value()), PLDM_SUCCESS);
    EXPECT_FALSE(callbackCalled);

    // initiateGetActiveFirmwareVersion returns early for unknown EIDs
    const pldm::eid unknownEid = 99;
    auto initiate = manager.initiateGetActiveFirmwareVersion(
        unknownEid, [&](pldm::eid) { callbackCalled = true; });
    auto initiateRc = stdexec::sync_wait(std::move(initiate));
    ASSERT_TRUE(initiateRc.has_value());
    EXPECT_EQ(std::get<0>(initiateRc.value()), PLDM_SUCCESS);
    EXPECT_FALSE(callbackCalled);

    auto refresh = manager.refreshSingleEndpoint(eid, mctpInterfaces, true);
    auto refreshRc = stdexec::sync_wait(std::move(refresh));
    ASSERT_TRUE(refreshRc.has_value());
    EXPECT_NE(std::get<0>(refreshRc.value()), PLDM_SUCCESS);
    EXPECT_FALSE(descriptorMap.contains(eid));
    EXPECT_FALSE(componentInfoMap.contains(eid));
}

TEST_F(InventoryManagerInternalTest, downstreamParserErrorPaths)
{
    InventoryManager manager(reqHandler, instanceIdDb, nullptr, nullptr,
                             descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + 1> invalidResp{
        0x00, 0x00, 0x00, 0x01};
    const auto* response =
        reinterpret_cast<const pldm_msg*>(invalidResp.data());

    auto parseDownstream =
        manager.parseQueryDownstreamDevicesResponse(1, response, 1);
    auto parseDownstreamRc = stdexec::sync_wait(std::move(parseDownstream));
    ASSERT_TRUE(parseDownstreamRc.has_value());
    EXPECT_NE(std::get<0>(parseDownstreamRc.value()), PLDM_SUCCESS);

    auto parseFw =
        manager.parseGetDownstreamFirmwareParametersResponse(1, response, 1);
    auto parseFwRc = stdexec::sync_wait(std::move(parseFw));
    ASSERT_TRUE(parseFwRc.has_value());
    EXPECT_NE(std::get<0>(parseFwRc.value()), PLDM_SUCCESS);
}
