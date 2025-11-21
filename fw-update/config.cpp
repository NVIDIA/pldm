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
#include "config.hpp"

#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>

#include <fstream>

PHOSPHOR_LOG2_USING;

namespace pldm::fw_update
{

using Json = nlohmann::json;

void parseConfig(const fs::path& jsonPath,
                 DeviceInventoryInfo& deviceInventoryInfo,
                 FirmwareInventoryInfo& fwInventoryInfo,
                 ComponentNameMapInfo& componentNameMapInfo)
{
    if (!fs::exists(jsonPath))
    {
        // No error tracing to avoid polluting journal for users not using
        // config JSON
        return;
    }

    std::ifstream jsonFile(jsonPath);
    auto data = Json::parse(jsonFile, nullptr, false);
    if (data.is_discarded())
    {
        error("Parsing fw_update config file failed, FILE={JSONPATH}",
              "JSONPATH", jsonPath);
        return;
    }

    const Json emptyJson{};

    auto entries = data.value("entries", emptyJson);
    for (const auto& entry : entries.items())
    {
        auto match = entry.value()["match"];
        auto intf = match["Interface"].get<std::string>();
        auto props = match["Properties"];
        dbus::PropertyMap propMap;
        for (const auto& prop : props.items())
        {
            auto name = prop.value()["Name"].get<std::string>();
            auto type = prop.value()["Type"].get<std::string>();
            dbus::Value value;
            if (type == "s")
            {
                value = prop.value()["Value"].get<std::string>();
            }
            else if (type == "u")
            {
                value = prop.value()["Value"].get<uint32_t>();
            }
            else if (type == "y")
            {
                value = prop.value()["Value"].get<uint8_t>();
            }
            propMap.emplace(name, value);
        }

        DBusIntfMatch toMatch = {intf, propMap};

        if (entry.value().contains("device_inventory"))
        {
            DeviceObjPath createObjPath{};
            DeviceObjPath updateObjPath{};
            Associations assocs{};
            if (entry.value()["device_inventory"].contains("update"))
            {
                updateObjPath =
                    entry.value()["device_inventory"]["update"]["object_path"]
                        .get<std::string>();
            }
            if (entry.value()["device_inventory"].contains("create"))
            {
                createObjPath =
                    entry.value()["device_inventory"]["create"]["object_path"]
                        .get<std::string>();

                if (entry.value()["device_inventory"]["create"].contains(
                        "associations"))
                {
                    auto associations = entry.value()["device_inventory"]
                                                     ["create"]["associations"];
                    for (const auto& assocEntry : associations.items())
                    {
                        auto forward = assocEntry.value()["forward"];
                        auto reverse = assocEntry.value()["reverse"];
                        auto endpoint = assocEntry.value()["endpoint"];
                        assocs.emplace_back(
                            std::make_tuple(forward, reverse, endpoint));
                    }
                }
            }

            deviceInventoryInfo.infos.push_back(std::make_tuple(
                toMatch,
                std::make_tuple(std::make_tuple(std::move(createObjPath),
                                                std::move(assocs)),
                                std::move(updateObjPath))));
        }

        if (entry.value().contains("firmware_inventory"))
        {
            CreateComponentIdNameMap createcomponentIdNameMap{};
            UpdateComponentIdNameMap updatecomponentIdNameMap{};

            if (entry.value()["firmware_inventory"].contains("create"))
            {
                for (const auto& createObject :
                     entry.value()["firmware_inventory"]["create"].items())
                {
                    Associations assocs{};

                    if (createObject.value().contains("associations"))
                    {
                        auto associations =
                            createObject.value()["associations"];
                        for (const auto& assocEntry : associations.items())
                        {
                            auto forward = assocEntry.value()["forward"];
                            auto reverse = assocEntry.value()["reverse"];
                            auto endpoint = assocEntry.value()["endpoint"];
                            assocs.emplace_back(
                                std::make_tuple(forward, reverse, endpoint));
                        }
                    }

                    if (createObject.value().contains("component_id"))
                    {
                        auto componentID = createObject.value()["component_id"];
                        auto componentName = createObject.key();
                        createcomponentIdNameMap[componentID] = {componentName,
                                                                 assocs};
                    }
                }
            }
            if (entry.value()["firmware_inventory"].contains("update"))
            {
                for (auto& [componentName, componentID] :
                     entry.value()["firmware_inventory"]["update"].items())
                {
                    updatecomponentIdNameMap[componentID] = componentName;
                }
            }

            fwInventoryInfo.infos.push_back(std::make_tuple(
                toMatch, std::make_tuple(std::move(createcomponentIdNameMap),
                                         std::move(updatecomponentIdNameMap))));
        }

        if (entry.value().contains("component_info"))
        {
            ComponentIdNameMap componentIdNameMap{};
            for (auto& [componentName, componentID] :
                 entry.value()["component_info"].items())
            {
                componentIdNameMap[componentID] = componentName;
            }
            if (componentIdNameMap.size())
            {
                componentNameMapInfo.infos.push_back(std::make_tuple(
                    std::move(toMatch), std::move(componentIdNameMap)));
            }
        }
    }
}

namespace
{

/**
 * @brief Extract EID value from a dbus::Value variant
 *
 * Handles both uint8_t (Type "y") and unsigned int (Type "u") variants.
 *
 * @param[in] value - The dbus::Value variant containing the EID
 * @return optional<uint8_t> - The extracted EID value, or nullopt if extraction
 * fails or value exceeds uint8_t range
 */
std::optional<uint8_t> extractEid(const pldm::dbus::Value& value)
{
    try
    {
        // Try uint8_t first (Type "y")
        return std::get<uint8_t>(value);
    }
    catch (const std::bad_variant_access&)
    {
        try
        {
            // Try unsigned int (Type "u")
            auto eidValue = std::get<unsigned int>(value);
            if (eidValue <= 255)
            {
                return static_cast<uint8_t>(eidValue);
            }
            error("EID value {EID} exceeds uint8_t range, skipping", "EID",
                  eidValue);
        }
        catch (const std::bad_variant_access&)
        {
            // EID property is neither uint8_t nor unsigned int
        }
    }
    return std::nullopt;
}

/**
 * @brief Build a map of EID to device/component name from config
 *
 * Parses the fw_update_config.json and builds a cached lookup map.
 * Primary source: device_inventory.create.object_path
 * Fallback: First firmware component name from firmware_inventory
 *
 * @return unordered_map of mctp_eid_t to device name string
 */
std::unordered_map<mctp_eid_t, std::string> buildEidToNameMap()
{
    std::unordered_map<mctp_eid_t, std::string> map;

    try
    {
        // Parse the fw_update_config.json file
        DeviceInventoryInfo deviceInventoryInfo;
        FirmwareInventoryInfo fwInventoryInfo;
        ComponentNameMapInfo componentNameMapInfo;

        parseConfig(FW_UPDATE_CONFIG_JSON, deviceInventoryInfo, fwInventoryInfo,
                    componentNameMapInfo);

        // Build component_id → component_name map for fallback lookups
        std::unordered_map<uint8_t, std::unordered_map<uint16_t, std::string>>
            eidToComponentNameMap;
        for (const auto& entry : componentNameMapInfo.infos)
        {
            const auto& [dbusIntfMatch, componentMap] = entry;
            const auto& [interface, propertyMap] = dbusIntfMatch;

            auto eidProp = propertyMap.find("EID");
            if (eidProp != propertyMap.end())
            {
                auto eidOpt = extractEid(eidProp->second);
                if (eidOpt.has_value() && *eidOpt != 0)
                {
                    eidToComponentNameMap[*eidOpt] = componentMap;
                }
            }
        }

        // Build EID→name map from parsed config
        // deviceInventoryInfo.infos is vector<tuple<DBusIntfMatch, DeviceInfo>>
        for (const auto& entry : deviceInventoryInfo.infos)
        {
            const auto& [dbusIntfMatch, deviceInfo] = entry;
            const auto& [interface, propertyMap] = dbusIntfMatch;

            // Look for EID property in the match criteria
            auto eidProp = propertyMap.find("EID");
            if (eidProp == propertyMap.end())
            {
                continue;
            }

            // Extract EID value - can be Type "y" (uint8_t) or "u" (unsigned
            // int)
            auto configEidOpt = extractEid(eidProp->second);
            if (!configEidOpt.has_value())
            {
                continue;
            }
            uint8_t configEid = *configEidOpt;

            // Extract device object path from DeviceInfo
            // Try "create" section first, then "update" section
            const auto& [createDeviceInfo, updateDeviceInfo] = deviceInfo;
            const auto& [createDeviceObjPath, associations] = createDeviceInfo;

            std::string deviceObjPath;
            if (!createDeviceObjPath.empty())
            {
                deviceObjPath = createDeviceObjPath;
            }
            else if (!updateDeviceInfo.empty())
            {
                deviceObjPath = updateDeviceInfo;
            }

            if (!deviceObjPath.empty())
            {
                // Extract device name from object path (filename)
                std::string deviceName =
                    std::filesystem::path(deviceObjPath).filename();

                if (!deviceName.empty())
                {
                    map[configEid] = deviceName;
                }
            }
        }

        // FALLBACK: For EIDs without device_inventory section, try
        // firmware_inventory. This handles cases like EID 14, 24 which only
        // have firmware components
        for (const auto& entry : fwInventoryInfo.infos)
        {
            const auto& [dbusIntfMatch, fwInfo] = entry;
            const auto& [interface, propertyMap] = dbusIntfMatch;

            // Look for EID property
            auto eidProp = propertyMap.find("EID");
            if (eidProp == propertyMap.end())
            {
                continue;
            }

            // Extract EID value
            auto configEidOpt = extractEid(eidProp->second);
            if (!configEidOpt.has_value())
            {
                continue;
            }
            uint8_t configEid = *configEidOpt;

            // Skip if we already have a device name from device_inventory
            if (map.find(configEid) != map.end())
            {
                continue;
            }

            // Extract device name from first firmware component name
            // fwInfo is tuple<FirmwareInventoryMap,
            // UpdatedFirmwareInventoryMap>
            const auto& [createFwInfo, updateFwInfo] = fwInfo;

            // createFwInfo is a map of component_id (uint16_t) → component
            // details
            if (!createFwInfo.empty())
            {
                // Get the first component ID
                uint16_t firstComponentId = createFwInfo.begin()->first;

                // Look up the component name from componentNameMapInfo
                auto eidCompMapIt = eidToComponentNameMap.find(configEid);
                if (eidCompMapIt != eidToComponentNameMap.end())
                {
                    const auto& compNameMap = eidCompMapIt->second;
                    auto compNameIt = compNameMap.find(firstComponentId);
                    if (compNameIt != compNameMap.end())
                    {
                        const std::string& componentName = compNameIt->second;
                        if (!componentName.empty())
                        {
                            map[configEid] = componentName;
                        }
                    }
                }
            }
        }

        if (!map.empty())
        {
            info("Loaded {COUNT} EID→device name mappings from config", "COUNT",
                 map.size());
        }
    }
    catch (const std::exception& e)
    {
        error("Failed to load EID→name map from config: {ERROR}", "ERROR",
              e.what());
    }

    return map;
}

} // anonymous namespace

std::optional<std::string> getDeviceNameFromEid(mctp_eid_t eid)
{
    // Static map cached on first call - thread-safe initialization per C++11
    static std::unordered_map<mctp_eid_t, std::string> eidToNameMap =
        buildEidToNameMap();

    // Fast lookup in cached map
    auto it = eidToNameMap.find(eid);
    if (it != eidToNameMap.end())
    {
        return it->second;
    }

    return std::nullopt; // EID not found in config
}

} // namespace pldm::fw_update
