#pragma once

#include <sdbusplus/bus.hpp>
#include <sdbusplus/server.hpp>
#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/Inventory/Item/PCIeSlot/server.hpp>

#include <string>

namespace pldm
{
namespace dbus
{
using ItemSlot = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::PCIeSlot>;

class PCIeSlot : public ItemSlot
{
  public:
    PCIeSlot() = delete;
    ~PCIeSlot() = default;
    PCIeSlot(const PCIeSlot&) = delete;
    PCIeSlot& operator=(const PCIeSlot&) = delete;

    PCIeSlot(sdbusplus::bus_t& bus, const std::string& objPath) :
        ItemSlot(bus, objPath.c_str())
    {}

    Generations generation() const override;
    Generations generation(Generations value) override;
    size_t lanes() const override;
    size_t lanes(size_t value) override;
    SlotTypes slotType() const override;
    SlotTypes slotType(SlotTypes value) override;
    bool hotPluggable() const override;
    bool hotPluggable(bool value) override;
};

} // namespace dbus
} // namespace pldm
