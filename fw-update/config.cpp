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
        try
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
                else
                {
                    error(
                        "Skipping property '{NAME}' with unrecognised type '{TYPE}'",
                        "NAME", name, "TYPE", type);
                    continue;
                }
                propMap.emplace(name, value);
            }

            DBusIntfMatch toMatch = {intf, propMap};

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
                                auto forward = assocEntry.value()["forward"]
                                                   .get<std::string>();
                                auto reverse = assocEntry.value()["reverse"]
                                                   .get<std::string>();
                                auto endpoint = assocEntry.value()["endpoint"]
                                                    .get<std::string>();
                                assocs.emplace_back(std::make_tuple(
                                    forward, reverse, endpoint));
                            }
                        }

                        if (createObject.value().contains("component_id"))
                        {
                            auto componentID =
                                createObject.value()["component_id"]
                                    .get<uint16_t>();
                            auto componentName = createObject.key();
                            std::string manufacturer = "NVIDIA";
                            if (createObject.value().contains("manufacturer"))
                            {
                                manufacturer =
                                    createObject.value()["manufacturer"]
                                        .get<std::string>();
                            }
                            createcomponentIdNameMap[componentID] = {
                                componentName, assocs, manufacturer, false};
                        }
                    }
                }
                if (entry.value()["firmware_inventory"].contains("update"))
                {
                    for (auto& [componentName, componentID] :
                         entry.value()["firmware_inventory"]["update"].items())
                    {
                        updatecomponentIdNameMap[componentID.get<uint16_t>()] =
                            componentName;
                    }
                }

                fwInventoryInfo.infos.push_back(std::make_tuple(
                    toMatch,
                    std::make_tuple(std::move(createcomponentIdNameMap),
                                    std::move(updatecomponentIdNameMap))));
            }

            if (entry.value().contains("component_info"))
            {
                ComponentIdNameMap componentIdNameMap{};
                for (auto& [componentName, componentID] :
                     entry.value()["component_info"].items())
                {
                    componentIdNameMap[componentID.get<uint16_t>()] =
                        componentName;
                }
                if (!componentIdNameMap.empty())
                {
                    componentNameMapInfo.infos.push_back(std::make_tuple(
                        std::move(toMatch), std::move(componentIdNameMap)));
                }
            }
        }
        catch (const std::exception& e)
        {
            error("Skipping malformed fw_update config entry '{KEY}': {ERROR}",
                  "KEY", entry.key(), "ERROR", e);
            continue;
        }
    }
}

} // namespace pldm::fw_update
