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
#include "firmware_inventory.hpp"

#include "dbusutil.hpp"

#include <phosphor-logging/lg2.hpp>

#include <format>
#include <iostream>

namespace pldm::fw_update::fw_inventory
{

Entry::Entry(sdbusplus::bus_t& bus, const std::string& objPath,
             const std::string& versionStr, const std::string& swId,
             const std::string& manufacturer) :
    Ifaces(bus, objPath.c_str(), action::defer_emit)
{
    Ifaces::version(versionStr, true);
    Ifaces::purpose(VersionPurpose::Other, true);
    Ifaces::softwareId(swId, true);
    Ifaces::manufacturer(manufacturer, true);
    Ifaces::emit_object_added();
}

void Entry::createUpdateableAssociation(const std::string& swObjPath)
{
    auto assocs = associations();
    assocs.emplace_back(
        std::make_tuple(upFwdAssociation, upRevAssociation, swObjPath));
    associations(assocs);
}

void Entry::createAssociation(const std::string fwdAssociation,
                              const std::string revAssociation,
                              const std::string& objPath)
{
    auto assocs = associations();
    assocs.emplace_back(
        std::make_tuple(fwdAssociation, revAssociation, objPath));

    associations(assocs);
}

void Entry::setVersion(const std::string& versionStr)
{
    Ifaces::version(versionStr, false);
}

Manager::Manager(sdbusplus::bus_t& bus,
                 const FirmwareInventoryInfo& firmwareInventoryInfo,
                 const ComponentInfoMap& componentInfoMap,
                 const ComponentNameMap& componentNameMap) :
    bus(bus), firmwareInventoryInfo(firmwareInventoryInfo),
    componentInfoMap(componentInfoMap), componentNameMap(componentNameMap)
{}

void Manager::createEntry(pldm::eid eid, const pldm::UUID& uuid,
                          dbus::MctpInterfaces& mctpInterfaces)
{
    if (!componentInfoMap.contains(eid))
    {
        lg2::info(
            "Skipping firmware inventory creation: no component info available, EID={EID}",
            "EID", eid);
        return;
    }

    auto compInfoSearch = componentInfoMap.find(eid);
    FirmwareInfo fwInfoSearch;

    // Prefer per-component metadata sourced from the entity-manager
    // Configuration.PLDMFirmwareDevice.Components array (Name, Associations →
    // RelatedItem, optional Manufacturer). Match by ComponentIdentifier against
    // the GetFirmwareParameters response; unmatched components fall through to
    // the fallback names in componentNameMap below.
    if (auto emIt = emComponentObjectMap.find(eid);
        emIt != emComponentObjectMap.end() && !emIt->second.empty())
    {
        const auto& emComponents = emIt->second;
        for (const auto& [compKey, compInfo] : compInfoSearch->second)
        {
            auto compObjIt = emComponents.find(compKey.second);
            std::string compObjName;
            Associations compObjAssocs;
            std::string compObjManufacturer = "NVIDIA";
            bool compUpdateOnly = false;
            if (compObjIt != emComponents.end())
            {
                std::tie(compObjName, compObjAssocs, compObjManufacturer,
                         compUpdateOnly) = compObjIt->second;
            }
            else if (componentNameMap.contains(eid) &&
                     componentNameMap.at(eid).contains(compKey.second))
            {
                // Component reported by the device but not declared in the EM
                // Components array — use the fallback name.
                compObjName = componentNameMap.at(eid).at(compKey.second);
            }
            else
            {
                continue;
            }

            std::string objPath = swBasePath + "/" + compObjName;
            auto swId = std::format("0x{:04X}", compKey.second);

            if (compUpdateOnly)
            {
                // The Software.Version object at objPath is owned by another
                // service (e.g. BMC firmware, component 16 -> BMC.Inventory).
                // Only stamp SoftwareId on it; do NOT create a competing
                // pldm-owned object (which would carry Purpose=Other and shadow
                // the real inventory entry).
                updateSwId(objPath, swId);
                lg2::info(
                    "Updated software ID for existing D-Bus object (EM config, update-only): path={PATH}, component_id={ID}",
                    "PATH", objPath, "ID", swId);
                continue;
            }

            auto entry = std::make_unique<Entry>(
                bus, objPath, std::get<CompVersion>(compInfo), swId,
                compObjManufacturer);
            entry->createUpdateableAssociation(swBasePath);

            lg2::info(
                "Created software D-Bus object (EM config): path={PATH}, component_id={ID}, version={VERSION}",
                "PATH", objPath, "ID", swId, "VERSION",
                std::get<CompVersion>(compInfo));

            for (const auto& assoc : compObjAssocs)
            {
                entry->createAssociation(std::get<0>(assoc), std::get<1>(assoc),
                                         std::get<2>(assoc));
            }

            firmwareInventoryMap.emplace(std::make_pair(eid, compKey.second),
                                         std::move(entry));
        }
        return;
    }

    if (mctpInterfaces.find(uuid) == mctpInterfaces.end())
    {
        lg2::info(
            "Skipping firmware inventory creation: UUID not found in mctpInterfaces, EID={EID}",
            "EID", eid);
        return;
    }

    // Check if we have a config JSON match
    if (firmwareInventoryInfo.matchInventoryEntry(mctpInterfaces[uuid],
                                                  fwInfoSearch))
    {
        for (const auto& [compKey, compInfo] : compInfoSearch->second)
        {
            if ((std::get<0>(fwInfoSearch)).contains(compKey.second))
            {
                // Inventory object is preserved across endpoint removal;
                // refresh it in place since re-registering the path throws
                // (-EEXIST).
                if (auto existing = firmwareInventoryMap.find(
                        std::make_pair(eid, compKey.second));
                    existing != firmwareInventoryMap.end())
                {
                    existing->second->setVersion(
                        std::get<CompVersion>(compInfo));

                    lg2::info(
                        "Refreshed software D-Bus object: component_id={ID}, version={VERSION}",
                        "ID", std::format("0x{:04X}", compKey.second),
                        "VERSION", std::get<CompVersion>(compInfo));
                }
                else
                {
                    auto componentObject =
                        (std::get<0>(fwInfoSearch)).find(compKey.second);
                    const auto& [compObjName, compObjAssocs,
                                 compObjManufacturer,
                                 compUpdateOnly] = componentObject->second;
                    std::string objPath = swBasePath + "/" + compObjName;
                    auto swId = std::format("0x{:04X}", compKey.second);

                    if (compUpdateOnly)
                    {
                        // Owned by another service: stamp SoftwareId only, no
                        // competing object.
                        updateSwId(objPath, swId);
                        lg2::info(
                            "Updated software ID for existing D-Bus object (update-only): path={PATH}, component_id={ID}",
                            "PATH", objPath, "ID", swId);
                        continue;
                    }

                    auto entry = std::make_unique<Entry>(
                        bus, objPath, std::get<CompVersion>(compInfo), swId,
                        compObjManufacturer);
                    entry->createUpdateableAssociation(swBasePath);

                    lg2::info(
                        "Created software D-Bus object: path={PATH}, component_id={ID}, version={VERSION}",
                        "PATH", objPath, "ID", swId, "VERSION",
                        std::get<CompVersion>(compInfo));

                    const auto& assocs = compObjAssocs;

                    for (auto& assoc : assocs)
                    {
                        std::string fwdAssociation = std::get<0>(assoc);
                        std::string revAssociation = std::get<1>(assoc);
                        std::string objectPathAssociation = std::get<2>(assoc);

                        entry->createAssociation(fwdAssociation, revAssociation,
                                                 objectPathAssociation);
                    }

                    firmwareInventoryMap.emplace(
                        std::make_pair(eid, compKey.second), std::move(entry));
                }
            }
            if ((std::get<1>(fwInfoSearch)).contains(compKey.second))
            {
                auto componentName =
                    (std::get<1>(fwInfoSearch)).find(compKey.second);
                std::string objPath = swBasePath + "/" + componentName->second;
                auto swId = std::format("0x{:04X}", compKey.second);
                updateSwId(objPath, swId);

                lg2::info(
                    "Updated software ID for existing D-Bus object: path={PATH}, component_id={ID}",
                    "PATH", objPath, "ID", swId);
            }
        }
    }
    else
    {
        // No config JSON - use generated names from componentNameMap
        // (populated by createInventory() before this call).
        const auto& compIdNameMap = componentNameMap.at(eid);
        for (const auto& [compKey, compInfo] : compInfoSearch->second)
        {
            // Refresh preserved objects in place; re-registering throws
            // (-EEXIST). See above.
            if (auto existing = firmwareInventoryMap.find(
                    std::make_pair(eid, compKey.second));
                existing != firmwareInventoryMap.end())
            {
                existing->second->setVersion(std::get<1>(compInfo));
                continue;
            }

            std::string objPath =
                swBasePath + "/" + compIdNameMap.at(compKey.second);

            auto swId = std::format("0x{:04X}", compKey.second);
            auto entry = std::make_unique<Entry>(
                bus, objPath, std::get<1>(compInfo), swId, "NVIDIA");
            entry->createUpdateableAssociation(swBasePath);

            lg2::info(
                "Created software D-Bus object (no config): path={PATH}, component_id={ID}, version={VERSION}",
                "PATH", objPath, "ID", swId, "VERSION", std::get<1>(compInfo));

            firmwareInventoryMap.emplace(std::make_pair(eid, compKey.second),
                                         std::move(entry));
        }
    }
}

void Manager::updateEntry(pldm::eid eid,
                          [[maybe_unused]] const pldm::UUID& uuid,
                          [[maybe_unused]] dbus::MctpInterfaces& mctpInterfaces)
{
    if (!componentInfoMap.contains(eid))
    {
        lg2::info(
            "Skipping firmware inventory update: EID not found in component info map, EID={EID}",
            "EID", eid);
        return;
    }

    auto compInfoSearch = componentInfoMap.find(eid);
    bool anyEntryUpdated = false;

    for (const auto& [compKey, compInfo] : compInfoSearch->second)
    {
        auto key = std::make_pair(eid, compKey.second);
        if (auto inventoryEntry = firmwareInventoryMap.find(key);
            inventoryEntry != firmwareInventoryMap.end())
        {
            inventoryEntry->second->setVersion(std::get<1>(compInfo));

            lg2::info(
                "Updated firmware inventory: EID={EID}, component_id={ID}, version={VERSION}",
                "EID", eid, "ID", compKey.second, "VERSION",
                std::get<1>(compInfo));
            anyEntryUpdated = true;
        }
    }

    if (!anyEntryUpdated)
    {
        lg2::info(
            "No firmware inventory entries found for EID={EID} during refresh, skipping update",
            "EID", eid);
    }
}

void Manager::updateFWVersion(pldm::eid eid)
{
    if (auto compInfoSearch = componentInfoMap.find(eid);
        compInfoSearch != componentInfoMap.end())
    {
        for (const auto& [compKey, compInfo] : compInfoSearch->second)
        {
            auto key = std::make_pair(eid, compKey.second);
            if (auto inventoryEntry = firmwareInventoryMap.find(key);
                inventoryEntry != firmwareInventoryMap.end())
            {
                inventoryEntry->second->setVersion(std::get<1>(compInfo));
            }
        }
    }
    else
    {
        lg2::info(
            "Skipping firmware version update: EID not found in component info map, EID={EID}",
            "EID", eid);
    }
}

void Manager::updateSwId(const dbus::ObjectPath& objPath,
                         const std::string& compId)
{
    if (objPath.empty())
    {
        return;
    }

    setDBusPropertyAsync(objPath, "xyz.openbmc_project.Software.Version",
                         "SoftwareId", compId);

    compIdentifierLookup.emplace(objPath, compId);
    updateFwMatch.emplace_back(
        bus,
        sdbusplus::bus::match::rules::interfacesAdded() +
            sdbusplus::bus::match::rules::argNpath(0, objPath),
        std::bind_front(&Manager::updateSwIdOnSignal, this));
}

void Manager::updateSwIdOnSignal(sdbusplus::message::message& msg)
{
    sdbusplus::object_path objPath;
    dbus::InterfaceMap interfaces;
    msg.read(objPath, interfaces);

    if (!interfaces.contains("xyz.openbmc_project.Software.Version"))
    {
        return;
    }

    if (compIdentifierLookup.contains(objPath))
    {
        auto search = compIdentifierLookup.find(objPath);
        const auto& compId = search->second;
        const std::string& pathStr = objPath;

        setDBusPropertyAsync(pathStr, "xyz.openbmc_project.Software.Version",
                             "SoftwareId", compId);
    }
}

} // namespace pldm::fw_update::fw_inventory

namespace pldm::fw_update
{

FirmwareInventory::FirmwareInventory(
    SoftwareIdentifier /*softwareIdentifier*/, const std::string& softwarePath,
    const std::string& softwareVersion, const std::string& associatedEndpoint,
    SoftwareVersionPurpose purpose) :
    softwarePath(softwarePath),
    association(this->bus, this->softwarePath.c_str()),
    version(this->bus, this->softwarePath.c_str(),
            SoftwareVersion::action::defer_emit)
{
    this->association.associations(
        {{"running", "ran_on", associatedEndpoint.c_str()}});
    this->version.version(softwareVersion.c_str());
    this->version.purpose(purpose);
    this->version.emit_added();
}

} // namespace pldm::fw_update
