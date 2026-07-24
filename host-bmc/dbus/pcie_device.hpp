#pragma once

#include <sdbusplus/bus.hpp>
#include <sdbusplus/server.hpp>
#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/Inventory/Item/PCIeDevice/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/PCIeSlot/common.hpp>

#include <string>

namespace pldm
{
namespace dbus
{
using ItemDevice = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::PCIeDevice>;
using Generations = sdbusplus::common::xyz::openbmc_project::inventory::item::
    PCIeSlot::Generations;

class PCIeDevice : public ItemDevice
{
  public:
    PCIeDevice() = delete;
    ~PCIeDevice() override = default;
    PCIeDevice(const PCIeDevice&) = delete;
    PCIeDevice& operator=(const PCIeDevice&) = delete;

    PCIeDevice(sdbusplus::bus_t& bus, const std::string& objPath) :
        ItemDevice(bus, objPath.c_str())
    {}

    size_t lanesInUse() const override;
    size_t lanesInUse(size_t value) override;
    Generations generationInUse() const override;
    Generations generationInUse(Generations value) override;
};

} // namespace dbus
} // namespace pldm
