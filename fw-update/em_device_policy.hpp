/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "oem/nvidia/common/em_device.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/** @brief Firmware-update policy of the devices of the platform topology
 *         (oem/nvidia/common/em_device.hpp): the associations and
 *         inventory paths that follow from the kind of a device.
 */
namespace pldm::fw_update::em_device_policy
{

using pldm::em_device::baseName;
using pldm::em_device::bmcRoot;
using pldm::em_device::chassisRoot;
using pldm::em_device::componentRoot;
using pldm::em_device::Device;
using pldm::em_device::inventoryRoot;
using pldm::em_device::PldmDevice;

/** @brief Inventory object of the parent chassis of a RoT. EM gives the
 *         parent either as a plain BMC object name ("BMC_0") or relative to
 *         the system inventory ("bmc/BMC_0"). */
inline std::string parentChassisPath(const std::string& parent)
{
    if (parent.contains('/'))
    {
        return std::string(inventoryRoot) + "/system/" + parent;
    }
    return bmcRoot + parent;
}

/** @brief The last segment of a ParentChassis value, which may carry a type
 *         prefix ("board/HGX_ProcessorModule_0"). */
inline std::string parentLeaf(const std::string& parent)
{
    return parent.substr(parent.rfind('/') + 1);
}

struct ComponentRule
{
    std::string forward;
    std::string backward;
    std::vector<std::string> endpoints;
    /** @brief Chassis the component also belongs to, associated by the
     *         "inventory_chassis" forward name; empty when there is none. */
    std::string chassisEndpoint;
    std::string manufacturer = "NVIDIA";
    bool updateOnly = false;
};

/** @brief Association and policy of one firmware component, by device kind:
 *         the BMC firmware components (16 behind an ERoT, 10 behind an
 *         IRoT) are UpdateOnly; FWInventoryOnlyComponent entries are
 *         associated as "active", updateable ones as "activation". */
inline ComponentRule componentRule(const Device& dev, const std::string& prefix,
                                   const PldmDevice& record, uint16_t id)
{
    constexpr uint16_t bmcFwBehindErot = 16;
    constexpr uint16_t bmcFwBehindIrot = 10;
    constexpr uint16_t cxFirmware = 1;
    constexpr uint16_t cpuFirmwareBehindErot = 218;
    constexpr uint16_t hpmCpld = 65283;

    ComponentRule rule;
    if (id == bmcFwBehindErot || id == bmcFwBehindIrot)
    {
        rule.updateOnly = true;
        return rule;
    }
    rule.forward = "inventory";
    rule.backward = record.inventoryOnly.contains(id) ? "active" : "activation";
    if (dev.kind == "cpu")
    {
        rule.endpoints = {componentRoot + dev.name};
    }
    else if (dev.kind == "irot")
    {
        rule.endpoints = {parentChassisPath(dev.parentChassis)};
    }
    else if ((dev.kind == "cpu_erot" || dev.kind == "cpu_erot_i2c") &&
             id == cpuFirmwareBehindErot)
    {
        // the CPU firmware reached through the ERoT of the CPU belongs to
        // the CPU chassis; the ERoT's own firmware to the ERoT chassis
        rule.endpoints = {chassisRoot + parentLeaf(dev.parentChassis)};
    }
    else if (dev.kind == "cx9_io" && id == cxFirmware)
    {
        // the firmware belongs to the NIC and to the chassis of the device
        if (!record.networkAdapter.empty())
        {
            rule.endpoints.push_back(
                chassisRoot + dev.name + "/NetworkAdapters/" +
                record.networkAdapter);
        }
        rule.chassisEndpoint = chassisRoot + dev.name;
    }
    else if (dev.kind == "hmc_hpm_sma" && id == hpmCpld)
    {
        rule.endpoints = {componentRoot + prefix + "CPLD_" +
                          std::to_string(dev.instance.value_or(0))};
        rule.manufacturer = "Lattice";
    }
    else
    {
        rule.endpoints = {chassisRoot + dev.name};
    }
    return rule;
}

struct DownstreamRule
{
    std::string forward = "inventory";
    std::string backward = "activation";
    /** @brief The ports of the device, by sensor id: the downstream device
     *         is plugged into them. */
    std::vector<std::string> endpoints;
    /** @brief Descriptor of the downstream device the Manufacturer is
     *         taken from. */
    std::string manufacturerNameSource = "IEEE_OUI";
};

/** @brief Association and policy of the downstream devices of a device (the
 *         optical transceivers of a CX9 IO board): each belongs to the
 *         ports of the device, which sit under its network adapter or, when
 *         it has none, under its chassis. */
inline DownstreamRule downstreamRule(const Device& dev,
                                     const PldmDevice& record)
{
    DownstreamRule rule;
    const std::string parent =
        record.networkAdapter.empty()
            ? chassisRoot + dev.name
            : chassisRoot + dev.name + "/NetworkAdapters/" +
                  record.networkAdapter;
    for (const auto& [id, port] : record.ports)
    {
        std::string endpoint = parent;
        endpoint += "/Ports/";
        endpoint += port;
        rule.endpoints.push_back(std::move(endpoint));
    }
    return rule;
}

struct InventoryPaths
{
    std::string create;
    std::string update;
};

/** @brief Where pldmd writes the device identity (UUID, SKUs): the RoT
 *         chassis and the update target, by device kind. A device without
 *         a RoT chassis (create path empty) gets its AP-SKU only: a CX9 IO
 *         board, and the GPU and HPM SMAs (gpu_sma, hmc_hpm_sma). An IRoT
 *         gets nothing: nsmd reports and updates its AP-SKU (the shared
 *         bmc_irot profile), and a second writer on the BMC would hide the
 *         writable SKU, because bmcweb reads the SKU on the update target
 *         first and only follows the association to nsmd when it is empty. */
inline std::optional<InventoryPaths> inventoryPaths(const Device& dev,
                                                    const std::string& prefix)
{
    const std::string chassis = chassisRoot + dev.name;
    // the BMC's own RoT has the BMC as its parent
    const std::string bmc = parentChassisPath(dev.parentChassis);
    if (dev.kind == "gpu")
    {
        return InventoryPaths{
            chassisRoot + prefix + "IRoT_" + baseName(dev.name, prefix),
            chassis};
    }
    if (dev.kind == "bluefield_sma" || dev.kind == "cx_sma")
    {
        return InventoryPaths{chassisRoot + std::string("IRoT_") + dev.name,
                              chassis};
    }
    if (dev.kind == "erot")
    {
        return InventoryPaths{chassis, bmc};
    }
    if (dev.kind == "irot")
    {
        // nsmd owns the SKU of an IRoT
        return std::nullopt;
    }
    if (dev.kind == "cx9_io" || dev.kind == "gpu_sma" ||
        dev.kind == "hmc_hpm_sma")
    {
        return InventoryPaths{"", chassis};
    }
    return std::nullopt;
}

} // namespace pldm::fw_update::em_device_policy
