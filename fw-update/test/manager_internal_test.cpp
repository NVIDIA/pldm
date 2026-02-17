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

// Clang analyzer reports a false-positive leak on Manager's bound callbacks in
// this test translation unit.
// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)

#include "common/instance_id.hpp"
#include "common/types.hpp"
#include "common/utils.hpp"
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "fw-update/manager.hpp"
#undef private
#include "requester/handler.hpp"
#include "test/test_instance_id.hpp"

#include <xyz/openbmc_project/Software/ApplyTime/server.hpp>

#include <gtest/gtest.h>

using namespace pldm;
using namespace pldm::fw_update;
using namespace std::chrono;

class ManagerInternalTest : public testing::Test
{
  protected:
    ManagerInternalTest() :
        event(sdeventplus::Event::get_default()), instanceIdDb(),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 0,
                   milliseconds(1))
    {}

    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    requester::Handler<requester::Request> reqHandler;
};

TEST_F(ManagerInternalTest, inlineManagerPathsAreCallable)
{
    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(event, reqHandler, instanceIdDb, configPath, true);

    dbus::MctpInterfaces mctpInterfaces;
    EXPECT_NO_THROW({ manager.getMctpInterfaces(mctpInterfaces); });

    MctpInfos mctpInfos{};
    EXPECT_NO_THROW({ manager.handleMctpEndpoints(mctpInfos); });
    EXPECT_NO_THROW({ manager.handleRemovedMctpEndpoints(mctpInfos); });

    const pldm::eid eid = 9;
    const UUID uuid = "00112233445566778899AABBCCDDEEFF";
    EXPECT_NO_THROW({ manager.createInventory(eid, uuid, mctpInterfaces); });
    EXPECT_NO_THROW({ manager.updateInventory(eid, uuid, mctpInterfaces); });

    MctpInfo info{eid,
                  uuid,
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
                  0,
                  std::nullopt,
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe"};
    EXPECT_NO_THROW({
        manager.updateMctpEndpointAvailability(info, true);
        manager.onlineMctpEndpoint(uuid, eid);
        manager.offlineMctpEndpoint(uuid, eid);
    });

    EXPECT_FALSE(manager.getActiveEidByName("does-not-exist").has_value());

    manager.updateManager.setRequestedApplyTime(
        sdbusplus::xyz::openbmc_project::Software::server::ApplyTime::
            RequestedApplyTimes::OnReset);
    EXPECT_FALSE(manager.updateManager.isApplyTimeImmediate());
    manager.updateManager.setRequestedApplyTime(
        sdbusplus::xyz::openbmc_project::Software::server::ApplyTime::
            RequestedApplyTimes::Immediate);
    EXPECT_TRUE(manager.updateManager.isApplyTimeImmediate());
}

TEST_F(ManagerInternalTest, refreshSingleEndpointCallbackPathIsCallable)
{
    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(event, reqHandler, instanceIdDb, configPath, true);

    ASSERT_TRUE(manager.updateManager.refreshSingleEndpointCallback);
    auto co = manager.updateManager.refreshSingleEndpointCallback(1, true);
    auto rc = stdexec::sync_wait(std::move(co));
    ASSERT_TRUE(rc.has_value());
}

TEST_F(ManagerInternalTest, constructorHandlesInvalidConfigPath)
{
    const std::filesystem::path badConfigPath{
        "./fw_update_jsons/does_not_exist.json"};
    EXPECT_NO_THROW({
        Manager manager(event, reqHandler, instanceIdDb, badConfigPath, true);
    });
}

TEST_F(ManagerInternalTest, createAndUpdateInventoryCoverComponentMapBranches)
{
    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(event, reqHandler, instanceIdDb, configPath, true);

    const pldm::eid eid = 7;
    const UUID uuid = "00112233445566778899AABBCCDDEEFF";
    dbus::MctpInterfaces mctpInterfaces{};

    manager.componentInfoMap[eid] = {
        {std::make_pair(static_cast<uint16_t>(1), static_cast<uint16_t>(2)),
         std::make_tuple(static_cast<uint8_t>(1), std::string("v1.2.3"),
                         static_cast<uint16_t>(0))}};
    manager.componentNameMap.clear();

    EXPECT_NO_THROW({ manager.createInventory(eid, uuid, mctpInterfaces); });
    EXPECT_TRUE(manager.componentNameMap.contains(eid));
    EXPECT_FALSE(manager.componentNameMap[eid].empty());

    EXPECT_NO_THROW({ manager.updateInventory(eid, uuid, mctpInterfaces); });
}

TEST_F(ManagerInternalTest, updateFwInventoryAndHandleRequestAreCallable)
{
    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(event, reqHandler, instanceIdDb, configPath, true);

    const pldm::eid eid = 5;
    EXPECT_NO_THROW({ manager.updateFWInventory(eid); });

    const pldm_msg req{};
    EXPECT_NO_THROW({
        [[maybe_unused]] auto resp = manager.handleRequest(
            eid, static_cast<Command>(0), &req, sizeof(req));
    });

    MctpInfos removed{
        {eid, "00112233445566778899AABBCCDDEEFF",
         "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 0, std::nullopt,
         "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe"}};
    EXPECT_NO_THROW({ manager.handleRemovedMctpEndpoints(removed); });
}

TEST_F(ManagerInternalTest,
       createInventoryRetainsExistingComponentNamesWhenAlreadyPresent)
{
    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(event, reqHandler, instanceIdDb, configPath, true);

    const pldm::eid eid = 21;
    const UUID uuid = "00112233445566778899AABBCCDDEEFF";
    dbus::MctpInterfaces mctpInterfaces{};

    manager.componentInfoMap[eid] = {
        {std::make_pair(static_cast<uint16_t>(2), static_cast<uint16_t>(3)),
         std::make_tuple(static_cast<uint8_t>(1), std::string("v9.9.9"),
                         static_cast<uint16_t>(0))}};
    manager.componentNameMap[eid] = {
        {static_cast<uint16_t>(3), "ExistingComponentName"}};

    EXPECT_NO_THROW({ manager.createInventory(eid, uuid, mctpInterfaces); });
    ASSERT_TRUE(manager.componentNameMap.contains(eid));
    EXPECT_EQ(manager.componentNameMap[eid][static_cast<uint16_t>(3)],
              "ExistingComponentName");
}

TEST_F(ManagerInternalTest,
       handleMctpEndpointsNonEmptyInputWhenDiscoveryAlreadyPending)
{
    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(event, reqHandler, instanceIdDb, configPath, true);

    manager.inventoryMgr.discoverFDsTaskHandle.emplace();

    MctpInfos mctpInfos{
        {9, "00112233445566778899AABBCCDDEEFF",
         "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 0, std::nullopt,
         "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe"}};

    EXPECT_NO_THROW({ manager.handleMctpEndpoints(mctpInfos); });
    EXPECT_TRUE(manager.inventoryMgr.queuedMctpInfos.size() >= 1);
}

// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
