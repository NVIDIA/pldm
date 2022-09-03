#include "device_inventory.hpp"

#include "libpldm/firmware_update.h"

#include <fmt/format.h>

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
}

Manager::Manager(sdbusplus::bus::bus& bus,
                 const DeviceInventoryInfo& deviceInventoryInfo,
                 const DescriptorMap& descriptorMap) :
    bus(bus),
    objectManager(bus, "/"), deviceInventoryInfo(deviceInventoryInfo),
    descriptorMap(descriptorMap)
{}

sdbusplus::message::object_path Manager::createEntry(pldm::EID eid,
                                                     const pldm::UUID& uuid)
{
    sdbusplus::message::object_path deviceObjPath{};
    if (deviceInventoryInfo.contains(uuid) && descriptorMap.contains(eid))
    {
        auto search = deviceInventoryInfo.find(uuid);
        const auto& objPath =
            std::get<DeviceObjPath>(std::get<CreateDeviceInfo>(search->second));
        const auto& assocs =
            std::get<Associations>(std::get<CreateDeviceInfo>(search->second));
        auto descSearch = descriptorMap.find(eid);
        std::string ecsku{};
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
            }
        }

        deviceEntryMap.emplace(
            uuid, std::make_unique<Entry>(bus, objPath, uuid, assocs, ecsku));
        deviceObjPath = objPath;
    }
    else
    {
        // Skip if UUID is not present or device inventory information from
        // firmware update config JSON is empty
    }

    return deviceObjPath;
}

} // namespace pldm::fw_update::device_inventory
