#pragma once

#include <vector>
#include "libpldm/platform.h"
#include "libpldm/requester/pldm.h"
#include "state_set.hpp"
#include "common/types.hpp"

#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/State/Decorator/Availability/server.hpp>
#include <xyz/openbmc_project/State/Decorator/OperationalStatus/server.hpp>


namespace pldm
{
namespace platform_mc
{

using namespace pldm::pdr;
using namespace std::chrono;
using OperationalStatusIntf =
    sdbusplus::server::object_t<sdbusplus::xyz::openbmc_project::State::
                                          Decorator::server::OperationalStatus>;
using AvailabilityIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::State::Decorator::server::Availability>;
using StateSets = std::vector<std::unique_ptr<StateSet>>;

/**
 * @brief StateEffecter
 *
 * This class handles pldm state effecter and export
 * status to D-Bus interface.
 */
class StateEffecter
{
  public:
    StateEffecter(const uint8_t tid,
                  const bool effecterDisabled,
                  const uint16_t effecterId,
                  StateSetInfo effecterInfo,
                  std::string& effecterName, std::string& associationPath);
    ~StateEffecter(){};

    /** @brief Get the ContainerID, EntityType, EntityInstance of the PLDM
     * Entity which the sensor belongs to
     *  @return EntityInfo - Entity ID
     */
    inline auto getEntityInfo()
    {
        return std::get<0>(effecterInfo);
    }

    /** @brief Updating the association to D-Bus interface
     *  @param[in] inventoryPath - inventory path of the entity
     */
    inline void setInventoryPath(const std::string& inventoryPath)
    {
        for (auto& stateSet : stateSets)
        {
            dbus::PathAssociation association = {"chassis", "all_controls",
                                                 inventoryPath.c_str()};
            stateSet->setAssociation(association);
        }
    }

    /** @brief Terminus ID of the PLDM Terminus which the sensor belongs to */
    uint8_t tid;

   /** @brief effecter ID */
    uint16_t effecterId;

    /** @brief  State Set Info */
    StateSetInfo effecterInfo;


  private:
    std::unique_ptr<AvailabilityIntf> availabilityIntf = nullptr;
    std::unique_ptr<OperationalStatusIntf> operationalStatusIntf = nullptr;
    StateSets stateSets;
};
} // namespace platform_mc
} // namespace pldm
