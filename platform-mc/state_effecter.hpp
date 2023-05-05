#pragma once

#include "libpldm/platform.h"
#include "libpldm/requester/pldm.h"

#include "common/types.hpp"
#include "platform-mc/oem_base.hpp"
#include "requester/handler.hpp"
#include "state_set.hpp"

#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/State/Decorator/Availability/server.hpp>
#include <xyz/openbmc_project/State/Decorator/OperationalStatus/server.hpp>

#include <vector>

class TestStateEffecter;

namespace pldm
{
namespace platform_mc
{

class TerminusManager;

using namespace pldm::pdr;
using namespace std::chrono;
using OperationalStatusIntf =
    sdbusplus::server::object_t<sdbusplus::xyz::openbmc_project::State::
                                    Decorator::server::OperationalStatus>;
using AvailabilityIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::State::Decorator::server::Availability>;


using StateType = sdbusplus::xyz::openbmc_project::State::Decorator::server::
    OperationalStatus::StateType;

/**
 * @brief StateEffecter
 *
 * This class handles pldm state effecter and export
 * status to D-Bus interface.
 */
class StateEffecter
{
  public:
    friend class ::TestStateEffecter;

    StateEffecter(const uint8_t tid, const bool effecterDisabled,
                  const uint16_t effecterId, StateSetInfo effecterInfo,
                  std::string& effecterName, std::string& associationPath,
                  TerminusManager& terminusManager);
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

    /** @brief Get current state effecter operational status
     *
     */
    StateType getOperationalStatus()
    {
        return operationalStatusIntf->state();
    }

    /** @brief Sending getStateEffecterStates command for the effecter
     *
     */
    requester::Coroutine getStateEffecterStates();

    /** @brief Sending setStateEffecterStates command for the effecter
     *
     */
    requester::Coroutine setStateEffecterStates(uint8_t cmpId, uint8_t value);

    /** @brief Terminus ID of the PLDM Terminus which the sensor belongs to */
    uint8_t tid;

    /** @brief effecter ID */
    uint16_t effecterId;

    /** @brief  State Set Info */
    StateSetInfo effecterInfo;

    /** @brief The function called by Sensor Manager to set sensor to
     * error status.
     */
    void handleErrGetStateEffecterStates();

    /** @brief Updating the sensor status to D-Bus interface
     */
    void updateReading(uint8_t compEffecterIndex,
                       pldm_effecter_oper_state effecterOperState,
                       uint8_t pendingValue, uint8_t presentValue);

    /** @brief  The DBus path of effecter */
    std::string path;

    /** @brief  A container to store OemIntf, it allows us to add additional OEM
     * sdbusplus object as extra attribute */
    std::vector<std::unique_ptr<platform_mc::OemIntf>> oemIntfs;

  private:
    std::unique_ptr<AvailabilityIntf> availabilityIntf = nullptr;
    std::unique_ptr<OperationalStatusIntf> operationalStatusIntf = nullptr;
    StateSets stateSets;

    TerminusManager& terminusManager;
};
} // namespace platform_mc
} // namespace pldm
