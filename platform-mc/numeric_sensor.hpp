/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include "config.h"

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
#include <xyz/openbmc_project/Sensor/Threshold/HardShutdown/server.hpp>
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
using Associations =
    std::vector<std::tuple<std::string, std::string, std::string>>;
using ValueIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Sensor::server::Value>;
using ThresholdWarningIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Sensor::Threshold::server::Warning>;
using ThresholdCriticalIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Sensor::Threshold::server::Critical>;
using ThresholdFatalIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Sensor::Threshold::server::HardShutdown>;
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

enum polling_method_indicator
{
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
    NumericSensor(
        const tid_t tid, const bool sensorDisabled,
        std::shared_ptr<pldm_oem_energycount_numeric_sensor_value_pdr> pdr,
        std::string& sensorName, std::string& associationPath,
        uint8_t oemIndicator);
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
    inline void setInventoryPaths(const std::vector<std::string>& inventoryPath)
    {
        if (associationDefinitionsIntf)
        {
            Associations assocs{};

            for (const std::string& path : inventoryPath)
            {
                assocs.emplace_back(
                    std::make_tuple("chassis", "all_sensors", path.c_str()));
            }
            associationDefinitionsIntf->associations(assocs);
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

    /** @brief  The time of sensor update interval in usec */
    uint64_t updateTime;

    /** @brief  getter of sensorName */
    std::string getSensorName()
    {
        return sensorName;
    }

    /** @brief  update sensorName to sensor PDIs*/
    void updateSensorName(std::string name);

    /** @brief  getter of sensorNameSpace */
    std::string getSensorNameSpace()
    {
        return sensorNameSpace;
    }

    /** @brief indicate that the sensor should be included in sensorMetrics */
    bool inSensorMetrics;

    /** @brief indicate if sensor is polled in priority */
    bool isPriority;

    void removeValueIntf();

    void setRefreshed(bool r)
    {
        refreshed = r;
    }

    inline bool isRefreshed()
    {
        return refreshed;
    }

    /** @brief  The time since last getSensorReading command in usec */
    uint64_t lastUpdatedTimeStampInUsec;

    /** @brief  The refresh limit in usec */
    uint64_t refreshLimitInUsec = DEFAULT_RR_REFRESH_LIMIT_IN_MS * 1000;

    inline void setLastUpdatedTimeStamp(const uint64_t currentTimestampInUsec)
    {
        lastUpdatedTimeStampInUsec = currentTimestampInUsec;
    }

    inline bool needsUpdate(const uint64_t currentTimestampInUsec)
    {
        if (skipPolling)
        {
            return false;
        }
        const uint64_t deltaInUsec =
            currentTimestampInUsec - lastUpdatedTimeStampInUsec;
        if (updateTime > deltaInUsec)
        {
            return false;
        }

        return (
            isPriority // We don't want to throttle if it's a priority sensor
            || (deltaInUsec > refreshLimitInUsec));
    }

    /** @brief  A container to store OemIntf, it allows us to add additional OEM
     * sdbusplus object as extra attribute */
    std::vector<std::shared_ptr<platform_mc::OemIntf>> oemIntfs;

    std::unique_ptr<ValueIntf> valueIntf = nullptr;
    std::unique_ptr<ThresholdWarningIntf> thresholdWarningIntf = nullptr;
    std::unique_ptr<ThresholdCriticalIntf> thresholdCriticalIntf = nullptr;
    std::unique_ptr<ThresholdFatalIntf> thresholdFatalIntf = nullptr;
    std::unique_ptr<AvailabilityIntf> availabilityIntf = nullptr;
    std::unique_ptr<OperationalStatusIntf> operationalStatusIntf = nullptr;
    std::unique_ptr<AssociationDefinitionsInft> associationDefinitionsIntf =
        nullptr;
    std::unique_ptr<InventoryDecoratorAreaIntf> inventoryDecoratorAreaIntf =
        nullptr;

  private:
    /**
     * @brief Check sensor reading if any threshold has been crossed and update
     * Threshold interfaces accordingly
     */
    void updateThresholds();

    /** @brief Amount of hysteresis associated with the sensor thresholds */
    double hysteresis{};

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

    /** @brief  sensorName */
    std::string sensorName;

    /** @brief  sensorNameSpace */
    std::string sensorNameSpace;

    /** @brief indicate if sensor is refreshed or not*/
    bool refreshed = false;

    /** @brief unit of the sensor reading */
    SensorUnit sensorUnit;

    /** @brief does sensor have valid value interface */
    bool hasValueIntf;

    /** @brief sensor upper value range */
    double maxValue;

    /** @brief sensor lower value range */
    double minValue;

    /** @brief flag to skip polling */
    bool skipPolling;
};
} // namespace platform_mc
} // namespace pldm
