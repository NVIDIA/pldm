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

#include <algorithm>
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

    /** @brief Create firmware inventory and write the device UUID + EC-SKU/AP-SKU
     *         to the entity-manager-owned RoT chassis (PLDM FW Update Config
     *         Migration, DGXOPENBMC-25121).
     *
     *  RoT chassis objects are no longer created by pldmd; entity-manager owns
     *  them via Configuration.PLDMDeviceInventory.CreateInventoryPath. pldmd
     *  writes the dynamic Common.UUID + SKU decorator to that EM-created object
     *  (and the AP-SKU to the optional UpdateInventoryPath if declared). This is
     *  the inventory-creation path, so the UUID is written here; updateInventory
     *  (the refresh path) deliberately does not re-write it — see
     *  writeDeviceInventoryIdentity().
     *
     *  @param[in] eid - MCTP endpoint
     *  @param[in] uuid - MCTP UUID
     */
    void createInventory(eid eid, UUID uuid,
                         dbus::MctpInterfaces& mctpInterfaces)
    {
        writeDeviceInventoryIdentity(eid, uuid, /*writeUuid=*/true);
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
     *         firmware parameter information, and re-write the device
     *         EC-SKU/AP-SKU to the entity-manager-owned RoT chassis.
     *
     *  @param[in] eid - MCTP endpoint
     *  @param[in] uuid - MCTP UUID (used for the firmware inventory entry)
     *  @param[in] mctpInterfaces - MCTP interface information
     */
    void updateInventory(eid eid, UUID uuid,
                         dbus::MctpInterfaces& mctpInterfaces)
    {
        writeDeviceInventoryIdentity(eid, uuid, /*writeUuid=*/false);
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

    /** @brief Component information to create message registries */
    ComponentNameMap componentNameMap;

    /** @brief PLDM firmware inventory/discovery manager */
    InventoryManager inventoryMgr;

    /** @brief PLDM firmware update manager */
    UpdateManager updateManager;

    /** @brief Firmware inventory D-Bus object manager */
    fw_inventory::Manager fwInventoryManager;

    /** @brief Write the device identity (UUID + EC-SKU/AP-SKU) to the
     *         entity-manager-owned RoT chassis.
     *
     *  PLDM FW Update Config Migration (DGXOPENBMC-25121), SADD §3.2.2/§3.3.2
     *  step 3e. pldmd creates no inventory objects: the RoT chassis is created
     *  by entity-manager from Configuration.PLDMDeviceInventory. This helper
     *  resolves the device's PLDMDeviceInventory config entry by string-match
     *  on MCTPTargetName, then writes the device identity onto the EM-owned
     *  objects: Common.UUID (only when @p writeUuid) + EC-SKU to
     *  CreateInventoryPath (the RoT chassis) and AP-SKU to the optional
     *  UpdateInventoryPath. Writes are applied when-ready (immediate Set plus an
     *  InterfacesAdded retry).
     *
     *  @p writeUuid is true only on inventory creation (createInventory). On a
     *  refresh (updateInventory) it is false: the UUID is static, and the
     *  blocking per-write ObjectMapper lookup it incurred during the periodic
     *  firmware refresh serialised ~1s per object, stalling the event loop long
     *  enough to drop in-flight PLDM responses.
     *
     *  @param[in] eid - MCTP endpoint
     *  @param[in] uuid - device UUID obtained over MCTP/PLDM
     *  @param[in] writeUuid - write Common.UUID (creation path only)
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
     *  Identity is configured_by-only (FCM-REQ-11) — the EID is never used as an
     *  identity key. If configured_by is absent the name is empty and the
     *  endpoint is treated as not (yet) resolvable.
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
            unpackComponents(service, objPath, pldmFwDeviceIntf, emComponents,
                             idNameMap);

            fwInventoryManager.setEmComponentObjects(eid, emComponents);
            if (!idNameMap.empty())
            {
                componentNameMap[eid] = std::move(idNameMap);
            }
            return true;
        }
        return false;
    }

    /** @brief Read the per-component definitions of a
     *         Configuration.PLDMFirmwareDevice entry.
     *
     *  entity-manager publishes each element of the nested Components array as a
     *  separate CHILD interface "<baseIntf>.Components<index>" on the same
     *  object path — NOT as flattened "Components<index><field>" properties on
     *  the parent interface. Each child interface carries Name,
     *  ComponentIdentifier and an optional Manufacturer. Per-component
     *  associations are published as three index-aligned string arrays
     *  (AssociationForward/Backward/Endpoint) on the same child interface —
     *  flat arrays survive PlatformExposes flattening where a nested object
     *  array would not — and are zipped here into each component's association
     *  list so firmware inventory can publish the inventory/activation
     *  associations the Redfish layer needs.
     *
     *  @param[in]  service      - D-Bus service owning the object (entity-manager)
     *  @param[in]  objPath      - PLDMFirmwareDevice object path
     *  @param[in]  baseIntf     - Configuration.PLDMFirmwareDevice interface name
     *  @param[out] emComponents - id → {Name, Associations, Manufacturer}
     *  @param[out] idNameMap    - id → Name (for target filtering / fallback)
     */
    static void unpackComponents(const std::string& service,
                                 const std::string& objPath,
                                 const std::string& baseIntf,
                                 CreateComponentIdNameMap& emComponents,
                                 ComponentIdNameMap& idNameMap)
    {
        // Probe contiguous child interfaces Components0, Components1, ... until
        // one is absent (GetAll throws), mirroring how EM numbers them.
        for (size_t index = 0;; ++index)
        {
            const std::string compIntf =
                baseIntf + ".Components" + std::to_string(index);

            pldm::utils::PropertyMap cprops;
            try
            {
                cprops = pldm::utils::DBusHandler().getDbusPropertiesVariant(
                    service.c_str(), objPath.c_str(), compIntf.c_str());
            }
            catch (const std::exception&)
            {
                // No Components<index> child interface — end of the array.
                break;
            }
            if (cprops.empty())
            {
                break;
            }

            auto nameIt = cprops.find("Name");
            auto idIt = cprops.find("ComponentIdentifier");
            if (nameIt == cprops.end() || idIt == cprops.end())
            {
                continue;
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
            if (auto it = cprops.find("Manufacturer"); it != cprops.end())
            {
                try
                {
                    manufacturer = std::get<std::string>(it->second);
                }
                catch (const std::exception&)
                {}
            }

            // Per-component associations are published by entity-manager as
            // three index-aligned "as" arrays (AssociationForward/Backward/
            // Endpoint) — flat arrays survive PlatformExposes flattening onto
            // the Components<N> child interface, unlike a nested object array.
            // Zip them into the (forward, reverse, endpoint) tuples firmware
            // inventory copies onto the Software.Version Association.Definitions.
            auto readStrList = [&cprops](const std::string& prop) {
                std::vector<std::string> v;
                if (auto it = cprops.find(prop); it != cprops.end())
                {
                    try
                    {
                        v = std::get<std::vector<std::string>>(it->second);
                    }
                    catch (const std::exception&)
                    {}
                }
                return v;
            };
            const auto fwd = readStrList("AssociationForward");
            const auto bwd = readStrList("AssociationBackward");
            const auto endp = readStrList("AssociationEndpoint");
            Associations compAssocs;
            const size_t nAssoc =
                std::min({fwd.size(), bwd.size(), endp.size()});
            for (size_t i = 0; i < nAssoc; ++i)
            {
                compAssocs.emplace_back(fwd[i], bwd[i], endp[i]);
            }

            // UpdateOnly: the component's Software.Version object is owned by
            // another service (e.g. BMC firmware, component 16, owned by
            // BMC.Inventory). firmware inventory must only stamp SoftwareId on
            // it, never create a competing Purpose=Other object.
            bool updateOnly = false;
            if (auto it = cprops.find("UpdateOnly"); it != cprops.end())
            {
                try
                {
                    updateOnly = std::get<bool>(it->second);
                }
                catch (const std::exception&)
                {}
            }

            emComponents[compId] = {compName, std::move(compAssocs),
                                    manufacturer, updateOnly};
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

    void writeDeviceInventoryIdentity(eid eid, const UUID& uuid, bool writeUuid)
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
        constexpr auto skuIntf =
            "xyz.openbmc_project.Inventory.Decorator.SKU";

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
                "writeDeviceInventoryIdentity: GetSubTree for PLDMDeviceInventory failed for EID {EID}, error - {ERROR}",
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
                    "writeDeviceInventoryIdentity: failed reading PLDMDeviceInventory props at {PATH}, error - {ERROR}",
                    "PATH", objPath, "ERROR", e);
                continue;
            }

            if (entryTargetName != targetName)
            {
                continue;
            }

            // Extract EC-SKU / AP-SKU from the device's vendor-defined PLDM
            // descriptors (origin/develop parity).
            std::string ecsku;
            std::string apsku;
            if (auto descIt = descriptorMap.find(eid);
                descIt != descriptorMap.end())
            {
                std::tie(ecsku, apsku) = extractSkus(descIt->second);
            }

            // Write the device identity onto the EM-owned objects, retrying
            // when each object/interface appears (never depending on NSM):
            //   - RoT chassis (CreateInventoryPath): Common.UUID (on inventory
            //     creation only) + EC-SKU
            //   - update target (UpdateInventoryPath): AP-SKU
            // The UUID is written once, when the inventory is created
            // (writeUuid), never on a refresh: the blocking per-write
            // ObjectMapper lookup during the periodic firmware refresh
            // serialised ~1s per object and stalled the event loop long enough
            // to drop in-flight PLDM responses.
            if (!createPath.empty())
            {
                if (writeUuid)
                {
                    writeInventoryPropWhenReady(createPath, uuidIntf, "UUID",
                                                uuid);
                }
                writeInventoryPropWhenReady(createPath, skuIntf, "SKU", ecsku);
            }
            if (!updatePath.empty())
            {
                writeInventoryPropWhenReady(updatePath, skuIntf, "SKU", apsku);
            }
            // At most one PLDMDeviceInventory entry matches a target.
            break;
        }
    }

    /** @brief A deferred identity write onto an inventory object. */
    struct PendingInventoryWrite
    {
        std::string interface;
        std::string property;
        std::string value;
    };

    /** @brief Parse EC-SKU and AP-SKU from a device's PLDM descriptors
     *         (vendor-defined "ECSKU"/"APSKU", 4 bytes each, formatted
     *         0xAABBCCDD). Ported from origin/develop device_inventory.
     *
     *  @param[in] descriptors - the device's PLDM descriptors
     *  @return {ecsku, apsku}; either may be empty if absent
     */
    static std::pair<std::string, std::string> extractSkus(
        const Descriptors& descriptors)
    {
        std::string ecsku;
        std::string apsku;
        for (const auto& [descType, descValue] : descriptors)
        {
            if (descType != PLDM_FWUP_VENDOR_DEFINED)
            {
                continue;
            }
            const auto& [title, data] =
                std::get<VendorDefinedDescriptorInfo>(descValue);
            if (data.size() != 4)
            {
                continue;
            }
            if (title == "ECSKU")
            {
                ecsku = std::format("0x{:02X}{:02X}{:02X}{:02X}", data[0],
                                    data[1], data[2], data[3]);
            }
            else if (title == "APSKU")
            {
                apsku = std::format("0x{:02X}{:02X}{:02X}{:02X}", data[0],
                                    data[1], data[2], data[3]);
            }
        }
        return {ecsku, apsku};
    }

    /** @brief Write a string property onto an entity-manager-owned inventory
     *         object, retrying when the object/interface appears.
     *
     *  pldmd no longer owns these objects (EM creates the RoT chassis), so a
     *  one-shot Set races EM/NSM object creation. Mirroring origin/develop's
     *  updateSKUOnMatch, this tries the Set immediately (covers the already-
     *  present case) and arms an InterfacesAdded watch on the path so the write
     *  lands when the object appears with the interface — making pldmd the sole
     *  authority for the value and never depending on NSM to populate it.
     *
     *  @param[in] objPath   - target inventory object path
     *  @param[in] interface - interface hosting the property
     *  @param[in] property  - property name
     *  @param[in] value     - value to write (no-op if empty)
     */
    void writeInventoryPropWhenReady(const std::string& objPath,
                                     const std::string& interface,
                                     const std::string& property,
                                     const std::string& value)
    {
        if (objPath.empty() || value.empty())
        {
            return;
        }
        // Record (or refresh) the pending write so onInventoryObjectAdded can
        // (re)apply it; dedupe so repeated create/update calls don't accumulate.
        auto& writes = pendingInventoryWrites[objPath];
        bool updated = false;
        for (auto& pending : writes)
        {
            if (pending.interface == interface && pending.property == property)
            {
                pending.value = value;
                updated = true;
                break;
            }
        }
        if (!updated)
        {
            writes.push_back({interface, property, value});
        }
        trySetInventoryProp(objPath, interface, property, value);
        if (!inventoryAddedMatches.contains(objPath))
        {
            inventoryAddedMatches.try_emplace(
                objPath, pldm::utils::DBusHandler::getBus(),
                sdbusplus::bus::match::rules::interfacesAdded() +
                    sdbusplus::bus::match::rules::argNpath(0, objPath),
                std::bind_front(&Manager::onInventoryObjectAdded, this));
        }
    }

    /** @brief Best-effort synchronous Set; failure before the object exists is
     *         expected and retried by onInventoryObjectAdded. */
    void trySetInventoryProp(const std::string& objPath,
                             const std::string& interface,
                             const std::string& property,
                             const std::string& value)
    {
        try
        {
            pldm::utils::DBusMapping dbusMapping{objPath, interface, property,
                                                 "string"};
            pldm::utils::DBusHandler().setDbusProperty(
                dbusMapping, pldm::utils::PropertyValue{value});
            info("Wrote {PROP} onto inventory object {PATH}", "PROP", property,
                 "PATH", objPath);
        }
        catch (const std::exception& e)
        {
            debug("Deferring {PROP} on {PATH} until ready: {ERROR}", "PROP",
                  property, "PATH", objPath, "ERROR", e);
        }
    }

    /** @brief InterfacesAdded handler: (re)apply pending writes for the path
     *         whose interface is now present. Ported from updateSKUOnMatch. */
    void onInventoryObjectAdded(sdbusplus::message_t& msg)
    {
        sdbusplus::message::object_path objPath;
        dbus::InterfaceMap interfaces;
        try
        {
            msg.read(objPath, interfaces);
        }
        catch (const std::exception&)
        {
            return;
        }
        auto it = pendingInventoryWrites.find(objPath.str);
        if (it == pendingInventoryWrites.end())
        {
            return;
        }
        for (const auto& write : it->second)
        {
            if (interfaces.contains(write.interface))
            {
                trySetInventoryProp(objPath.str, write.interface,
                                    write.property, write.value);
            }
        }
    }

    /** @brief Pending identity writes per inventory object path, applied when
     *         the object appears. */
    std::unordered_map<std::string, std::vector<PendingInventoryWrite>>
        pendingInventoryWrites;

    /** @brief InterfacesAdded watches per inventory object path. */
    std::unordered_map<std::string, sdbusplus::bus::match_t>
        inventoryAddedMatches;
};

} // namespace fw_update

} // namespace pldm
