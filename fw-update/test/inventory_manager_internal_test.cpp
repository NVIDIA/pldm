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

#include <boost/asio/io_context.hpp>
#include <com/nvidia/State/DeviceState/server.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <array>
#include <chrono>
#include <coroutine>
#include <thread>

#include <gtest/gtest.h>

using namespace pldm;
using namespace pldm::fw_update;
using namespace std::chrono;

namespace
{

constexpr size_t getFwParamsPayloadLen = 119;

using DeviceState = sdbusplus::server::com::nvidia::state::DeviceState;

class AsyncDbusObjectServer
{
  public:
    explicit AsyncDbusObjectServer(const char* serviceName)
    {
        connection = std::make_shared<sdbusplus::asio::connection>(
            io, sdbusplus::bus::new_bus());
        connection->request_name(serviceName);
        server = std::make_unique<sdbusplus::asio::object_server>(connection);
        ioThread = std::thread([this] { io.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ~AsyncDbusObjectServer()
    {
        io.stop();
        if (ioThread.joinable())
        {
            ioThread.join();
        }
    }

    boost::asio::io_context io;
    std::shared_ptr<sdbusplus::asio::connection> connection;
    std::unique_ptr<sdbusplus::asio::object_server> server;

  private:
    std::thread ioThread;
};

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
    ExcludedFwUpdateEids excludedFwUpdateEids{};
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
        nullptr, reqHandler, instanceIdDb,
        [&](pldm::eid callbackEid, UUID callbackUuid, dbus::MctpInterfaces&) {
            createCallbackCalled = true;
            EXPECT_EQ(callbackEid, eid);
            EXPECT_EQ(callbackUuid, uuid);
        },
        [&](pldm::eid, UUID, dbus::MctpInterfaces&) {
            updateCallbackCalled = true;
        },
        descriptorMap, downstreamDescriptorMap, componentInfoMap,
        deviceInventoryInfo, excludedFwUpdateEids);

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
        nullptr, reqHandler, instanceIdDb,
        [&](pldm::eid, UUID, dbus::MctpInterfaces&) {
            createCallbackCalled = true;
        },
        [&](pldm::eid callbackEid, UUID callbackUuid, dbus::MctpInterfaces&) {
            updateCallbackCalled = true;
            EXPECT_EQ(callbackEid, eid);
            EXPECT_EQ(callbackUuid, uuid);
        },
        descriptorMap, downstreamDescriptorMap, componentInfoMap,
        deviceInventoryInfo, excludedFwUpdateEids);

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

    InventoryManager manager(nullptr, reqHandler, instanceIdDb, nullptr,
                             nullptr, descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo,
                             excludedFwUpdateEids);

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

    InventoryManager manager(nullptr, reqHandler, instanceIdDb, nullptr,
                             nullptr, descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo,
                             excludedFwUpdateEids);

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
    InventoryManager manager(nullptr, reqHandler, instanceIdDb, nullptr,
                             nullptr, descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo,
                             excludedFwUpdateEids);

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
    InventoryManager manager(nullptr, reqHandler, instanceIdDb, nullptr,
                             nullptr, descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo,
                             excludedFwUpdateEids);

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
    InventoryManager manager(nullptr, reqHandler, instanceIdDb, nullptr,
                             nullptr, descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo,
                             excludedFwUpdateEids);

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
    InventoryManager manager(nullptr, reqHandler, instanceIdDb, nullptr,
                             nullptr, descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo,
                             excludedFwUpdateEids);

    manager.queuedMctpInfos.emplace(MctpInfos{}, dbus::MctpInterfaces{});
    auto co = manager.discoverFDsTask();
    stdexec::sync_wait(std::move(co));

    EXPECT_TRUE(manager.queuedMctpInfos.empty());
}

TEST_F(InventoryManagerInternalTest, discoverFDsTaskSkipsExcludedEids)
{
    const pldm::eid excludedEid = 42;
    const UUID uuid = "00112233445566778899AABBCCDDEEFF";
    const MctpMedium medium =
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe";
    const MctpBinding binding =
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe";

    // The manager holds a reference to excludedFwUpdateEids, so populating the
    // fixture member before discovery takes effect.
    excludedFwUpdateEids.insert(excludedEid);

    InventoryManager manager(nullptr, reqHandler, instanceIdDb, nullptr,
                             nullptr, descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo,
                             excludedFwUpdateEids);

    MctpInfos mctpInfos{MctpInfo{excludedEid, uuid, medium, NetworkId{1},
                                 MctpInfoName{}, binding, LocalEid{}}};
    manager.queuedMctpInfos.emplace(mctpInfos, dbus::MctpInterfaces{});

    auto co = manager.discoverFDsTask();
    stdexec::sync_wait(std::move(co));

    // The excluded EID is skipped before being recorded, so no entry is cached
    // and no PLDM discovery traffic is generated for it.
    EXPECT_TRUE(manager.queuedMctpInfos.empty());
    EXPECT_FALSE(manager.mctpEidMap.contains(excludedEid));
    EXPECT_FALSE(descriptorMap.contains(excludedEid));
}

TEST_F(InventoryManagerInternalTest, refreshSingleEndpointSkipsExcludedEid)
{
    const pldm::eid excludedEid = 42;

    excludedFwUpdateEids.insert(excludedEid);

    InventoryManager manager(nullptr, reqHandler, instanceIdDb, nullptr,
                             nullptr, descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo,
                             excludedFwUpdateEids);

    dbus::MctpInterfaces mctpInterfaces;
    auto refresh =
        manager.refreshSingleEndpoint(excludedEid, mctpInterfaces, true);
    auto refreshRc = stdexec::sync_wait(std::move(refresh));

    // Excluded EID returns early without issuing any T5 command
    // (queryDeviceIdentifiers/getFirmwareParameters), so no descriptors are
    // recorded.
    ASSERT_TRUE(refreshRc.has_value());
    EXPECT_EQ(std::get<0>(refreshRc.value()), PLDM_SUCCESS);
    EXPECT_FALSE(descriptorMap.contains(excludedEid));
}

TEST_F(InventoryManagerInternalTest, cleanUpResourcesErasesTrackedMaps)
{
    const pldm::eid eid = 33;
    const UUID uuid = "00112233445566778899AABBCCDDEEFF";
    const MctpMedium medium =
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe";
    const MctpBinding binding =
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe";

    InventoryManager manager(nullptr, reqHandler, instanceIdDb, nullptr,
                             nullptr, descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo,
                             excludedFwUpdateEids);

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
    InventoryManager manager(nullptr, reqHandler, instanceIdDb, nullptr,
                             nullptr, descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo,
                             excludedFwUpdateEids);

    EXPECT_FALSE(manager.logDeviceStatusErrors(1));
}

TEST_F(InventoryManagerInternalTest,
       logDeviceStatusErrorsReturnsTrueWhenDeviceStatusContainsErrors)
{
    AsyncDbusObjectServer loggingService("xyz.openbmc_project.Logging");
    auto statusIface = loggingService.server->add_interface(
        "/com/nvidia/state/device_status/8", "com.nvidia.State.DeviceState");
    statusIface->register_property(
        "DeviceStatus",
        makeStatusMap(DeviceState::DeviceHealth::Degraded,
                      {{7,
                        DeviceState::ErrorClass::MCTP,
                        {{"REDFISH_MESSAGE_ID", "Update.1.0.TransferFailed"},
                         {"REDFISH_MESSAGE_ARGS", "GPU8, 1.2.3"}}},
                       {8,
                        DeviceState::ErrorClass::Recovery,
                        {{"REDFISH_MESSAGE_ID", "Update.1.0.AwaitToActivate"},
                         {"REDFISH_MESSAGE_ARGS", "GPU8, 2.0"}}}}));
    statusIface->initialize();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    InventoryManager manager(nullptr, reqHandler, instanceIdDb, nullptr,
                             nullptr, descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo,
                             excludedFwUpdateEids);

    EXPECT_TRUE(manager.logDeviceStatusErrors(8, true, "CoverageFW"));
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

    InventoryManager manager(nullptr, reqHandler, instanceIdDb, nullptr,
                             nullptr, descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo,
                             excludedFwUpdateEids);
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

    InventoryManager manager(nullptr, reqHandler, instanceIdDb, nullptr,
                             nullptr, descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, matchingDeviceInventoryInfo,
                             excludedFwUpdateEids);
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

// MctpEidInfo::operator< must tolerate empty or unrecognized medium/binding
// strings without throwing; unknown values rank below any known value so a
// priority_queue keeps known entries on top.
TEST(InventoryManagerHeaderInternalTest,
     mctpEidInfoOperatorLessUnknownMediumOrBindingDoesNotThrow)
{
    const MctpMedium pcieMedium =
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe";
    const MctpBinding pcieBinding =
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe";

    MctpEidInfo known{1, pcieMedium, pcieBinding};
    MctpEidInfo emptyMediumBinding{2, "", ""};
    MctpEidInfo unknownMedium{
        3, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.NotARealMedium",
        pcieBinding};
    MctpEidInfo unknownBinding{
        4, pcieMedium,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.NotARealBinding"};

    // Unknown values must never throw and must compare as lower priority.
    EXPECT_NO_THROW({
        EXPECT_TRUE(emptyMediumBinding < known);
        EXPECT_FALSE(known < emptyMediumBinding);
        EXPECT_TRUE(unknownMedium < known);
        EXPECT_FALSE(known < unknownMedium);
        EXPECT_TRUE(unknownBinding < known);
        EXPECT_FALSE(known < unknownBinding);
    });

    // priority_queue uses operator<; pushing entries with unknown values must
    // not blow up the queue.
    EXPECT_NO_THROW({
        std::priority_queue<MctpEidInfo> queue;
        queue.push(emptyMediumBinding);
        queue.push(known);
        queue.push(unknownMedium);
        queue.push(unknownBinding);
        // The known (PCIe/PCIe) entry has the highest priority and should be
        // the first popped from the priority queue.
        ASSERT_FALSE(queue.empty());
        EXPECT_EQ(queue.top().eid, 1);
    });
}

TEST(InventoryManagerHeaderInternalTest,
     mctpEidInfoOperatorLessUsesMediumPriorityWhenMediumsDiffer)
{
    MctpEidInfo fastMedium{
        1, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus"};
    MctpEidInfo slowMedium{
        2, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus",
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe"};

    EXPECT_TRUE(slowMedium < fastMedium);
    EXPECT_FALSE(fastMedium < slowMedium);
}

TEST(InventoryManagerHeaderInternalTest,
     mctpEidInfoPriorityQueueKeepsFastestEndpointOnTop)
{
    MCTPEidInfoPriorityQueue queue;

    queue.push({3, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus",
                "xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus"});
    queue.push({2, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
                "xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus"});
    queue.push({1, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
                "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe"});

    ASSERT_FALSE(queue.empty());
    EXPECT_EQ(queue.top().eid, 1);
}

TEST(InventoryManagerHeaderInternalTest,
     mctpEidInfoOperatorLessTreatsEqualMediumAndBindingAsEquivalent)
{
    const MctpMedium medium =
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe";
    const MctpBinding binding =
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe";
    MctpEidInfo lhs{1, medium, binding};
    MctpEidInfo rhs{2, medium, binding};

    EXPECT_FALSE(lhs < rhs);
    EXPECT_FALSE(rhs < lhs);
}

TEST(InventoryManagerHeaderInternalTest,
     mctpEidInfoPriorityQueueBeginEndCoverEmptyAndFilledQueues)
{
    MCTPEidInfoPriorityQueue queue;
    EXPECT_EQ(std::distance(queue.begin(), queue.end()), 0);

    queue.push({4, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus",
                "xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus"});
    queue.push({5, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
                "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe"});

    const auto count = std::distance(queue.begin(), queue.end());
    EXPECT_EQ(count, 2);
}

TEST_F(InventoryManagerInternalTest,
       destructorHandlesTrackedCoroutineHandlesCoverage)
{
    EXPECT_NO_THROW({
        InventoryManager manager(nullptr, reqHandler, instanceIdDb, nullptr,
                                 nullptr, descriptorMap,
                                 downstreamDescriptorMap, componentInfoMap,
                                 deviceInventoryInfo, excludedFwUpdateEids);

        manager.inventoryCoRoutineHandlers.emplace(8, std::noop_coroutine());
        manager.inventoryCoRoutineHandlers.emplace(9, std::noop_coroutine());
    });
}

TEST_F(InventoryManagerInternalTest,
       parseGetFWParametersResponseWithoutEidMappingSkipsDiscoveryCallbacks)
{
    bool createCallbackCalled = false;
    bool updateCallbackCalled = false;

    InventoryManager manager(
        nullptr, reqHandler, instanceIdDb,
        [&](pldm::eid, UUID, dbus::MctpInterfaces&) {
            createCallbackCalled = true;
        },
        [&](pldm::eid, UUID, dbus::MctpInterfaces&) {
            updateCallbackCalled = true;
        },
        descriptorMap, downstreamDescriptorMap, componentInfoMap,
        deviceInventoryInfo, excludedFwUpdateEids);

    std::string messageError;
    std::string resolution;
    dbus::MctpInterfaces mctpInterfaces;
    auto responseMsg =
        reinterpret_cast<const pldm_msg*>(getFirmwareParametersResp.data());

    auto co = manager.parseGetFWParametersResponse(
        99, responseMsg, getFwParamsPayloadLen, messageError, resolution,
        mctpInterfaces, false);
    stdexec::sync_wait(std::move(co));

    EXPECT_FALSE(createCallbackCalled);
    EXPECT_FALSE(updateCallbackCalled);
    EXPECT_TRUE(componentInfoMap.contains(99));
    EXPECT_TRUE(manager.mctpInfoMap.empty());
}

TEST_F(InventoryManagerInternalTest,
       parseGetFWParametersResponseRefreshOnlySkipsInventoryCallbacks)
{
    bool createCallbackCalled = false;
    bool updateCallbackCalled = false;
    const pldm::eid eid = 12;
    const UUID uuid = "00112233445566778899AABBCCDDEEFF";
    const MctpMedium medium =
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe";
    const MctpBinding binding =
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe";

    InventoryManager manager(
        nullptr, reqHandler, instanceIdDb,
        [&](pldm::eid, UUID, dbus::MctpInterfaces&) {
            createCallbackCalled = true;
        },
        [&](pldm::eid, UUID, dbus::MctpInterfaces&) {
            updateCallbackCalled = true;
        },
        descriptorMap, downstreamDescriptorMap, componentInfoMap,
        deviceInventoryInfo, excludedFwUpdateEids);

    manager.mctpEidMap[eid] = std::make_tuple(uuid, medium, binding);
    std::string messageError;
    std::string resolution;
    dbus::MctpInterfaces mctpInterfaces;
    auto responseMsg =
        reinterpret_cast<const pldm_msg*>(getFirmwareParametersResp.data());

    auto co = manager.parseGetFWParametersResponse(
        eid, responseMsg, getFwParamsPayloadLen, messageError, resolution,
        mctpInterfaces, true);
    stdexec::sync_wait(std::move(co));

    EXPECT_FALSE(createCallbackCalled);
    EXPECT_FALSE(updateCallbackCalled);
    EXPECT_TRUE(componentInfoMap.contains(eid));
    EXPECT_TRUE(manager.mctpInfoMap.empty());
}

TEST_F(InventoryManagerInternalTest,
       parseGetFWParametersResponseFirstEndpointWithoutCreateCallback)
{
    const pldm::eid eid = 13;
    const UUID uuid = "00112233445566778899AABBCCDDEEFF";
    const MctpMedium medium =
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe";
    const MctpBinding binding =
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe";

    InventoryManager manager(nullptr, reqHandler, instanceIdDb, nullptr,
                             nullptr, descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo,
                             excludedFwUpdateEids);

    manager.mctpEidMap[eid] = std::make_tuple(uuid, medium, binding);
    std::string messageError;
    std::string resolution;
    dbus::MctpInterfaces mctpInterfaces;
    auto responseMsg =
        reinterpret_cast<const pldm_msg*>(getFirmwareParametersResp.data());

    auto co = manager.parseGetFWParametersResponse(
        eid, responseMsg, getFwParamsPayloadLen, messageError, resolution,
        mctpInterfaces, false);
    stdexec::sync_wait(std::move(co));

    ASSERT_TRUE(manager.mctpInfoMap.contains(uuid));
    EXPECT_EQ(manager.mctpInfoMap.at(uuid).top().eid, eid);
}

TEST_F(InventoryManagerInternalTest,
       parseGetFWParametersResponseRediscoveryWithoutUpdateCallback)
{
    const pldm::eid eid = 14;
    const UUID uuid = "00112233445566778899AABBCCDDEEFF";
    const MctpMedium medium =
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe";
    const MctpBinding binding =
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe";

    InventoryManager manager(nullptr, reqHandler, instanceIdDb, nullptr,
                             nullptr, descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo,
                             excludedFwUpdateEids);

    manager.mctpEidMap[eid] = std::make_tuple(uuid, medium, binding);
    MCTPEidInfoPriorityQueue queue;
    queue.push({eid, medium, binding});
    manager.mctpInfoMap.emplace(uuid, std::move(queue));

    std::string messageError;
    std::string resolution;
    dbus::MctpInterfaces mctpInterfaces;
    auto responseMsg =
        reinterpret_cast<const pldm_msg*>(getFirmwareParametersResp.data());

    auto co = manager.parseGetFWParametersResponse(
        eid, responseMsg, getFwParamsPayloadLen, messageError, resolution,
        mctpInterfaces, false);
    stdexec::sync_wait(std::move(co));

    ASSERT_TRUE(manager.mctpInfoMap.contains(uuid));
    EXPECT_EQ(manager.mctpInfoMap.at(uuid).top().eid, eid);
}

TEST_F(InventoryManagerInternalTest, transportWrapperPathsReturnErrors)
{
    InventoryManager manager(nullptr, reqHandler, instanceIdDb, nullptr,
                             nullptr, descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo,
                             excludedFwUpdateEids);

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

    InventoryManager manager(nullptr, reqHandler, instanceIdDb, nullptr,
                             nullptr, descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo,
                             excludedFwUpdateEids);
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
    InventoryManager manager(nullptr, reqHandler, instanceIdDb, nullptr,
                             nullptr, descriptorMap, downstreamDescriptorMap,
                             componentInfoMap, deviceInventoryInfo,
                             excludedFwUpdateEids);

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
