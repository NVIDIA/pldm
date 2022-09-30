#include "device_inventory.hpp"
#include "dbusutil.hpp"
#include "libpldm/firmware_update.h"

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
}

Manager::Manager(sdbusplus::bus::bus& bus,
                 const DeviceInventoryInfo& deviceInventoryInfo,
                 const DescriptorMap& descriptorMap,
                 utils::DBusHandlerInterface* dBusHandlerIntf) :
    bus(bus),
    objectManager(bus, "/"), deviceInventoryInfo(deviceInventoryInfo),
    descriptorMap(descriptorMap), dBusHandlerIntf(dBusHandlerIntf)
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

        deviceEntryMap.emplace(
            uuid, std::make_unique<Entry>(bus, objPath, uuid, assocs, ecsku));
        deviceObjPath = objPath;

        const auto& updateObjPath = std::get<UpdateDeviceInfo>(search->second);

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
                std::cerr << e.what() << '\n';
            }
        });
        propertySet.detach();
    }
}

} // namespace pldm::fw_update::device_inventory
