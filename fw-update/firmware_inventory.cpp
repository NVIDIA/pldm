#include "firmware_inventory.hpp"

#include <fmt/format.h>

#include <iostream>

namespace pldm::fw_update::fw_inventory
{

Entry::Entry(sdbusplus::bus::bus& bus, const std::string& objPath,
             const std::string& versionStr, const std::string& swId) :
    Ifaces(bus, objPath.c_str(), action::defer_emit)
{
    Ifaces::version(versionStr, true);
    Ifaces::purpose(VersionPurpose::Other, true);
    Ifaces::softwareId(swId, true);
    Ifaces::emit_object_added();
}

void Entry::createInventoryAssociation(const std::string& deviceObjPath)
{
    auto assocs = associations();
    assocs.emplace_back(
        std::make_tuple(invFwdAssociation, invRevAssociation, deviceObjPath));
    associations(assocs);
}

void Entry::createUpdateableAssociation(const std::string& swObjPath)
{
    auto assocs = associations();
    assocs.emplace_back(
        std::make_tuple(upFwdAssociation, upRevAssociation, swObjPath));
    associations(assocs);
}

Manager::Manager(sdbusplus::bus::bus& bus,
                 const FirmwareInventoryInfo& firmwareInventoryInfo,
                 const ComponentInfoMap& componentInfoMap) :
    bus(bus),
    firmwareInventoryInfo(firmwareInventoryInfo),
    componentInfoMap(componentInfoMap)
{}

void Manager::createEntry(pldm::EID eid, const pldm::UUID& uuid,
                          const sdbusplus::message::object_path& objectPath)
{
    if (firmwareInventoryInfo.contains(uuid) && componentInfoMap.contains(eid))
    {
        auto fwInfoSearch = firmwareInventoryInfo.find(uuid);
        auto compInfoSearch = componentInfoMap.find(eid);

        for (const auto& [compKey, compInfo] : compInfoSearch->second)
        {
            if ((std::get<0>(fwInfoSearch->second)).contains(compKey.second))
            {
                auto componentName =
                    (std::get<0>(fwInfoSearch->second)).find(compKey.second);
                std::string objPath = swBasePath + "/" + componentName->second;
                auto swId = fmt::format("0x{:04X}", compKey.second);
                auto entry = std::make_unique<Entry>(
                    bus, objPath, std::get<1>(compInfo), swId);
                entry->createUpdateableAssociation(swBasePath);
                if (objectPath != "")
                {
                    entry->createInventoryAssociation(objectPath);
                }

                firmwareInventoryMap.emplace(
                    std::make_pair(eid, compKey.second), std::move(entry));
            }
        }
    }
    else
    {
        // Skip if UUID is not present or firmware inventory information from
        // firmware update config JSON is empty
    }
}

} // namespace pldm::fw_update::fw_inventory
