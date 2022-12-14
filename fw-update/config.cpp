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
        auto mctp_endpoint_uuid =
            entry.value()["mctp_endpoint_uuid"].get<std::string>();

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

            deviceInventoryInfo[mctp_endpoint_uuid] = std::make_tuple(
                std::make_tuple(std::move(createObjPath), std::move(assocs)),
                std::move(updateObjPath));
        }

        if (entry.value().contains("firmware_inventory"))
        {
            CreateComponentIdNameMap createcomponentIdNameMap{};
            UpdateComponentIdNameMap updatecomponentIdNameMap{};
            if (entry.value()["firmware_inventory"].contains("create"))
            {
                for (auto& [componentName, componentID] :
                     entry.value()["firmware_inventory"]["create"].items())
                {
                    createcomponentIdNameMap[componentID] = componentName;
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

            fwInventoryInfo[mctp_endpoint_uuid] =
                std::make_tuple(std::move(createcomponentIdNameMap),
                                std::move(updatecomponentIdNameMap));
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
                componentNameMapInfo[mctp_endpoint_uuid] = componentIdNameMap;
            }
        }
    }
}

} // namespace pldm::fw_update