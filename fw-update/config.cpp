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
#include <iostream>

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
        lg2::error("Parsing fw_update config file failed, FILE={JSONPATH}",
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

} // namespace pldm::fw_update