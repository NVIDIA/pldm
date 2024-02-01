#pragma once

#include "libpldm/platform.h"
#include "libpldm/requester/pldm.h"
#ifdef OEM_NVIDIA
#include "oem/nvidia/libpldm/energy_count_numeric_sensor_oem.h"
#endif

#include "common/types.hpp"
#include "platform-mc/oem_base.hpp"

#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/Area/server.hpp>
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
using PhysicalContextType = sdbusplus::xyz::openbmc_project::Inventory::
    Decorator::server::Area::PhysicalContextType;
using InventoryDecoratorAreaIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::Area>;
using sensorMap = std::map<
    std::string,
    std::tuple<std::variant<std::string, int, int16_t, int64_t, uint16_t,
                            uint32_t, uint64_t, double, bool>,
               uint64_t, sdbusplus::message::object_path>>;

enum polling_method_indicator {
    POLLING_METHOD_INDICATOR_PLDM_TYPE_TWO,
    POLLING_METHOD_INDICATOR_PLDM_TYPE_OEM
};

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
#ifdef OEM_NVIDIA
    NumericSensor(const tid_t tid, const bool sensorDisabled,
                  std::shared_ptr<pldm_oem_energycount_numeric_sensor_value_pdr> pdr,
                  std::string& sensorName, std::string& associationPath, uint8_t oemIndicator);
#endif
    ~NumericSensor(){};

    /** @brief The function called by Sensor Manager to set sensor to
     * error status.
     */
    void handleErrGetSensorReading();

    /** @brief Updating the sensor status to D-Bus interface
     */
    void updateReading(bool available, bool functional, double value = 0);

    /** @brief ConversionFormula is used to convert raw value to the unit
     * specified in PDR
     *
     *  @param[in] value - raw value
     *  @return double - converted value
     */
    double conversionFormula(double value);

    /** @brief UnitModifier is used to apply the unit modifier specified in PDR
     *
     *  @param[in] value - raw value
     *  @return double - converted value
     */
    double unitModifier(double value);

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

    /** @brief Updating the physicalContext to D-Bus interface
     *  @param[in] type - physical context type
     */
    inline void setPhysicalContext(PhysicalContextType type)
    {
        if (inventoryDecoratorAreaIntf)
        {
            inventoryDecoratorAreaIntf->physicalContext(type);
        }
    }

    /** @brief Get Upper Critical threshold
     *
     *  @return double - Upper Critical threshold
     */
    double getThresholdUpperCritical()
    {
        if (thresholdCriticalIntf)
        {
            return thresholdCriticalIntf->criticalHigh();
        }
        else
        {
            return std::numeric_limits<double>::quiet_NaN();
        }
    };

    /** @brief Get Lower Critical threshold
     *
     *  @return double - Lower Critical threshold
     */
    double getThresholdLowerCritical()
    {
        if (thresholdCriticalIntf)
        {
            return thresholdCriticalIntf->criticalLow();
        }
        else
        {
            return std::numeric_limits<double>::quiet_NaN();
        }
    };

    /** @brief Get Upper Warning threshold
     *
     *  @return double - Upper Warning threshold
     */
    double getThresholdUpperWarning()
    {
        return thresholdWarningIntf->warningHigh();
    };

    /** @brief Get Lower Warning threshold
     *
     *  @return double - Lower Warning threshold
     */
    double getThresholdLowerWarning()
    {
        return thresholdWarningIntf->warningLow();
    };

    /** @brief Get base unit defined in table74 of DSP0248 v1.2.1
     *
     *  @return uint8_t - base unit
     */
    uint8_t getBaseUnit()
    {
        return baseUnit;
    };

    /** @brief Get Sensor Reading
     *
     *  @return double
     */
    double getReading()
    {
        if (valueIntf)
        {
            return valueIntf->value();
        }
        return unitModifier(conversionFormula(rawValue));
    };

    /** @brief Get polling method indicator
     *
     *  @return uint8_t - polling indicator
     */
    uint8_t getPollingIndicator()
    {
        return pollingIndicator;
    };

    /** @brief Terminus ID which the sensor belongs to */
    tid_t tid;

    /** @brief Sensor ID */
    uint16_t sensorId;

    /** @brief ContainerID, EntityType, EntityInstance of the PLDM Entity which
     * the sensor belongs to */
    EntityInfo entityInfo;

    /** @brief  The DBus path of sensor */
    std::string path;

    /** @brief  The time since last getSensorReading command in usec */
    uint64_t elapsedTime;

    /** @brief  The time of sensor update interval in usec */
    uint64_t updateTime;

    /** @brief  sensorName */
    std::string sensorName;

    /** @brief  sensorNameSpace */
    std::string sensorNameSpace;

    /** @brief indicate that the sensor should be included in sensorMetrics */
    bool inSensorMetrics;

    /** @brief indicate if sensor is polled in priority */
    bool isPriority;

    /** @brief  A container to store OemIntf, it allows us to add additional OEM
     * sdbusplus object as extra attribute */
    std::vector<std::shared_ptr<platform_mc::OemIntf>> oemIntfs;

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
    std::unique_ptr<InventoryDecoratorAreaIntf> inventoryDecoratorAreaIntf =
        nullptr;

    /** @brief Amount of hysteresis associated with the sensor thresholds */
    double hysteresis;

    /** @brief The resolution of sensor in Units */
    double resolution;

    /** @brief A constant value that is added in as part of conversion process
     * of converting a raw sensor reading to Units */
    double offset;

    /** @brief A power-of-10 multiplier for baseUnit */
    int8_t baseUnitModifier;

    /** @brief sensor reading baseUnit */
    uint8_t baseUnit;

    /** @brief raw value of numeric sensor */
    double rawValue;
    
    /** @brief indicates if we are using PLDM Type-2 command or PLDM OEM Type 
     * command for polling */
    uint8_t pollingIndicator;
};
} // namespace platform_mc
} // namespace pldm
