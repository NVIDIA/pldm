#pragma once

#include <sdbusplus/bus.hpp>
#include <sdbusplus/server.hpp>
#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/Inventory/Item/Cable/server.hpp>

#include <string>

namespace pldm
{
namespace dbus
{

using ItemCable = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::Cable>;

class Cable : public ItemCable
{
  public:
    Cable() = delete;
    ~Cable() override = default;
    Cable(const Cable&) = delete;
    Cable& operator=(const Cable&) = delete;

    Cable(sdbusplus::bus_t& bus, const std::string& objPath) :
        ItemCable(bus, objPath.c_str())
    {}

    double length() const override;
    double length(double value) override;
    std::string cableTypeDescription() const override;
    std::string cableTypeDescription(std::string value) override;
};

} // namespace dbus
} // namespace pldm
