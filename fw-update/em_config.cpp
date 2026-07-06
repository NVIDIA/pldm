/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
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
#include "em_config.hpp"

#include "common/utils.hpp"

#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <exception>
#include <string>
#include <utility>
#include <variant>
#include <vector>

PHOSPHOR_LOG2_USING;

namespace pldm::fw_update::em_config
{

namespace
{

/** @brief Extract a uint16 ComponentIdentifier from a D-Bus variant that
 *         entity-manager may publish as several integral types.
 */
uint16_t extractUint16(const pldm::utils::PropertyValue& v)
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

/** @brief Read the per-component definitions of a
 *         Configuration.PLDMFirmwareDevice entry.
 *
 *  entity-manager publishes each element of the nested Components array as
 *  a separate CHILD interface "<baseIntf>.Components<index>" on the same
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
 *  @param[in]  service      - D-Bus service owning the object
 *                             (entity-manager)
 *  @param[in]  objPath      - PLDMFirmwareDevice object path
 *  @param[in]  baseIntf     - Configuration.PLDMFirmwareDevice interface
 *                             name
 *  @param[out] emComponents - id → {Name, Associations, Manufacturer}
 *  @param[out] idNameMap    - id → Name (for target filtering / fallback)
 */
void unpackComponents(const std::string& service, const std::string& objPath,
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
        // inventory copies onto the Software.Version
        // Association.Definitions.
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
        const size_t nAssoc = std::min({fwd.size(), bwd.size(), endp.size()});
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

        emComponents[compId] = {compName, std::move(compAssocs), manufacturer,
                                updateOnly};
        idNameMap[compId] = compName;
    }
}

} // namespace

std::string targetNameForEid(const Configurations& configurations,
                             pldm::eid mctpEid)
{
    // configured_by-resolved name (the only identity path).
    for (const auto& [emPath, mctpInfo] : configurations)
    {
        if (std::get<pldm::eid>(mctpInfo) != mctpEid)
        {
            continue;
        }
        const auto& nameOpt = std::get<std::optional<std::string>>(mctpInfo);
        if (nameOpt)
        {
            return *nameOpt;
        }
        break;
    }

    return {};
}

std::optional<DeviceComponentInfo> fetchComponentInfo(
    const Configurations& configurations, pldm::eid mctpEid)
{
    const std::string targetName = targetNameForEid(configurations, mctpEid);
    if (targetName.empty())
    {
        return std::nullopt;
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
            "fetchComponentInfo: GetSubTree for PLDMFirmwareDevice failed for EID {EID}, error - {ERROR}",
            "EID", mctpEid, "ERROR", e);
        return std::nullopt;
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
                "fetchComponentInfo: reading props at {PATH} failed, error - {ERROR}",
                "PATH", objPath, "ERROR", e);
            continue;
        }

        auto nameIt = props.find("MCTPTargetName");
        if (nameIt == props.end())
        {
            continue;
        }
        const auto* namePtr = std::get_if<std::string>(&nameIt->second);
        if (namePtr == nullptr || *namePtr != targetName)
        {
            continue;
        }

        DeviceComponentInfo info;
        unpackComponents(service, objPath, pldmFwDeviceIntf, info.emComponents,
                         info.idNameMap);
        return info;
    }
    return std::nullopt;
}

} // namespace pldm::fw_update::em_config
