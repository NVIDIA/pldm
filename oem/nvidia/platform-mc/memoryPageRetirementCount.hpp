#pragma once

#include "libpldm/platform.h"

#include "common/types.hpp"
#include "platform-mc/numeric_sensor.hpp"
#include "platform-mc/oem_base.hpp"

#include <com/nvidia/MemoryPageRetirementCount/server.hpp>
#include <sdbusplus/server/object.hpp>

#include <cmath>

namespace pldm
{
namespace platform_mc
{

using namespace sdbusplus;
using MemoryPageRetirementCountInft = sdbusplus::server::object_t<
    sdbusplus::com::nvidia::server::MemoryPageRetirementCount>;

class OemMemoryPageRetirementCountInft :
    public OemIntf,
    MemoryPageRetirementCountInft
{
  public:
    /** @brief Constructor to put object onto bus at a dbus path.
     *  @param[in] bus - Bus to attach to.
     *  @param[in] path - Path to attach at.
     */
    OemMemoryPageRetirementCountInft(std::shared_ptr<NumericSensor> sensor,
                                     bus::bus& bus, const char* path) :
        MemoryPageRetirementCountInft(bus, path),
        sensor(sensor)
    {}

    virtual ~OemMemoryPageRetirementCountInft() = default;

    uint32_t memoryPageRetirementCount() const override
    {
        auto value = sensor->getReading();
        if (std::isnan(value))
        {
            return 0;
        }
        return value;
    }

  private:
    std::shared_ptr<NumericSensor> sensor;
};

} // namespace platform_mc
} // namespace pldm
