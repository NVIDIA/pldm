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
#pragma once

#include "activation.hpp"
#include "common/types.hpp"
#include "common/utils.hpp"
#include "config.hpp"
#include "device_inventory.hpp"
#include "device_updater.hpp"
#include "firmware_inventory.hpp"
#include "inventory_manager.hpp"
#include "requester/handler.hpp"
#include "requester/mctp_endpoint_discovery.hpp"
#include "update_manager.hpp"

#include <libpldm/pldm.h>

#include <phosphor-logging/lg2.hpp>

#include <unordered_map>
#include <vector>

namespace pldm
{

namespace fw_update
{

class MctpDiscoveryHandlerIntf;

/** @class Manager
 *
 * This class handles all the aspects of the PLDM FW update specification for
 * the MCTP devices
 */
class Manager : public pldm::MctpDiscoveryHandlerIntf
{

  public:
    Manager() = delete;
    Manager(const Manager&) = delete;
    Manager(Manager&&) = delete;
    Manager& operator=(const Manager&) = delete;
    Manager& operator=(Manager&&) = delete;
    ~Manager() = default;

    /** @brief Constructor
     *
     *  @param[in] event - reference to PLDM daemon's main event loop
     *  @param[in] handler - PLDM request handler
     *  @param[in] requester - Managing instance ID for PLDM requests
     *  @param[in] fwUpdateConfigFile - Config file for firmware update
     *  @param[in] dBusHandlerIntf - Interface to make D-Bus client calls
     *  @param[in] fwDebug - Verbosity flag to enable debug traces for fw update
     */
    explicit Manager(Event& event,
                     requester::Handler<requester::Request>& handler,
                     InstanceIdDb& instanceIdDb,
                     const std::filesystem::path& fwUpdateConfigFile,
                     utils::DBusHandlerInterface* dBusHandlerIntf,
                     bool fwDebug) :
        inventoryMgr(handler, instanceIdDb,
                     std::bind_front(&Manager::createInventory, this),
                     descriptorMap, componentInfoMap, deviceInventoryInfo),
        updateManager(event, handler, instanceIdDb, descriptorMap,
                      componentInfoMap, componentNameMap, fwDebug),
        deviceInventoryManager(pldm::utils::DBusHandler::getBus(),
                               deviceInventoryInfo, descriptorMap,
                               dBusHandlerIntf),
        fwInventoryManager(pldm::utils::DBusHandler::getBus(), fwInventoryInfo,
                           componentInfoMap, dBusHandlerIntf)
    {
        try
        {
            parseConfig(fwUpdateConfigFile, deviceInventoryInfo,
                        fwInventoryInfo, componentNameMapInfo);
        }
        catch (const std::exception& e)
        {
            lg2::error("Error while parsing json.", "ERROR", e);
        }
    }

    /** @brief Discover MCTP endpoints that support the PLDM firmware update
     *         specification and create component name information for creating
     *         message registry entries.
     *
     *  @param[in] mctpInfos - <EID, UUID> for every MCTP endpoint
     */
    void handleMctpEndpoints(const MctpInfos& mctpInfos) override
    {
        std::vector<mctp_eid_t> eids;
        for (auto& mctpInfo : mctpInfos)
        {
            eids.emplace_back(std::get<0>(mctpInfo));
        }
        dbus::ObjectValueTree objects;
        std::set<dbus::Service> mctpCtrlServices;
        dbus::MctpInterfaces mctpInterfaces;
        auto& bus = pldm::utils::DBusHandler::getBus();
        std::string uuid;
        const dbus::Interfaces ifaceList{"xyz.openbmc_project.MCTP.Endpoint"};
        auto getSubTreeResponse = utils::DBusHandler().getSubtree(
            "/au/com/codeconstruct/mctp1/networks", 0, ifaceList);
        for (const auto& [objPath, mapperServiceMap] : getSubTreeResponse)
        {
            for (const auto& [serviceName, interfaces] : mapperServiceMap)
            {
                mctpCtrlServices.emplace(serviceName);
            }
        }
        for (const auto& service : mctpCtrlServices)
        {
            auto method = bus.new_method_call(
                service.c_str(), "/au/com/codeconstruct/mctp1",
                "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
            auto reply = bus.call(method);
            reply.read(objects);
            for (const auto& [objectPath, interfaces] : objects)
            {
                for (const auto& [intfName, properties] : interfaces)
                {
                    if (intfName == uuidEndpointIntfName)
                    {
                        uuid = std::get<std::string>(properties.at("UUID"));
                        mctpInterfaces[uuid] = interfaces;
                    }
                }
            }
        }

        inventoryMgr.discoverFDs(mctpInfos, mctpInterfaces);
        for (const auto& [eid, uuid, networkId] : mctpInfos)
        {
            ComponentIdNameMap componentIdNameMap;
            if (componentNameMapInfo.matchInventoryEntry(mctpInterfaces[uuid],
                                                         componentIdNameMap))
            {
                componentNameMap[eid] = componentIdNameMap;
            }
        }
    }

    /** @brief Create device and firmware inventory based on the firmware update
     *         config file and firmware inventory commands
     *
     *  @param[in] eid - MCTP endpoint
     *  @param[in] uuid - MCTP UUID
     */
    void createInventory(EID eid, UUID uuid,
                         dbus::MctpInterfaces& mctpInterfaces)
    {
        deviceInventoryManager.createEntry(eid, uuid, mctpInterfaces);
        if (componentInfoMap.contains(eid))
        {
            fwInventoryManager.createEntry(eid, uuid, mctpInterfaces);
        }
    }

    /** @brief Update Active Firmware Version for the given EID
     * This method is called whenever platform event is received for firmware
     * version change.
     *  Data Structure containing active firmware version is updated for all the
     * components associated with the input EID in the
     * initiateGetActiveFirmwareVersion method, and then in updateFWVersion
     * method, for each component, dbus is updated only if there's a change in
     * firmware version
     *
     *  @param[in] eid - MCTP endpoint
     */
    void updateFWInventory(EID eid)
    {
        try
        {
            UpdateFWVersionCallBack updateFWVersionCallback = [this](EID eid) {
                this->fwInventoryManager.updateFWVersion(eid);
            };
            inventoryMgr.initiateGetActiveFirmwareVersion(
                eid, updateFWVersionCallback);
        }
        catch (const std::exception& e)
        {
            lg2::error("Error while updating Firmware version.", "ERROR", e);
        }
    }

    /** @brief Handle PLDM request for the commands in the FW update
     *         specification
     *
     *  @param[in] eid - Remote MCTP Endpoint ID
     *  @param[in] command - PLDM command code
     *  @param[in] request - PLDM request message
     *  @param[in] requestLen - PLDM request message length
     *
     *  @return PLDM response message
     */
    Response handleRequest(mctp_eid_t eid, Command command,
                           const pldm_msg* request, size_t reqMsgLen)
    {
        return updateManager.handleRequest(eid, command, request, reqMsgLen);
    }

    void onlineMctpEndpoint([[maybe_unused]] const UUID& uuid,
                            [[maybe_unused]] const EID& eid)
    {
        this->updateFWInventory(eid);
    }

    void offlineMctpEndpoint([[maybe_unused]] const UUID& uuid,
                             [[maybe_unused]] const EID& eid)
    {
        // placeholder
    }

  private:
    /** @brief Descriptor information of all the discovered MCTP endpoints */
    DescriptorMap descriptorMap;

    /** @brief Component information of all the discovered MCTP endpoints */
    ComponentInfoMap componentInfoMap;

    /** @brief PLDM firmware inventory manager */
    InventoryManager inventoryMgr;

    /** @brief PLDM firmware update manager */
    UpdateManager updateManager;

    /** @brief Config info to create D-Bus device inventory */
    DeviceInventoryInfo deviceInventoryInfo;

    /** @brief Config info to create D-Bus firmware inventory */
    FirmwareInventoryInfo fwInventoryInfo;

    /** @brief Config info to create message registry entries for fw update */
    ComponentNameMapInfo componentNameMapInfo;

    /** @brief Component information to create message registries */
    ComponentNameMap componentNameMap;

    /** @brief Device inventory D-Bus object manager */
    device_inventory::Manager deviceInventoryManager;

    /** @brief Firmware inventory D-Bus object manager */
    fw_inventory::Manager fwInventoryManager;
};

} // namespace fw_update

} // namespace pldm
