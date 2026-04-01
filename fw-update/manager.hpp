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
#include "common/instance_id.hpp"
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

#include <format>
#include <unordered_map>
#include <vector>

namespace pldm
{

class MctpDiscoveryHandlerIntf;

namespace fw_update
{

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
     *  @param[in] fwDebug - Verbosity flag to enable debug traces for fw update
     */
    explicit Manager(Event& event,
                     requester::Handler<requester::Request>& handler,
                     InstanceIdDb& instanceIdDb,
                     const std::filesystem::path& fwUpdateConfigFile,
                     bool fwDebug) :
        inventoryMgr(handler, instanceIdDb,
                     std::bind_front(&Manager::createInventory, this),
                     std::bind_front(&Manager::updateInventory, this),
                     descriptorMap, downstreamDescriptorMap, componentInfoMap,
                     deviceInventoryInfo),
        updateManager(event, handler, instanceIdDb, descriptorMap,
                      componentInfoMap, componentNameMap, fwDebug,
                      // manager_internal_test.cpp triggers a false-positive
                      // clang-analyzer-cplusplus.NewDeleteLeaks here:
                      // std::function allocates callback storage during
                      // Manager construction, but the analyzer loses track of
                      // that ownership and reports the error on this line.
                      // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
                      [this](mctp_eid_t eid, bool isTarget) -> exec::task<int> {
                          dbus::MctpInterfaces mctpInterfaces;
                          getMctpInterfaces(mctpInterfaces);

                          co_return co_await inventoryMgr.refreshSingleEndpoint(
                              eid, mctpInterfaces, isTarget);
                      }),
        deviceInventoryManager(pldm::utils::DBusHandler::getBus(),
                               deviceInventoryInfo, descriptorMap),
        fwInventoryManager(pldm::utils::DBusHandler::getBus(), fwInventoryInfo,
                           componentInfoMap, componentNameMap)
    {
        try
        {
            parseConfig(fwUpdateConfigFile, deviceInventoryInfo,
                        fwInventoryInfo, componentNameMapInfo);
        }
        catch (const std::exception& e)
        {
            error("Error while parsing json.", "ERROR", e);
        }
    }

    /** @brief Discover MCTP endpoints that support the PLDM firmware update
     *         specification and create component name information for creating
     *         message registry entries.
     *
     *  @param[in] mctpInfos - <EID, UUID> for every MCTP endpoint
     *  @param[in] signalMctpIfMap - UUID→InterfaceMap cache built from the
     *             InterfacesAdded signal payload; empty on the startup path
     */
    void handleMctpEndpoints(
        const MctpInfos& mctpInfos,
        const dbus::MctpInterfaces& signalMctpIfMap) override
    {
        // Use the signal payload cache to avoid ObjectMapper calls during boot.
        // Fall back to a full D-Bus scan only for UUIDs not covered by the
        // cache (startup path: cache is empty, ObjectMapper is ready).
        dbus::MctpInterfaces mctpInterfaces = signalMctpIfMap;

        bool needFallback = std::any_of(
            mctpInfos.begin(), mctpInfos.end(), [&](const auto& info) {
                return !mctpInterfaces.contains(std::get<1>(info));
            });
        if (needFallback)
        {
            warning(
                "handleMctpEndpoints: signal cache missing UUIDs, falling back to D-Bus scan");
            getMctpInterfaces(mctpInterfaces);
        }

        inventoryMgr.discoverFDs(mctpInfos, mctpInterfaces);
        for (const auto& [eid, uuid, mediumType, networkId, _, bindingType,
                          localEid] : mctpInfos)
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
    void createInventory(eid eid, UUID uuid,
                         dbus::MctpInterfaces& mctpInterfaces)
    {
        deviceInventoryManager.createEntry(eid, uuid, mctpInterfaces);
        if (componentInfoMap.contains(eid))
        {
            // If no config JSON match, populate componentNameMap with
            // generated names for target filtering, logging, and D-Bus object
            // path consistency
            if (!componentNameMap.contains(eid))
            {
                ComponentIdNameMap componentIdNameMap;
                for (const auto& [compKey, compInfo] : componentInfoMap[eid])
                {
                    componentIdNameMap[compKey.second] = std::format(
                        "PLDM_Device_Firmware_Device_{}_Component_{}_{}",
                        static_cast<int>(eid), compKey.second,
                        pldm::utils::generateSwId());
                }
                componentNameMap[eid] = std::move(componentIdNameMap);
            }
            fwInventoryManager.createEntry(eid, uuid, mctpInterfaces);
        }
    }

    /** @brief Update device and firmware inventory based on refreshed
     *         descriptor and firmware parameter information
     *
     *  @param[in] eid - MCTP endpoint
     *  @param[in] uuid - MCTP UUID
     *  @param[in] mctpInterfaces - MCTP interface information
     */
    void updateInventory(eid eid, UUID uuid,
                         dbus::MctpInterfaces& mctpInterfaces)
    {
        deviceInventoryManager.updateEntry(eid, uuid, mctpInterfaces);
        if (componentInfoMap.contains(eid))
        {
            fwInventoryManager.updateEntry(eid, uuid, mctpInterfaces);
        }
    }

    /** @brief Update Active Firmware Version for the given eid
     * This method is called whenever platform event is received for firmware
     * version change.
     *  Data Structure containing active firmware version is updated for all the
     * components associated with the input eid in the
     * initiateGetActiveFirmwareVersion method, and then in updateFWVersion
     * method, for each component, dbus is updated only if there's a change in
     * firmware version
     *
     *  @param[in] eid - MCTP endpoint
     */
    void updateFWInventory(eid eid)
    {
        try
        {
            UpdateFWVersionCallBack updateFWVersionCallback =
                [this](uint8_t eid) {
                    this->fwInventoryManager.updateFWVersion(eid);
                };
            [[maybe_unused]] auto co =
                inventoryMgr.initiateGetActiveFirmwareVersion(
                    eid, updateFWVersionCallback);
        }
        catch (const std::exception& e)
        {
            error("Error while updating Firmware version.", "ERROR", e);
        }
    }

    /** @brief Helper function to invoke registered handlers for
     *         the removed MCTP endpoints
     *
     *  Clears cached data associated with the removed endpoints:
     *  - descriptorMap
     *  - mctpEidMap
     *
     *  D-Bus objects (device/firmware inventory) are intentionally
     *  preserved as they were created during initial discovery.
     *
     *  @param[in] mctpInfos - information of removed MCTP endpoints
     */
    void handleRemovedMctpEndpoints(const MctpInfos& mctpInfos) override
    {
        for (const auto& mctpInfo : mctpInfos)
        {
            auto eid = std::get<pldm::eid>(mctpInfo);
            inventoryMgr.cleanUpResources(eid);
        }
    }

    /** @brief Helper function to invoke registered handlers for
     *  updating the availability status of the MCTP endpoint
     *
     *  @param[in] mctpInfo - information of the target endpoint
     *  @param[in] availability - new availability status
     */
    void updateMctpEndpointAvailability(const MctpInfo&, Availability) override
    {
        return;
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

    /** @brief Notify that a PLDM FW update response has been sent
     *
     *  @param[in] eid - Remote MCTP Endpoint ID
     *  @param[in] success - true if sendMsg succeeded
     */
    void onResponseSendComplete(mctp_eid_t eid, bool success)
    {
        updateManager.onResponseSendComplete(eid, success);
    }

    void onlineMctpEndpoint([[maybe_unused]] const UUID& uuid,
                            [[maybe_unused]] const eid& eid) override
    {
        this->updateFWInventory(eid);
    }

    void offlineMctpEndpoint([[maybe_unused]] const UUID& uuid,
                             [[maybe_unused]] const eid& eid) override
    {
        // placeholder
    }

    /** @brief Get Active EIDs.
     *
     *  @param[in] addr - MCTP address of terminus
     *  @param[in] terminiNames - MCTP terminus name
     */
    std::optional<mctp_eid_t> getActiveEidByName(const std::string&) override
    {
        return std::nullopt;
    }

  private:
    /** @brief Get MCTP interfaces from D-Bus
     *
     *  Queries D-Bus for MCTP endpoint information and builds the
     *  mctpInterfaces map used for inventory operations.
     *
     *  @param[out] mctpInterfaces - Map of UUID to interface properties
     */
    void getMctpInterfaces(dbus::MctpInterfaces& mctpInterfaces)
    {
        dbus::ObjectValueTree objects;
        std::set<dbus::Service> mctpCtrlServices;
        auto& bus = pldm::utils::DBusHandler::getBus();
        const dbus::Interfaces ifaceList{"xyz.openbmc_project.MCTP.Endpoint"};
        pldm::utils::GetSubTreeResponse getSubTreeResponse;

        try
        {
            getSubTreeResponse = utils::DBusHandler().getSubtree(
                "/au/com/codeconstruct/mctp1/networks", 0, ifaceList);
        }
        catch (const sdbusplus::exception_t& e)
        {
            error(
                "Failed to getSubtree call at path '{PATH}' and interface '{INTERFACE}', error - {ERROR} ",
                "ERROR", e, "PATH", MCTPPath, "INTERFACE", MCTPInterface);
            return;
        }

        for (const auto& [objPath, mapperServiceMap] : getSubTreeResponse)
        {
            for (const auto& [serviceName, interfaces] : mapperServiceMap)
            {
                mctpCtrlServices.emplace(serviceName);
            }
        }

        for (const auto& service : mctpCtrlServices)
        {
            try
            {
                auto method = bus.new_method_call(
                    service.c_str(), "/au/com/codeconstruct/mctp1",
                    "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
                auto reply = bus.call(method);
                reply.read(objects);
            }
            catch (const sdbusplus::exception_t& e)
            {
                error(
                    "Failed to GetManagedObjects call at path '{PATH}' and service '{SERVICE}', error - {ERROR} ",
                    "ERROR", e, "PATH", MCTPPath, "SERVICE", service);
                continue;
            }

            for (const auto& [objectPath, interfaces] : objects)
            {
                for (const auto& [intfName, properties] : interfaces)
                {
                    if (intfName == EndpointUUID)
                    {
                        std::string uuid =
                            std::get<std::string>(properties.at("UUID"));
                        mctpInterfaces[uuid] = interfaces;
                    }
                }
            }
        }
    }

    /** @brief Descriptor information of all the discovered MCTP endpoints */
    DescriptorMap descriptorMap;

    /** Downstream descriptor information of all the discovered MCTP endpoints
     */
    DownstreamDescriptorMap downstreamDescriptorMap;

    /** Component information of all the discovered MCTP endpoints */
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
