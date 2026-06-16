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
#include "device_inventory.hpp"

#include "libpldm/firmware_update.h"

#include "dbusutil.hpp"

#include <format>

namespace pldm::fw_update::device_inventory
{

Entry::Entry(sdbusplus::bus_t& bus, const pldm::dbus::ObjectPath& objPath,
             const pldm::UUID& mctpUUID, const Associations& assocs,
             const std::string& sku) :
    Ifaces(bus, objPath.c_str(), action::defer_emit)
{
    Ifaces::type(ChassisType::Component, true);
    Ifaces::uuid(mctpUUID, true);
    Ifaces::associations(assocs, true);
    if (!sku.empty())
    {
        SKUIntf::sku(sku, true);
    }
    Ifaces::manufacturer("NVIDIA", true);
    Ifaces::locationType(LocationTypes::Embedded, true);
    Ifaces::emit_object_added();
    Ifaces::health(HealthType::OK);
}

Manager::Manager(sdbusplus::bus_t& bus,
                 const DeviceInventoryInfo& deviceInventoryInfo,
                 const DescriptorMap& descriptorMap) :
    bus(bus), objectManager(bus, "/"), deviceInventoryInfo(deviceInventoryInfo),
    descriptorMap(descriptorMap)
{}

std::optional<sdbusplus::object_path> Manager::createEntry(
    pldm::eid eid, const pldm::UUID& uuid, dbus::MctpInterfaces& mctpInterfaces)
{
    std::optional<sdbusplus::object_path> deviceObjPath{};

    auto descIt = descriptorMap.find(eid);
    if (descIt == descriptorMap.end())
    {
        lg2::info(
            "Skipping device inventory creation: descriptor not found, EID={EID}, UUID={UUID}",
            "EID", eid, "UUID", uuid);
        return deviceObjPath;
    }
    const auto descriptors = descIt->second;

    DeviceInfo deviceInfo;

    if (mctpInterfaces.find(uuid) != mctpInterfaces.end() &&
        deviceInventoryInfo.matchInventoryEntry(mctpInterfaces[uuid],
                                                deviceInfo))
    {
        const auto& objPath =
            std::get<DeviceObjPath>(std::get<CreateDeviceInfo>(deviceInfo));
        const auto& assocs =
            std::get<Associations>(std::get<CreateDeviceInfo>(deviceInfo));
        std::string ecsku{};
        std::string apsku{};
        for (const auto& [descType, descValue] : descriptors)
        {
            if (descType == PLDM_FWUP_VENDOR_DEFINED)
            {
                const auto& [vendorDescTitle, vendorDescInfo] =
                    std::get<VendorDefinedDescriptorInfo>(descValue);
                if ((vendorDescTitle == "ECSKU") &&
                    (vendorDescInfo.size() == 4))
                {
                    ecsku = std::format("0x{:02X}{:02X}{:02X}{:02X}",
                                        vendorDescInfo[0], vendorDescInfo[1],
                                        vendorDescInfo[2], vendorDescInfo[3]);
                }
                if (vendorDescTitle == "APSKU" && vendorDescInfo.size() == 4)
                {
                    apsku = std::format("0x{:02X}{:02X}{:02X}{:02X}",
                                        vendorDescInfo[0], vendorDescInfo[1],
                                        vendorDescInfo[2], vendorDescInfo[3]);
                }
            }
        }
        if (!objPath.empty())
        {
            // Inventory object is preserved across endpoint removal; refresh
            // it in place since re-registering the same path throws (-EEXIST).
            if (auto existing = deviceEntryMap.find(uuid);
                existing != deviceEntryMap.end())
            {
                if (!ecsku.empty())
                {
                    existing->second->sku(ecsku);
                }
            }
            else
            {
                deviceEntryMap.emplace(
                    uuid,
                    std::make_unique<Entry>(bus, objPath, uuid, assocs, ecsku));

                lg2::info(
                    "Created ERoT Chassis D-Bus object: path={PATH}, uuid={UUID}, ecsku={ECSKU}",
                    "PATH", objPath, "UUID", uuid, "ECSKU",
                    ecsku.empty() ? "N/A" : ecsku);
            }
            deviceObjPath = objPath;
        }

        const auto& updateObjPath = std::get<UpdateDeviceInfo>(deviceInfo);

        if (!apsku.empty() && !updateObjPath.empty())
        {
            updateSKU(updateObjPath, apsku);
        }
    }
    else
    {
        // Skip if UUID is not present or device inventory information from
        // firmware update config JSON is empty
        lg2::info(
            "Skipping device inventory creation: UUID not found or empty device inventory config, EID={EID}, UUID={UUID}",
            "EID", eid, "UUID", uuid);
    }
    return deviceObjPath;
}

std::optional<sdbusplus::object_path> Manager::updateEntry(
    pldm::eid eid, const pldm::UUID& uuid, dbus::MctpInterfaces& mctpInterfaces)
{
    if (auto deviceEntry = deviceEntryMap.find(uuid);
        deviceEntry != deviceEntryMap.end())
    {
        auto descIt = descriptorMap.find(eid);
        if (descIt == descriptorMap.end())
        {
            return std::nullopt;
        }
        const auto descriptors = descIt->second;

        DeviceInfo deviceInfo;
        if (mctpInterfaces.find(uuid) != mctpInterfaces.end() &&
            deviceInventoryInfo.matchInventoryEntry(mctpInterfaces[uuid],
                                                    deviceInfo))
        {
            std::string ecsku{};
            std::string apsku{};
            for (const auto& [descType, descValue] : descriptors)
            {
                if (descType == PLDM_FWUP_VENDOR_DEFINED)
                {
                    const auto& [vendorDescTitle, vendorDescInfo] =
                        std::get<VendorDefinedDescriptorInfo>(descValue);
                    if (vendorDescTitle == "ECSKU" &&
                        vendorDescInfo.size() == 4)
                    {
                        ecsku =
                            std::format("0x{:02X}{:02X}{:02X}{:02X}",
                                        vendorDescInfo[0], vendorDescInfo[1],
                                        vendorDescInfo[2], vendorDescInfo[3]);
                    }
                    if (vendorDescTitle == "APSKU" &&
                        vendorDescInfo.size() == 4)
                    {
                        apsku =
                            std::format("0x{:02X}{:02X}{:02X}{:02X}",
                                        vendorDescInfo[0], vendorDescInfo[1],
                                        vendorDescInfo[2], vendorDescInfo[3]);
                    }
                }
            }

            if (!ecsku.empty())
            {
                deviceEntry->second->sku(ecsku);
            }

            const auto& updateObjPath = std::get<UpdateDeviceInfo>(deviceInfo);
            if (!apsku.empty() && !updateObjPath.empty())
            {
                updateSKU(updateObjPath, apsku);
            }

            const auto& objPath =
                std::get<DeviceObjPath>(std::get<CreateDeviceInfo>(deviceInfo));

            lg2::info(
                "Device inventory already exists for UUID={UUID}, updated SKU if needed",
                "UUID", uuid);

            return objPath;
        }
        return std::nullopt;
    }

    lg2::info(
        "Device inventory not found for UUID={UUID} during refresh, skipping update",
        "UUID", uuid);
    return std::nullopt;
}

void Manager::updateSKU(const dbus::ObjectPath& objPath, const std::string& sku)
{
    if (objPath.empty())
    {
        return;
    }

    lg2::info("Setting APSKU for path={PATH}: apsku={APSKU}", "PATH", objPath,
              "APSKU", sku);
    setDBusPropertyAsync(
        objPath, "xyz.openbmc_project.Inventory.Decorator.SKU", "SKU", sku,
        [objPath, sku](bool success) {
            if (!success)
            {
                lg2::error("Failed to set APSKU for path={PATH}: apsku={APSKU}",
                           "PATH", objPath, "APSKU", sku);
            }
        });

    skuLookup.emplace(objPath, sku);
    updateSKUMatch.try_emplace(
        objPath, bus,
        sdbusplus::bus::match::rules::interfacesAdded() +
            sdbusplus::bus::match::rules::argNpath(0, objPath),
        std::bind_front(&Manager::updateSKUOnMatch, this));
}

void Manager::updateSKUOnMatch(sdbusplus::message::message& msg)
{
    sdbusplus::object_path objPath;
    dbus::InterfaceMap interfaces;
    msg.read(objPath, interfaces);

    if (!interfaces.contains("xyz.openbmc_project.Inventory.Decorator.SKU"))
    {
        return;
    }

    if (skuLookup.contains(objPath))
    {
        auto search = skuLookup.find(objPath);
        const auto& skuVal = search->second;
        const std::string& pathStr = objPath;

        lg2::info("Updating APSKU for path={PATH}: apsku={APSKU}", "PATH",
                  pathStr, "APSKU", skuVal);
        setDBusPropertyAsync(
            pathStr, "xyz.openbmc_project.Inventory.Decorator.SKU", "SKU",
            skuVal, [pathStr, skuVal](bool success) {
                if (!success)
                {
                    lg2::error(
                        "Failed to update APSKU for path={PATH}: apsku={APSKU}",
                        "PATH", pathStr, "APSKU", skuVal);
                }
            });
    }
}

} // namespace pldm::fw_update::device_inventory
