#pragma once

#include "libpldm/platform.h"
#include "libpldm/requester/pldm.h"

#include "common/types.hpp"

#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Sensor/Threshold/Critical/server.hpp>
#include <xyz/openbmc_project/Sensor/Threshold/Warning/server.hpp>
#include <xyz/openbmc_project/Sensor/Value/server.hpp>
#include <xyz/openbmc_project/State/Decorator/Availability/server.hpp>
#include <xyz/openbmc_project/State/Decorator/OperationalStatus/server.hpp>

namespace pldm
{
namespace platform_mc
{

using namespace std::chrono;
using namespace pldm::pdr;
using SensorUnit = sdbusplus::xyz::openbmc_project::Sensor::server::Value::Unit;
using ValueIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Sensor::server::Value>;
using ThresholdWarningIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Sensor::Threshold::server::Warning>;
using ThresholdCriticalIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Sensor::Threshold::server::Critical>;
using OperationalStatusIntf =
    sdbusplus::server::object_t<sdbusplus::xyz::openbmc_project::State::
                                    Decorator::server::OperationalStatus>;
using AvailabilityIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::State::Decorator::server::Availability>;
using AssociationDefinitionsInft = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Association::server::Definitions>;

/**
 * @brief NumericSensor
 *
 * This class handles sensor reading updated by sensor manager and export
 * status to D-Bus interface.
 */
class NumericSensor
{
  public:
    NumericSensor(const tid_t tid, const bool sensorDisabled,
                  std::shared_ptr<pldm_numeric_sensor_value_pdr> pdr,
                  std::string& sensorName, std::string& associationPath);
    ~NumericSensor(){};

    /** @brief The function called by Sensor Manager to set sensor to
     * error status.
     */
    void handleErrGetSensorReading();

    /** @brief Updating the sensor status to D-Bus interface
     */
    void updateReading(bool available, bool functional, double value = 0);

    /** @brief ConversionFormula is used to convert raw value to normalized
     * units
     *
     *  @param[in] value - raw value
     *  @return double - converted value
     */
    double conversionFormula(double value);

    /** @brief Check if value is over threshold.
     *
     *  @param[in] alarm - previous alarm state
     *  @param[in] direction - upper or lower threshold checking
     *  @param[in] value - raw value
     *  @param[in] threshold - threshold value
     *  @param[in] hyst - hysteresis value
     *  @return bool - new alarm state
     */
    bool checkThreshold(bool alarm, bool direction, double value,
                        double threshold, double hyst);

    /** @brief Get the ContainerID, EntityType, EntityInstance of the PLDM
     * Entity which the sensor belongs to
     *  @return EntityInfo - Entity ID
     */
    inline auto getEntityInfo()
    {
        return entityInfo;
    }

    /** @brief Updating the association to D-Bus interface
     *  @param[in] inventoryPath - inventory path of the entity
     */
    inline void setInventoryPath(const std::string& inventoryPath)
    {
        if (associationDefinitionsIntf)
        {
            associationDefinitionsIntf->associations(
                {{"chassis", "all_sensors", inventoryPath.c_str()}});
        }
    }

    /** @brief Terminus ID which the sensor belongs to */
    tid_t tid;

    /** @brief Sensor ID */
    uint16_t sensorId;

    /** @brief ContainerID, EntityType, EntityInstance of the PLDM Entity which
     * the sensor belongs to */
    EntityInfo entityInfo;

    /** @brief  The time since last getSensorReading command in usec */
    uint64_t elapsedTime;

    /** @brief  The time of sensor update interval in usec */
    uint64_t updateTime;

  private:
    /**
     * @brief Check sensor reading if any threshold has been crossed and update
     * Threshold interfaces accordingly
     */
    void updateThresholds();

    std::unique_ptr<ValueIntf> valueIntf = nullptr;
    std::unique_ptr<ThresholdWarningIntf> thresholdWarningIntf = nullptr;
    std::unique_ptr<ThresholdCriticalIntf> thresholdCriticalIntf = nullptr;
    std::unique_ptr<AvailabilityIntf> availabilityIntf = nullptr;
    std::unique_ptr<OperationalStatusIntf> operationalStatusIntf = nullptr;
    std::unique_ptr<AssociationDefinitionsInft> associationDefinitionsIntf =
        nullptr;

    /** @brief Amount of hysteresis associated with the sensor thresholds */
    double hysteresis;

    /** @brief The resolution of sensor in Units */
    double resolution;

    /** @brief A constant value that is added in as part of conversion process
     * of converting a raw sensor reading to Units */
    double offset;

    /** @brief A power-of-10 multiplier for baseUnit */
    int8_t unitModifier;
};
} // namespace platform_mc
} // namespace pldm
