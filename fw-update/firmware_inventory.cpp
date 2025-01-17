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

#include <fmt/format.h>

#include <phosphor-logging/lg2.hpp>

#include <iostream>
#include <thread>

namespace pldm::fw_update::fw_inventory
{

Entry::Entry(sdbusplus::bus::bus& bus, const std::string& objPath,
             const std::string& versionStr, const std::string& swId) :
    Ifaces(bus, objPath.c_str(), action::defer_emit)
{
    Ifaces::version(versionStr, true);
    Ifaces::purpose(VersionPurpose::Other, true);
    Ifaces::softwareId(swId, true);
    Ifaces::manufacturer("NVIDIA", true);
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

Manager::Manager(sdbusplus::bus::bus& bus,
                 const FirmwareInventoryInfo& firmwareInventoryInfo,
                 const ComponentInfoMap& componentInfoMap,
                 utils::DBusHandlerInterface* dBusHandlerIntf) :
    bus(bus),
    firmwareInventoryInfo(firmwareInventoryInfo),
    componentInfoMap(componentInfoMap), dBusHandlerIntf(dBusHandlerIntf)
{}

void Manager::createEntry(pldm::EID eid, const pldm::UUID& uuid,
                          dbus::MctpInterfaces& mctpInterfaces)
{
    FirmwareInfo fwInfoSearch;
    if (mctpInterfaces.find(uuid) != mctpInterfaces.end() &&
        firmwareInventoryInfo.matchInventoryEntry(mctpInterfaces[uuid],
                                                  fwInfoSearch) &&
        componentInfoMap.contains(eid))
    {
        auto compInfoSearch = componentInfoMap.find(eid);

        for (const auto& [compKey, compInfo] : compInfoSearch->second)
        {
            if ((std::get<0>(fwInfoSearch)).contains(compKey.second))
            {
                auto componentObject =
                    (std::get<0>(fwInfoSearch)).find(compKey.second);
                std::string objPath =
                    swBasePath + "/" +
                    std::get<ComponentName>(componentObject->second);

                auto swId = fmt::format("0x{:04X}", compKey.second);
                auto entry = std::make_unique<Entry>(
                    bus, objPath, std::get<1>(compInfo), swId);
                entry->createUpdateableAssociation(swBasePath);

                const auto& assocs =
                    std::get<Associations>(componentObject->second);

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
            if ((std::get<1>(fwInfoSearch)).contains(compKey.second))
            {
                auto componentName =
                    (std::get<1>(fwInfoSearch)).find(compKey.second);
                std::string objPath = swBasePath + "/" + componentName->second;
                auto swId = fmt::format("0x{:04X}", compKey.second);
                updateSwId(objPath, swId);
            }
        }
    }
    else
    {
        // Skip if UUID is not present or firmware inventory information from
        // firmware update config JSON is empty
    }
}

void Manager::updateFWVersion(pldm::EID eid)
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
}

void Manager::updateSwId(const dbus::ObjectPath& objPath,
                         const std::string& compId)
{
    if (objPath.empty())
    {
        return;
    }

    utils::PropertyValue value{compId};
    utils::DBusMapping dbusMapping{objPath,
                                   "xyz.openbmc_project.Software.Version",
                                   "SoftwareId", "string"};
    std::thread propertySet([dbusMapping, value] {
        try
        {
            std::string tmpVal = std::get<std::string>(value);
            setDBusProperty(dbusMapping, tmpVal);
        }
        catch (const std::exception& e)
        {
            // If the D-Bus object is not present, skip updating SoftwareID
            // and update later by registering for D-Bus signal.
        }
    });
    propertySet.detach();

    compIdentifierLookup.emplace(objPath, compId);
    updateFwMatch.emplace_back(
        bus,
        sdbusplus::bus::match::rules::interfacesAdded() +
            sdbusplus::bus::match::rules::argNpath(0, objPath),
        std::bind_front(&Manager::updateSwIdOnSignal, this));
}

void Manager::updateSwIdOnSignal(sdbusplus::message::message& msg)
{
    sdbusplus::message::object_path objPath;
    dbus::InterfaceMap interfaces;
    msg.read(objPath, interfaces);

    if (!interfaces.contains("xyz.openbmc_project.Software.Version"))
    {
        return;
    }

    if (compIdentifierLookup.contains(objPath))
    {
        auto search = compIdentifierLookup.find(objPath);

        utils::PropertyValue value{search->second};
        utils::DBusMapping dbusMapping{objPath,
                                       "xyz.openbmc_project.Software.Version",
                                       "SoftwareId", "string"};
        std::thread propertySet([dbusMapping, value] {
            try
            {
                std::string tmpVal = std::get<std::string>(value);
                setDBusProperty(dbusMapping, tmpVal);
            }
            catch (const std::exception& e)
            {
                lg2::error("SoftwareId set error: {ERROR}", "ERROR", e);
            }
        });
        propertySet.detach();
    }
}

} // namespace pldm::fw_update::fw_inventory
