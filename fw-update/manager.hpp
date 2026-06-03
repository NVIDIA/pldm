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
#include "device_updater.hpp"
#include "firmware_inventory.hpp"
#include "inventory_manager.hpp"
#include "requester/handler.hpp"
#include "requester/mctp_endpoint_discovery.hpp"
#include "update_manager.hpp"

#include <libpldm/pldm.h>

#include <exec/start_detached.hpp>
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

    /**
     * @brief Constructor for the PLDM Firmware Update Manager
     *
     *  @param[in] event - reference to PLDM daemon's main event loop
     *  @param[in] handler - PLDM request handler
     *  @param[in] requester - Managing instance ID for PLDM requests
     *  @param[in] fwUpdateConfigFile - Config file for firmware update
     *  @param[in] fwDebug - Verbosity flag to enable debug traces for fw update
     */
    explicit Manager(const pldm::utils::DBusHandler* dbusHandler, Event& event,
                     requester::Handler<requester::Request>& handler,
                     InstanceIdDb& instanceIdDb,
                     const std::filesystem::path& fwUpdateConfigFile,
                     bool fwDebug) :
        inventoryMgr(dbusHandler, handler, instanceIdDb,
                     std::bind_front(&Manager::createInventory, this),
                     std::bind_front(&Manager::updateInventory, this),
                     descriptorMap, downstreamDescriptorMap, componentInfoMap),
        updateManager(event, handler, instanceIdDb, descriptorMap,
                      componentInfoMap, componentNameMap, fwDebug,
                      // manager_internal_test.cpp triggers false-positive
                      // clang-analyzer-unix.Malloc and
                      // clang-analyzer-cplusplus.NewDeleteLeaks diagnostics.
                      // NOLINTNEXTLINE
                      [this](mctp_eid_t eid, bool isTarget) -> exec::task<int> {
                          dbus::MctpInterfaces mctpInterfaces;
                          getMctpInterfaces(mctpInterfaces);

                          co_return co_await inventoryMgr.refreshSingleEndpoint(
                              eid, mctpInterfaces, isTarget);
                      }),
        fwInventoryManager(pldm::utils::DBusHandler::getBus(), fwInventoryInfo,
                           componentInfoMap, componentNameMap)
    {
        try
        {
            parseConfig(fwUpdateConfigFile, fwInventoryInfo,
                        componentNameMapInfo);
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
            // PLDM FW Update Config Migration (DGXOPENBMC-25121), SADD §3.3.2
            // step 3b/3d: source per-component naming/Associations/Manufacturer
            // from the entity-manager Configuration.PLDMFirmwareDevice.Components
            // array (joined by MCTPTargetName). Falls back to the legacy
            // JSON-parsed componentNameMapInfo only if no EM entry is present.
            if (!populateComponentInfoFromEM(eid))
            {
                ComponentIdNameMap componentIdNameMap;
                if (componentNameMapInfo.matchInventoryEntry(
                        mctpInterfaces[uuid], componentIdNameMap))
                {
                    componentNameMap[eid] = componentIdNameMap;
                }
            }
        }
    }

    /** @brief Create firmware inventory and write the device UUID to the
     *         entity-manager-owned RoT chassis (PLDM FW Update Config
     *         Migration, DGXOPENBMC-25121).
     *
     *  RoT chassis objects are no longer created by pldmd; entity-manager owns
     *  them via Configuration.PLDMDeviceInventory.CreateInventoryPath. pldmd
     *  writes only the dynamic Common.UUID to that EM-created object (and to the
     *  optional UpdateInventoryPath if declared).
     *
     *  @param[in] eid - MCTP endpoint
     *  @param[in] uuid - MCTP UUID
     */
    void createInventory(eid eid, UUID uuid,
                         dbus::MctpInterfaces& mctpInterfaces)
    {
        writeDeviceInventoryUuid(eid, uuid);
        if (componentInfoMap.contains(eid))
        {
            // FCM-REQ-16: ensure EVERY component the device reports has a name,
            // even those absent from the EM Components array. EM-declared names
            // (from populateComponentInfoFromEM) are kept; any device-reported
            // component without one gets the SADD fallback
            // "<targetName>_C<ComponentIdentifier>" (or a generated name when no
            // configured_by target name is available).
            const std::string targetName = getTargetNameForEid(eid);
            auto& componentIdNameMap = componentNameMap[eid];
            for (const auto& [compKey, compInfo] : componentInfoMap[eid])
            {
                if (componentIdNameMap.contains(compKey.second))
                {
                    continue;
                }
                if (!targetName.empty())
                {
                    componentIdNameMap[compKey.second] = std::format(
                        "{}_C{}", targetName, compKey.second);
                }
                else
                {
                    componentIdNameMap[compKey.second] = std::format(
                        "PLDM_Device_Firmware_Device_{}_Component_{}_{}",
                        static_cast<int>(eid), compKey.second,
                        pldm::utils::generateSwId());
                }
            }
            fwInventoryManager.createEntry(eid, uuid, mctpInterfaces);
        }
    }

    /** @brief Update firmware inventory based on refreshed descriptor and
     *         firmware parameter information, and re-write the device UUID to
     *         the entity-manager-owned RoT chassis.
     *
     *  @param[in] eid - MCTP endpoint
     *  @param[in] uuid - MCTP UUID
     *  @param[in] mctpInterfaces - MCTP interface information
     */
    void updateInventory(eid eid, UUID uuid,
                         dbus::MctpInterfaces& mctpInterfaces)
    {
        writeDeviceInventoryUuid(eid, uuid);
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
            exec::start_detached(stdexec::on(
                stdexec::inline_scheduler{},
                inventoryMgr.initiateGetActiveFirmwareVersion(
                    eid, updateFWVersionCallback) |
                    stdexec::then([eid](int rc) {
                        if (rc)
                        {
                            error(
                                "Failed to refresh firmware version for EID={EID}, RC={RC}",
                                "EID", eid, "RC", rc);
                        }
                    })));
        }
        catch (const std::exception& e)
        {
            error("Error while updating Firmware version.", "ERROR", e);
        }
    }

    void handleConfigurations(const Configurations& configurations) override
    {
        this->configurations = configurations;
        inventoryMgr.setConfigurations(configurations);
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
        inventoryMgr.removeFDs(mctpInfos);
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
                        auto uuidIt = properties.find("UUID");
                        if (uuidIt == properties.end())
                        {
                            continue;
                        }
                        const auto* uuidPtr =
                            std::get_if<std::string>(&uuidIt->second);
                        if (uuidPtr == nullptr)
                        {
                            continue;
                        }
                        mctpInterfaces[*uuidPtr] = interfaces;
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

    /** Configuration bindings from the Entity Manager */
    Configurations configurations;

    /** @brief Config info to create D-Bus firmware inventory */
    FirmwareInventoryInfo fwInventoryInfo;

    /** @brief Config info to create message registry entries for fw update */
    ComponentNameMapInfo componentNameMapInfo;

    /** @brief EIDs excluded from PLDM T5 firmware discovery */
    ExcludedFwUpdateEids excludedFwUpdateEids;

    /** @brief Component information to create message registries */
    ComponentNameMap componentNameMap;

    /** @brief PLDM firmware inventory/discovery manager */
    InventoryManager inventoryMgr;

    /** @brief PLDM firmware update manager */
    UpdateManager updateManager;

    /** @brief Firmware inventory D-Bus object manager */
    fw_inventory::Manager fwInventoryManager;

    /** @brief Write the device UUID to the entity-manager-owned RoT chassis.
     *
     *  PLDM FW Update Config Migration (DGXOPENBMC-25121), SADD §3.2.2/§3.3.2
     *  step 3e. pldmd creates no inventory objects: the RoT chassis is created
     *  by entity-manager from Configuration.PLDMDeviceInventory. This helper
     *  resolves the device's PLDMDeviceInventory config entry by string-match
     *  on MCTPTargetName, then writes the dynamic device UUID
     *  (xyz.openbmc_project.Common.UUID) to CreateInventoryPath and, if set, to
     *  the optional UpdateInventoryPath. No Decorator.SKU write (ECSKU/APSKU
     *  descoped).
     *
     *  @param[in] eid - MCTP endpoint
     *  @param[in] uuid - device UUID obtained over MCTP/PLDM
     */
    /** @brief Resolve a device's friendly Name (the MCTPTargetName join key)
     *         for a discovered endpoint.
     *
     *  PLDM FW Update Config Migration (DGXOPENBMC-25121), SADD §3.3.2 Step 2.
     *
     *  Identity is resolved via configured_by ONLY: the configurations map is
     *  keyed by the entity-manager config path that the endpoint's configured_by
     *  association resolves to; the stored name is the transport object's
     *  `.Name`, which equals the MCTPTargetName cited by every PLDM-* entry for
     *  the same device. This is the canonical, transport-agnostic key.
     *
     *  StaticEID plays NO part in identity resolution (SADD §3.3.2 Step 2,
     *  FCM-REQ-11): the optional PLDMFirmwareDevice.StaticEID is used solely to
     *  seed the pre-FW-update descriptor refresh in UpdateManager::processStream
     *  (SADD §2.3 / §3.3.2), never to resolve a device's identity, inventory, or
     *  the MCTPTargetName join. If configured_by is absent the name is empty and
     *  the endpoint is treated as not (yet) resolvable.
     *
     *  @param[in] eid - MCTP endpoint
     *  @return the device's target Name, empty if not resolved
     */
    std::string getTargetNameForEid(eid eid) const
    {
        // configured_by-resolved name (the only identity path).
        for (const auto& [emPath, mctpInfo] : configurations)
        {
            if (std::get<pldm::eid>(mctpInfo) != eid)
            {
                continue;
            }
            const auto& nameOpt =
                std::get<std::optional<std::string>>(mctpInfo);
            if (nameOpt)
            {
                return *nameOpt;
            }
            break;
        }

        return {};
    }

    /** @brief Populate per-component naming/Associations/Manufacturer from the
     *         entity-manager Configuration.PLDMFirmwareDevice.Components array.
     *
     *  PLDM FW Update Config Migration (DGXOPENBMC-25121), SADD §3.3.2 step 3b.
     *  Resolves the device's target Name via getTargetNameForEid (configured_by
     *  only — SADD §3.3.2 Step 2), runs a global ObjectMapper GetSubTree for
     *  Configuration.PLDMFirmwareDevice, keeps the entry whose MCTPTargetName
     *  equals the target Name, unpacks the nested
     *  Components array (D-Bus aa{sv}) into a ComponentIdentifier → {Name,
     *  Associations, Manufacturer} map, and hands it to the firmware inventory
     *  manager (drives one Software.Version per component with RelatedItem and
     *  per-component update Task name — FCM-REQ-13/14/17). Also seeds
     *  componentNameMap[eid] (id → Name) for target filtering and the
     *  FCM-REQ-16 fallback.
     *
     *  @param[in] eid - MCTP endpoint
     *  @return true if a matching PLDMFirmwareDevice entry was found and applied
     */
    bool populateComponentInfoFromEM(eid eid)
    {
        const std::string targetName = getTargetNameForEid(eid);
        if (targetName.empty())
        {
            return false;
        }

        constexpr auto pldmFwDeviceIntf =
            "xyz.openbmc_project.Configuration.PLDMFirmwareDevice";

        pldm::utils::GetSubTreeResponse subtree;
        try
        {
            subtree = pldm::utils::DBusHandler().getSubtree(
                "/xyz/openbmc_project/inventory", 0, {pldmFwDeviceIntf});
        }
        catch (const std::exception& e)
        {
            warning(
                "populateComponentInfoFromEM: GetSubTree for PLDMFirmwareDevice failed for EID {EID}, error - {ERROR}",
                "EID", eid, "ERROR", e);
            return false;
        }

        for (const auto& [objPath, serviceMap] : subtree)
        {
            if (serviceMap.empty())
            {
                continue;
            }
            const std::string service = serviceMap.begin()->first;

            pldm::utils::PropertyMap props;
            try
            {
                props = pldm::utils::DBusHandler().getDbusPropertiesVariant(
                    service.c_str(), objPath.c_str(), pldmFwDeviceIntf);
            }
            catch (const std::exception& e)
            {
                warning(
                    "populateComponentInfoFromEM: reading props at {PATH} failed, error - {ERROR}",
                    "PATH", objPath, "ERROR", e);
                continue;
            }

            auto nameIt = props.find("MCTPTargetName");
            if (nameIt == props.end() ||
                std::get<std::string>(nameIt->second) != targetName)
            {
                continue;
            }

            CreateComponentIdNameMap emComponents;
            ComponentIdNameMap idNameMap;
            unpackComponents(props, emComponents, idNameMap);

            fwInventoryManager.setEmComponentObjects(eid, emComponents);
            if (!idNameMap.empty())
            {
                componentNameMap[eid] = std::move(idNameMap);
            }
            return true;
        }
        return false;
    }

    /** @brief Unpack the nested Components array (aa{sv}) of a
     *         Configuration.PLDMFirmwareDevice entry. The EM entity-manager
     *         publishes nested arrays-of-objects flattened on D-Bus; each
     *         element carries Name, ComponentIdentifier, optional Manufacturer,
     *         and optional Associations (Forward/Backward/AbsolutePath triples).
     *
     *  @param[in]  props        - PLDMFirmwareDevice property map
     *  @param[out] emComponents - id → {Name, Associations, Manufacturer}
     *  @param[out] idNameMap    - id → Name (for target filtering / fallback)
     */
    static void unpackComponents(const pldm::utils::PropertyMap& props,
                                 CreateComponentIdNameMap& emComponents,
                                 ComponentIdNameMap& idNameMap)
    {
        // entity-manager exposes a nested object array's element count under a
        // top-level property and each element's fields under indexed keys (the
        // established EM nested-array encoding, mirroring sensor Thresholds).
        // We tolerate either an explicit count or scan indexed keys until a gap.
        for (size_t index = 0;; ++index)
        {
            const std::string prefix = "Components" + std::to_string(index);
            auto nameIt = props.find(prefix + "Name");
            auto idIt = props.find(prefix + "ComponentIdentifier");
            if (nameIt == props.end() || idIt == props.end())
            {
                // No more elements at this index.
                if (index == 0)
                {
                    // Components may be absent entirely (FCM-REQ-16 fallback).
                    return;
                }
                break;
            }

            std::string compName;
            try
            {
                compName = std::get<std::string>(nameIt->second);
            }
            catch (const std::exception&)
            {
                continue;
            }

            uint16_t compId = 0;
            try
            {
                compId = extractUint16(idIt->second);
            }
            catch (const std::exception&)
            {
                continue;
            }

            std::string manufacturer = "NVIDIA";
            if (auto it = props.find(prefix + "Manufacturer");
                it != props.end())
            {
                try
                {
                    manufacturer = std::get<std::string>(it->second);
                }
                catch (const std::exception&)
                {}
            }

            Associations assocs;
            for (size_t a = 0;; ++a)
            {
                const std::string aprefix =
                    prefix + "Associations" + std::to_string(a);
                auto fIt = props.find(aprefix + "Forward");
                auto bIt = props.find(aprefix + "Backward");
                auto pIt = props.find(aprefix + "AbsolutePath");
                if (fIt == props.end() || bIt == props.end() ||
                    pIt == props.end())
                {
                    break;
                }
                try
                {
                    assocs.emplace_back(std::get<std::string>(fIt->second),
                                        std::get<std::string>(bIt->second),
                                        std::get<std::string>(pIt->second));
                }
                catch (const std::exception&)
                {}
            }

            emComponents[compId] = {compName, assocs, manufacturer};
            idNameMap[compId] = compName;
        }
    }

    /** @brief Extract a uint16 ComponentIdentifier from a D-Bus variant that
     *         entity-manager may publish as several integral types.
     */
    static uint16_t extractUint16(const pldm::utils::PropertyValue& v)
    {
        if (auto p = std::get_if<uint16_t>(&v))
        {
            return *p;
        }
        if (auto p = std::get_if<uint64_t>(&v))
        {
            return static_cast<uint16_t>(*p);
        }
        if (auto p = std::get_if<uint32_t>(&v))
        {
            return static_cast<uint16_t>(*p);
        }
        if (auto p = std::get_if<int64_t>(&v))
        {
            return static_cast<uint16_t>(*p);
        }
        if (auto p = std::get_if<double>(&v))
        {
            return static_cast<uint16_t>(*p);
        }
        throw std::bad_variant_access();
    }

    void writeDeviceInventoryUuid(eid eid, const UUID& uuid)
    {
        const std::string targetName = getTargetNameForEid(eid);
        if (targetName.empty())
        {
            // No configured_by-resolved name: cannot match a PLDMDeviceInventory
            // entry. Nothing to write (device may simply have no RoT chassis).
            return;
        }

        constexpr auto pldmDeviceInventoryIntf =
            "xyz.openbmc_project.Configuration.PLDMDeviceInventory";
        constexpr auto uuidIntf = "xyz.openbmc_project.Common.UUID";

        pldm::utils::GetSubTreeResponse subtree;
        try
        {
            subtree = pldm::utils::DBusHandler().getSubtree(
                "/xyz/openbmc_project/inventory", 0,
                {pldmDeviceInventoryIntf});
        }
        catch (const std::exception& e)
        {
            error(
                "writeDeviceInventoryUuid: GetSubTree for PLDMDeviceInventory failed for EID {EID}, error - {ERROR}",
                "EID", eid, "ERROR", e);
            return;
        }

        for (const auto& [objPath, serviceMap] : subtree)
        {
            std::string service;
            if (!serviceMap.empty())
            {
                service = serviceMap.begin()->first;
            }
            if (service.empty())
            {
                continue;
            }

            std::string entryTargetName;
            std::string createPath;
            std::string updatePath;
            try
            {
                auto props = pldm::utils::DBusHandler().getDbusPropertiesVariant(
                    service.c_str(), objPath.c_str(), pldmDeviceInventoryIntf);
                if (auto it = props.find("MCTPTargetName"); it != props.end())
                {
                    entryTargetName = std::get<std::string>(it->second);
                }
                if (auto it = props.find("CreateInventoryPath");
                    it != props.end())
                {
                    createPath = std::get<std::string>(it->second);
                }
                if (auto it = props.find("UpdateInventoryPath");
                    it != props.end())
                {
                    updatePath = std::get<std::string>(it->second);
                }
            }
            catch (const std::exception& e)
            {
                error(
                    "writeDeviceInventoryUuid: failed reading PLDMDeviceInventory props at {PATH}, error - {ERROR}",
                    "PATH", objPath, "ERROR", e);
                continue;
            }

            if (entryTargetName != targetName)
            {
                continue;
            }

            // Write the UUID to the EM-created chassis (CreateInventoryPath) and
            // to the optional pre-existing inventory object (UpdateInventoryPath)
            for (const auto& path : {createPath, updatePath})
            {
                if (path.empty())
                {
                    continue;
                }
                try
                {
                    pldm::utils::DBusMapping dbusMapping{path, uuidIntf, "UUID",
                                                         "string"};
                    pldm::utils::DBusHandler().setDbusProperty(
                        dbusMapping, pldm::utils::PropertyValue{uuid});
                    info(
                        "writeDeviceInventoryUuid: wrote UUID for EID {EID} to {PATH}",
                        "EID", eid, "PATH", path);
                }
                catch (const std::exception& e)
                {
                    error(
                        "writeDeviceInventoryUuid: failed to set UUID at {PATH} for EID {EID}, error - {ERROR}",
                        "PATH", path, "EID", eid, "ERROR", e);
                }
            }
            // At most one PLDMDeviceInventory entry matches a target.
            break;
        }
    }
};

} // namespace fw_update

} // namespace pldm
