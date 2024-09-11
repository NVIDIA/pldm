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

#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Common/UUID/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/Asset/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/Location/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Chassis/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/SPDMResponder/server.hpp>
#include <xyz/openbmc_project/State/Decorator/Health/server.hpp>

namespace pldm::fw_update::device_inventory
{

using ChassisIntf =
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::Chassis;
using UUIDIntf = sdbusplus::xyz::openbmc_project::Common::server::UUID;
using AssociationIntf =
    sdbusplus::xyz::openbmc_project::Association::server::Definitions;
using SPDMResponderIntf =
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::SPDMResponder;
using DecoratorAssetIntf =
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::Asset;
using LocationCodeIntf =
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::Location;
using DecoratorHealthIntf =
    sdbusplus::xyz::openbmc_project::State::Decorator::server::Health;

using Ifaces =
    sdbusplus::server::object::object<ChassisIntf, UUIDIntf, AssociationIntf,
                                      SPDMResponderIntf, DecoratorAssetIntf,
                                      LocationCodeIntf, DecoratorHealthIntf>;

/** @class Entry
 *
 *  Implementation of device inventory D-Bus object implementing the D-Bus
 *  interfaces.
 *
 *  a) xyz.openbmc_project.Inventory.Item.Chassis
 *  b) xyz.openbmc_project.Common.UUID
 *  c) xyz.openbmc_project.Association.Definitions
 *  d) xyz.openbmc_project.Inventory.Item.SPDMResponder
 *  e) xyz.openbmc_project.Inventory.Decorator.Asset
 *  f) xyz.openbmc_project.Inventory.Decorator.LocationCode
 */
class Entry : public Ifaces
{
  public:
    Entry() = delete;
    ~Entry() = default;
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
    Entry(Entry&&) = default;
    Entry& operator=(Entry&&) = default;

    /** @brief Constructor
     *
     *  @param[in] bus  - Bus to attach to
     *  @param[in] objPath - D-Bus object path
     *  @param[in] uuid - MCTP UUID
     *  @param[in] assocs - D-Bus associations
     *  @param[in] sku - SKU
     */
    explicit Entry(sdbusplus::bus::bus& bus,
                   const pldm::dbus::ObjectPath& objPath,
                   const pldm::UUID& uuid, const Associations& assocs,
                   const std::string& sku);
};

/** @class Manager
 *
 *  Object manager for device inventory objects
 */
class Manager
{
  public:
    Manager() = delete;
    ~Manager() = default;
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;
    Manager(Manager&&) = default;
    Manager& operator=(Manager&&) = default;

    /** @brief Constructor
     *
     *  @param[in] bus  - Bus to attach to
     *  @param[in] deviceInventoryInfo - Config info for device inventory
     *  @param[in] descriptorMap - Descriptor info of MCTP endpoints
     */
    explicit Manager(sdbusplus::bus::bus& bus,
                     const DeviceInventoryInfo& deviceInventoryInfo,
                     const DescriptorMap& descriptorMap,
                     utils::DBusHandlerInterface* dBusHandlerIntf);

    /** @brief Create device inventory object
     *
     *  @param[in] eid - MCTP endpointID
     *  @param[in] uuid - MCTP UUID of the device
     *
     *  @return Object path of the device inventory object, std::nullopt if
     * object path is empty
     */
    std::optional<sdbusplus::message::object_path>
        createEntry(pldm::EID eid, const pldm::UUID& uuid,
                    dbus::MctpInterfaces& mctpInterfaces);

  private:
    sdbusplus::bus::bus& bus;

    sdbusplus::server::manager::manager objectManager;

    /** @brief Config info for device inventory */
    const DeviceInventoryInfo& deviceInventoryInfo;

    /** @brief Descriptor info of MCTP endpoints */
    const DescriptorMap& descriptorMap;

    /** @brief Map to store device inventory objects */
    std::map<pldm::UUID, std::unique_ptr<Entry>> deviceEntryMap;

    /** @brief Interface to make D-Bus client calls */
    utils::DBusHandlerInterface* dBusHandlerIntf;

    /** @brief D-Bus signal match for objects to be updated with SKU*/
    std::vector<sdbusplus::bus::match_t> updateSKUMatch;

    /** @brief Lookup table to find the SKU for the input D-Bus object
     */
    std::unordered_map<dbus::ObjectPath, std::string> skuLookup;

    /** @brief Update SKU on the D-Bus object and register for InterfaceAdded
     *         signal to update if the D-Bus object is created again.
     *
     *  @param[in] objPath - D-Bus object path
     *  @param[in] sku - SKU
     */
    void updateSKU(const dbus::ObjectPath& objPath, const std::string& sku);

    /** @brief Update SKU on the D-Bus object
     *
     *  @param[in] msg - D-Bus message
     */
    void updateSKUOnMatch(sdbusplus::message::message& msg);
};

} // namespace pldm::fw_update::device_inventory
