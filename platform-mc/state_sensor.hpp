#pragma once

#include "libpldm/platform.h"
#include "libpldm/requester/pldm.h"

#include "common/types.hpp"
#include "state_set.hpp"

#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/Sensor/Threshold/Critical/server.hpp>
#include <xyz/openbmc_project/Sensor/Threshold/Warning/server.hpp>
#include <xyz/openbmc_project/Sensor/Value/server.hpp>
#include <xyz/openbmc_project/State/Decorator/Availability/server.hpp>
#include <xyz/openbmc_project/State/Decorator/OperationalStatus/server.hpp>

#include <vector>

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
 * @brief StateSensor
 *
 * This class handles sensor reading updated by sensor manager and export
 * status to D-Bus interface.
 */
class StateSensor
{
  public:
    StateSensor(const uint8_t tid, const bool sensorDisabled,
                const uint16_t sensorId, StateSetInfo sensorInfo,
                std::string& sensorName, std::string& associationPath);
    ~StateSensor(){};

    /** @brief The function called by Sensor Manager to set sensor to
     * error status.
     */
    void handleErrGetSensorReading();

    void updateReading(bool available, bool functional, uint8_t compSensorIndex,
                       uint8_t value);

    /** @brief Get the ContainerID, EntityType, EntityInstance of the PLDM
     * Entity which the sensor belongs to
     *  @return EntityInfo - Entity ID
     */
    inline auto getEntityInfo()
    {
        return std::get<0>(sensorInfo);
    }

    /** @brief Updating the association to D-Bus interface
     *  @param[in] inventoryPath - inventory path of the entity
     */
    inline void setInventoryPath(const std::string& inventoryPath)
    {
        for (auto& stateSet : stateSets)
        {
            dbus::PathAssociation association = {"chassis", "all_states",
                                                 inventoryPath.c_str()};
            stateSet->setAssociation(association);
        }
    }

    void handleSensorEvent(uint8_t sensorOffset, uint8_t eventState);
    void createLogEntry(std::string& messageID, std::string& arg1,
                        std::string& arg2, std::string& resolution);

    /** @brief Terminus ID of the PLDM Terminus which the sensor belongs to
     */
    uint8_t tid;

    /** @brief Sensor ID */
    uint16_t sensorId;

    /** @brief  State Sensor Info */
    StateSetInfo sensorInfo;

    /** @brief flag to update the value once */
    bool needUpdate;

    /** @brief indicate the sensor updated asynchronously */
    bool async;

    std::string getAssociationEntityId()
    {
        return associationEntityId;
    }

  private:
    std::unique_ptr<AvailabilityIntf> availabilityIntf = nullptr;
    std::unique_ptr<OperationalStatusIntf> operationalStatusIntf = nullptr;
    StateSets stateSets;
    std::string associationEntityId;
};
} // namespace platform_mc
} // namespace pldm
