/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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

#include <fmt/format.h>

#include <thread>

namespace pldm::fw_update::device_inventory
{

Entry::Entry(sdbusplus::bus::bus& bus, const pldm::dbus::ObjectPath& objPath,
             const pldm::UUID& mctpUUID, const Associations& assocs,
             const std::string& sku) :
    Ifaces(bus, objPath.c_str(), action::defer_emit)
{
    Ifaces::type(ChassisType::Component, true);
    Ifaces::uuid(mctpUUID, true);
    Ifaces::associations(assocs, true);
    Ifaces::sku(sku, true);
    Ifaces::manufacturer("NVIDIA", true);
    Ifaces::locationType(LocationTypes::Embedded, true);
    Ifaces::emit_object_added();
    Ifaces::health(HealthType::OK);
}

Manager::Manager(sdbusplus::bus::bus& bus,
                 const DeviceInventoryInfo& deviceInventoryInfo,
                 const DescriptorMap& descriptorMap,
                 utils::DBusHandlerInterface* dBusHandlerIntf) :
    bus(bus),
    objectManager(bus, "/"), deviceInventoryInfo(deviceInventoryInfo),
    descriptorMap(descriptorMap), dBusHandlerIntf(dBusHandlerIntf)
{}

std::optional<sdbusplus::message::object_path>
    Manager::createEntry(pldm::EID eid, const pldm::UUID& uuid,
                         dbus::MctpInterfaces& mctpInterfaces)
{
    std::optional<sdbusplus::message::object_path> deviceObjPath{};

    DeviceInfo deviceInfo;

    if (mctpInterfaces.find(uuid) != mctpInterfaces.end() &&
        deviceInventoryInfo.matchInventoryEntry(mctpInterfaces[uuid],
                                                deviceInfo) &&
        descriptorMap.contains(eid))
    {
        const auto& objPath =
            std::get<DeviceObjPath>(std::get<CreateDeviceInfo>(deviceInfo));
        const auto& assocs =
            std::get<Associations>(std::get<CreateDeviceInfo>(deviceInfo));
        auto descSearch = descriptorMap.find(eid);
        std::string ecsku{};
        std::string apsku{};
        for (const auto& [descType, descValue] : descSearch->second)
        {
            if (descType == PLDM_FWUP_VENDOR_DEFINED)
            {
                const auto& [vendorDescTitle, vendorDescInfo] =
                    std::get<VendorDefinedDescriptorInfo>(descValue);
                if ((vendorDescTitle == "ECSKU") &&
                    (vendorDescInfo.size() == 4))
                {
                    ecsku = fmt::format("0x{:02X}{:02X}{:02X}{:02X}",
                                        vendorDescInfo[0], vendorDescInfo[1],
                                        vendorDescInfo[2], vendorDescInfo[3]);
                }
                if (vendorDescTitle == "APSKU" && vendorDescInfo.size() == 4)
                {
                    apsku = fmt::format("0x{:02X}{:02X}{:02X}{:02X}",
                                        vendorDescInfo[0], vendorDescInfo[1],
                                        vendorDescInfo[2], vendorDescInfo[3]);
                }
            }
        }
        if (!objPath.empty())
        {
            deviceEntryMap.emplace(
                uuid,
                std::make_unique<Entry>(bus, objPath, uuid, assocs, ecsku));
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
    }
    return deviceObjPath;
}

void Manager::updateSKU(const dbus::ObjectPath& objPath, const std::string& sku)
{
    if (objPath.empty())
    {
        return;
    }

    utils::PropertyValue value{sku};
    utils::DBusMapping dbusMapping{
        objPath, "xyz.openbmc_project.Inventory.Decorator.Asset", "SKU",
        "string"};
    std::thread propertySet([dbusMapping, value] {
        try
        {
            std::string tmpVal = std::get<std::string>(value);
            setDBusProperty(dbusMapping, tmpVal);
        }
        catch (const std::exception& e)
        {
            // If the D-Bus object is not present, skip updating SKU
            // and update later by registering for D-Bus signal.
        }
    });
    propertySet.detach();

    skuLookup.emplace(objPath, sku);
    updateSKUMatch.emplace_back(
        bus,
        sdbusplus::bus::match::rules::interfacesAdded() +
            sdbusplus::bus::match::rules::argNpath(0, objPath),
        std::bind_front(&Manager::updateSKUOnMatch, this));
}

void Manager::updateSKUOnMatch(sdbusplus::message::message& msg)
{
    sdbusplus::message::object_path objPath;
    dbus::InterfaceMap interfaces;
    msg.read(objPath, interfaces);

    if (!interfaces.contains("xyz.openbmc_project.Inventory.Decorator.Asset"))
    {
        return;
    }

    if (skuLookup.contains(objPath))
    {
        auto search = skuLookup.find(objPath);

        utils::PropertyValue value{search->second};
        utils::DBusMapping dbusMapping{
            objPath, "xyz.openbmc_project.Inventory.Decorator.Asset", "SKU",
            "string"};
        std::thread propertySet([dbusMapping, value] {
            try
            {
                std::string tmpVal = std::get<std::string>(value);
                setDBusProperty(dbusMapping, tmpVal);
            }
            catch (const std::exception& e)
            {
                lg2::error("Set SKU Error: {ERROR}", "ERROR", e);
            }
        });
        propertySet.detach();
    }
}

} // namespace pldm::fw_update::device_inventory
