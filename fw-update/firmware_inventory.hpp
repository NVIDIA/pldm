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

#include "common/types.hpp"
#include "common/utils.hpp"

#include <sdbusplus/server.hpp>
#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/Asset/server.hpp>
#include <xyz/openbmc_project/Software/Version/server.hpp>

class FirmwareInventoryTest;

namespace pldm::fw_update::fw_inventory
{

using VersionIntf = sdbusplus::xyz::openbmc_project::Software::server::Version;
using AssociationIntf =
    sdbusplus::xyz::openbmc_project::Association::server::Definitions;
using DecoratorAssetIntf =
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::Asset;

using Ifaces = sdbusplus::server::object::object<VersionIntf, AssociationIntf,
                                                 DecoratorAssetIntf>;

/** @class Entry
 *
 *  Implementation of firmware inventory D-Bus object implementing the D-Bus
 *  interfaces.
 *
 *  a) xyz.openbmc_project.Software.Version
 *  b) xyz.openbmc_project.Association.Definitions
 *  c) xyz.openbmc_project.Inventory.Decorator.Asset
 */
class Entry : public Ifaces
{
  public:
    Entry() = delete;
    ~Entry() = default;
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
    Entry(Entry&&) = delete;
    Entry& operator=(Entry&&) = delete;

    const std::string upFwdAssociation = "software_version";
    const std::string upRevAssociation = "updateable";

    /** @brief Constructor
     *
     *  @param[in] bus  - Bus to attach to
     *  @param[in] objPath - D-Bus object path
     *  @param[in] version - Version string
     *  @param[in] swId - Software ID
     *  @param[in] manufacturer - Manufacturer string
     */
    explicit Entry(sdbusplus::bus_t& bus, const std::string& objPath,
                   const std::string& versionStr, const std::string& swId,
                   const std::string& manufacturer);

    /** @brief Create association {"software_version", "updateable"} between
     * software version object and "/xyz/openbmc_project/software"
     *
     *  @param[in] swObjPath - "/xyz/openbmc_project/software"
     */
    void createUpdateableAssociation(const std::string& swObjPath);

    /** @brief Create association defined in parameters
     *
     *  @param[in] fwdAssociation - Association forward
     *  @param[in] revAssociation - Association reverse
     *  @param[in] objPath - D-Bus object path
     */
    void createAssociation(const std::string fwdAssociation,
                           const std::string revAssociation,
                           const std::string& objPath);

    /** @brief Update Active Firmware version
     *
     *  @param[in] version - Version string
     */
    void setVersion(const std::string& versionStr);
};

/** @class Manager
 *
 *  Object manager for firmware inventory objects
 */
class Manager
{
  public:
    Manager() = delete;
    ~Manager() = default;
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;
    Manager(Manager&&) = delete;
    Manager& operator=(Manager&&) = delete;

    /** @brief Constructor
     *
     *  @param[in] bus  - Bus to attach to
     *  @param[in] firmwareInventoryInfo - Config info for firmware inventory
     *  @param[in] componentInfoMap - Component information of managed FDs
     *  @param[in] componentNameMap - Component name map for D-Bus paths
     */
    explicit Manager(sdbusplus::bus_t& bus,
                     const FirmwareInventoryInfo& firmwareInventoryInfo,
                     const ComponentInfoMap& componentInfoMap,
                     const ComponentNameMap& componentNameMap);

    /** @brief Create firmware inventory object
     *
     *  @param[in] eid - MCTP endpointID
     *  @param[in] uuid - MCTP UUID
     *  @param[in] deviceObjPath - Object path of the device inventory object
     */
    void createEntry(pldm::eid eid, const pldm::UUID& uuid,
                     dbus::MctpInterfaces& mctpInterfaces);

    /** @brief Update existing firmware inventory objects
     *
     *  @param[in] eid - MCTP endpointID
     *  @param[in] uuid - MCTP UUID
     *  @param[in] mctpInterfaces - MCTP interface information
     */
    void updateEntry(pldm::eid eid, const pldm::UUID& uuid,
                     dbus::MctpInterfaces& mctpInterfaces);

    /** @brief Update firmware version
     *
     *  @param[in] eid - MCTP endpointID
     */
    void updateFWVersion(pldm::eid eid);

    /** @brief Provide per-component metadata sourced from the entity-manager
     *         Configuration.PLDMFirmwareDevice.Components array for an endpoint.
     *
     *  PLDM FW Update Config Migration (DGXOPENBMC-25121), SADD §3.3.2 step
     *  3b/3d. When present for an EID, createEntry() prefers this EM-sourced map
     *  (component Name, Associations → RelatedItem, optional Manufacturer) over
     *  the legacy JSON-parsed firmwareInventoryInfo. Match is by
     *  ComponentIdentifier against the PLDM GetFirmwareParameters response.
     *
     *  @param[in] eid - MCTP endpointID
     *  @param[in] components - ComponentIdentifier → {Name, Associations,
     *                          Manufacturer}
     */
    void setEmComponentObjects(pldm::eid eid,
                               const CreateComponentIdNameMap& components)
    {
        emComponentObjectMap[eid] = components;
    }

    const std::string swBasePath = "/xyz/openbmc_project/software";

  private:
    sdbusplus::bus_t& bus;

    /** @brief Config info for firmware inventory */
    const FirmwareInventoryInfo& firmwareInventoryInfo;

    /** @brief Component information needed for the update of the managed FDs */
    const ComponentInfoMap& componentInfoMap;

    /** @brief Component name map for D-Bus object paths */
    const ComponentNameMap& componentNameMap;

    /** @brief Per-endpoint component metadata sourced from entity-manager
     *         Configuration.PLDMFirmwareDevice.Components (DGXOPENBMC-25121).
     */
    std::unordered_map<eid, CreateComponentIdNameMap> emComponentObjectMap;

    /** @brief Map to store firmware inventory objects */
    std::map<std::pair<eid, CompIdentifier>, std::unique_ptr<Entry>>
        firmwareInventoryMap;

    /** @brief D-Bus signal match for objects to be updated with SoftwareID*/
    std::vector<sdbusplus::bus::match_t> updateFwMatch;

    /** @brief Lookup table to find the SoftwareID for the input D-Bus object
     */
    std::unordered_map<dbus::ObjectPath, std::string> compIdentifierLookup;

    /** @brief Update SoftwareID on the D-Bus object and register for
     *         InterfaceAdded signal to update if the D-Bus object is created
     *         again.
     *
     *  @param[in] objPath - D-Bus object path
     *  @param[in] compId - Component Identifier
     */
    void updateSwId(const dbus::ObjectPath& objPath, const std::string& compId);

    /** @brief Update SoftwareID on the D-Bus object
     *
     *  @param[in] msg - D-Bus message
     */
    void updateSwIdOnSignal(sdbusplus::message::message& msg);
};

} // namespace pldm::fw_update::fw_inventory

namespace pldm::fw_update
{

using SoftwareVersion = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Software::server::Version>;
using SoftwareAssociationDefinitions = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Association::server::Definitions>;
using SoftwareVersionPurpose = SoftwareVersion::VersionPurpose;

class FirmwareInventory
{
  public:
    friend class ::FirmwareInventoryTest;
    FirmwareInventory() = delete;
    FirmwareInventory(const FirmwareInventory&) = delete;
    FirmwareInventory(FirmwareInventory&&) = delete;
    FirmwareInventory& operator=(const FirmwareInventory&) = delete;
    FirmwareInventory& operator=(FirmwareInventory&&) = delete;
    ~FirmwareInventory() = default;

    /**
     * @brief Constructor
     * @param[in] softwareIdentifier - Software identifier containing EID and
     *                                 component identifier
     * @param[in] softwarePath - D-Bus object path for the firmware inventory
     * entry
     * @param[in] softwareVersion - Active version of the firmware
     * @param[in] associatedEndpoint - D-Bus object path of the endpoint
     * associated with the firmware
     * @param[in] purpose - Purpose of the software version, default is Unknown
     */
    explicit FirmwareInventory(
        SoftwareIdentifier softwareIdentifier, const std::string& softwarePath,
        const std::string& softwareVersion,
        const std::string& associatedEndpoint,
        SoftwareVersionPurpose purpose = SoftwareVersionPurpose::Unknown);

  private:
    sdbusplus::bus_t& bus = utils::DBusHandler::getBus();
    std::string softwarePath;
    SoftwareAssociationDefinitions association;
    SoftwareVersion version;
};

} // namespace pldm::fw_update
