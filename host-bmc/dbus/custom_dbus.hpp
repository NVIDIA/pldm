#pragma once

#include "asset.hpp"
#include "availability.hpp"
#include "board.hpp"
#include "cable.hpp"
#include "chassis.hpp"
#include "common/utils.hpp"
#include "connector.hpp"
#include "cpu_core.hpp"
#include "fabric_adapter.hpp"
#include "fan.hpp"
#include "inventory_item.hpp"
#include "motherboard.hpp"
#include "panel.hpp"
#include "pcie_device.hpp"
#include "pcie_slot.hpp"
#include "power_supply.hpp"
#include "vrm.hpp"

#include <sdbusplus/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/LocationCode/server.hpp>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace pldm
{
namespace dbus
{
using ObjectPath = std::string;

using LocationIntf =
    sdbusplus::server::object_t<sdbusplus::xyz::openbmc_project::Inventory::
                                    Decorator::server::LocationCode>;

class CustomDBus
{
  private:
    CustomDBus() {}

  public:
    CustomDBus(const CustomDBus&) = delete;
    CustomDBus(CustomDBus&&) = delete;
    CustomDBus& operator=(const CustomDBus&) = delete;
    CustomDBus& operator=(CustomDBus&&) = delete;
    ~CustomDBus() = default;

    static CustomDBus& getCustomDBus()
    {
        static CustomDBus customDBus;
        return customDBus;
    }

    void setLocationCode(const std::string& path, std::string value);
    std::optional<std::string> getLocationCode(const std::string& path) const;
    void implementCpuCoreInterface(const std::string& path);
    void setMicroCode(const std::string& path, uint32_t value);
    std::optional<uint32_t> getMicroCode(const std::string& path) const;
    void implementPCIeSlotInterface(const std::string& path);
    void setSlotType(const std::string& path, const std::string& slotType);
    void implementPCIeDeviceInterface(const std::string& path);
    void setPCIeDeviceProps(const std::string& path, size_t lanesInUse,
                            const std::string& value);
    void implementCableInterface(const std::string& path);
    void setCableAttributes(const std::string& path, double length,
                            const std::string& cableDescription);
    void implementMotherboardInterface(const std::string& path);
    void implementFanInterface(const std::string& path);
    void implementChassisInterface(const std::string& path);
    void implementPowerSupplyInterface(const std::string& path);
    void implementConnecterInterface(const std::string& path);
    void implementFabricAdapter(const std::string& path);
    void implementBoard(const std::string& path);
    void implementAssetInterface(const std::string& path);
    void setAvailabilityState(const std::string& path, const bool& state);
    void updateItemPresentStatus(const std::string& path, bool isPresent);
    void implementPanelInterface(const std::string& path);
    void implementVRMInterface(const std::string& path);

  private:
    std::unordered_map<ObjectPath, std::unique_ptr<Asset>> asset;
    std::unordered_map<ObjectPath, std::unique_ptr<Availability>>
        availabilityState;
    std::unordered_map<ObjectPath, std::unique_ptr<LocationIntf>> location;
    std::unordered_map<ObjectPath, std::unique_ptr<InventoryItem>>
        presentStatus;
    std::unordered_map<ObjectPath, std::unique_ptr<CPUCore>> cpuCore;
    std::unordered_map<ObjectPath, std::unique_ptr<ItemChassis>> chassis;
    std::unordered_map<ObjectPath, std::unique_ptr<PCIeDevice>> pcieDevice;
    std::unordered_map<ObjectPath, std::unique_ptr<PCIeSlot>> pcieSlot;
    std::unordered_map<ObjectPath, std::unique_ptr<PowerSupply>> powersupply;
    std::unordered_map<ObjectPath, std::unique_ptr<Board>> board;
    std::unordered_map<ObjectPath, std::unique_ptr<FabricAdapter>>
        fabricAdapter;
    std::unordered_map<ObjectPath, std::unique_ptr<Cable>> cable;
    std::unordered_map<ObjectPath, std::unique_ptr<Motherboard>> motherboard;
    std::unordered_map<ObjectPath, std::unique_ptr<Fan>> fan;
    std::unordered_map<ObjectPath, std::unique_ptr<Connector>> connector;
    std::unordered_map<ObjectPath, std::unique_ptr<Panel>> panel;
    std::unordered_map<ObjectPath, std::unique_ptr<VRM>> vrm;
};

} // namespace dbus
} // namespace pldm
