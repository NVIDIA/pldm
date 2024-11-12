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
#include "numeric_sensor.hpp"

#include "libpldm/platform.h"

#include "common/utils.hpp"

#include <phosphor-logging/lg2.hpp>
#include <tal.hpp>

#include <limits>
#include <regex>

namespace pldm
{
namespace platform_mc
{

NumericSensor::NumericSensor(const tid_t tid, const bool sensorDisabled,
                             std::shared_ptr<pldm_numeric_sensor_value_pdr> pdr,
                             std::string& sensorName,
                             std::string& associationPath) :
    tid(tid),
    sensorId(pdr->sensor_id),
    entityInfo(ContainerID(pdr->container_id), EntityType(pdr->entity_type),
               EntityInstance(pdr->entity_instance_num)),
    inSensorMetrics(false), isPriority(false), baseUnit(pdr->base_unit),
    sensorName(sensorName)
{
    sensorUnit = SensorUnit::DegreesC;
    hasValueIntf = true;
    skipPolling = false;
    pollingIndicator = POLLING_METHOD_INDICATOR_PLDM_TYPE_TWO;
    switch (baseUnit)
    {
        case PLDM_SENSOR_UNIT_DEGRESS_C:
            sensorNameSpace = "/xyz/openbmc_project/sensors/temperature/";
            sensorUnit = SensorUnit::DegreesC;
            break;
        case PLDM_SENSOR_UNIT_VOLTS:
            sensorNameSpace = "/xyz/openbmc_project/sensors/voltage/";
            sensorUnit = SensorUnit::Volts;
            break;
        case PLDM_SENSOR_UNIT_AMPS:
            sensorNameSpace = "/xyz/openbmc_project/sensors/current/";
            sensorUnit = SensorUnit::Amperes;
            break;
        case PLDM_SENSOR_UNIT_RPM:
            sensorNameSpace = "/xyz/openbmc_project/sensors/fan_pwm/";
            sensorUnit = SensorUnit::RPMS;
            break;
        case PLDM_SENSOR_UNIT_WATTS:
            sensorNameSpace = "/xyz/openbmc_project/sensors/power/";
            sensorUnit = SensorUnit::Watts;
            break;
        case PLDM_SENSOR_UNIT_JOULES:
            sensorNameSpace = "/xyz/openbmc_project/sensors/energy/";
            sensorUnit = SensorUnit::Joules;
            break;
        case PLDM_SENSOR_UNIT_HERTZ:
            sensorNameSpace = "/xyz/openbmc_project/sensors/frequency/";
            sensorUnit = SensorUnit::Hertz;
            break;
        case PLDM_SENSOR_UNIT_PERCENTAGE:
            sensorNameSpace = "/xyz/openbmc_project/sensors/utilization/";
            sensorUnit = SensorUnit::Percent;
            break;
        case PLDM_SENSOR_UNIT_COUNTS:
            sensorNameSpace = "/xyz/openbmc_project/sensors/counter/";
            sensorUnit = SensorUnit::Counts;
            break;
        default:
            hasValueIntf = false;
            sensorNameSpace = "/xyz/openbmc_project/sensors/none/";
            lg2::info(
                "SensorID={SENSORID}, baseUnit({BASEUNIT}) is not supported by value PDI.",
                "SENSORID", sensorId, "BASEUNIT", pdr->base_unit);
            break;
    }

    path = sensorNameSpace + sensorName;
    path = std::regex_replace(path, std::regex("[^a-zA-Z0-9_/]+"), "_");

    auto& bus = pldm::utils::DBusHandler::getBus();
    associationDefinitionsIntf =
        std::make_unique<AssociationDefinitionsInft>(bus, path.c_str());
    associationDefinitionsIntf->associations(
        {{"chassis", "all_sensors", associationPath.c_str()}});

    maxValue = std::numeric_limits<double>::quiet_NaN();
    minValue = std::numeric_limits<double>::quiet_NaN();

    switch (pdr->sensor_data_size)
    {
        case PLDM_SENSOR_DATA_SIZE_UINT8:
            maxValue = pdr->max_readable.value_u8;
            minValue = pdr->min_readable.value_u8;
            hysteresis = pdr->hysteresis.value_u8;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT8:
            maxValue = pdr->max_readable.value_s8;
            minValue = pdr->min_readable.value_s8;
            hysteresis = pdr->hysteresis.value_s8;
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT16:
            maxValue = pdr->max_readable.value_u16;
            minValue = pdr->min_readable.value_u16;
            hysteresis = pdr->hysteresis.value_u16;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT16:
            maxValue = pdr->max_readable.value_s16;
            minValue = pdr->min_readable.value_s16;
            hysteresis = pdr->hysteresis.value_s16;
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT32:
            maxValue = pdr->max_readable.value_u32;
            minValue = pdr->min_readable.value_u32;
            hysteresis = pdr->hysteresis.value_u32;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT32:
            maxValue = pdr->max_readable.value_s32;
            minValue = pdr->min_readable.value_s32;
            hysteresis = pdr->hysteresis.value_s32;
            break;
    }

    bool hasWarningThresholds = false;
    bool hasCriticalThresholds = false;
    bool hasFatalThresholds = false;
    double fatalHigh = std::numeric_limits<double>::quiet_NaN();
    double fatalLow = std::numeric_limits<double>::quiet_NaN();
    double criticalHigh = std::numeric_limits<double>::quiet_NaN();
    double criticalLow = std::numeric_limits<double>::quiet_NaN();
    double warningHigh = std::numeric_limits<double>::quiet_NaN();
    double warningLow = std::numeric_limits<double>::quiet_NaN();

    if (pdr->supported_thresholds.bits.bit0)
    {
        hasWarningThresholds = true;
        switch (pdr->range_field_format)
        {
            case PLDM_RANGE_FIELD_FORMAT_UINT8:
                warningHigh = pdr->warning_high.value_u8;
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT8:
                warningHigh = pdr->warning_high.value_s8;
                break;
            case PLDM_RANGE_FIELD_FORMAT_UINT16:
                warningHigh = pdr->warning_high.value_u16;
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT16:
                warningHigh = pdr->warning_high.value_s16;
                break;
            case PLDM_RANGE_FIELD_FORMAT_UINT32:
                warningHigh = pdr->warning_high.value_u32;
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT32:
                warningHigh = pdr->warning_high.value_s32;
                break;
            case PLDM_RANGE_FIELD_FORMAT_REAL32:
                warningHigh = pdr->warning_high.value_f32;
                break;
        }
    }

    if (pdr->supported_thresholds.bits.bit3)
    {
        hasWarningThresholds = true;
        switch (pdr->range_field_format)
        {
            case PLDM_RANGE_FIELD_FORMAT_UINT8:
                warningLow = pdr->warning_low.value_u8;
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT8:
                warningLow = pdr->warning_low.value_u8;
                break;
            case PLDM_RANGE_FIELD_FORMAT_UINT16:
                warningLow = pdr->warning_low.value_u16;
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT16:
                warningLow = pdr->warning_low.value_s16;
                break;
            case PLDM_RANGE_FIELD_FORMAT_UINT32:
                warningLow = pdr->warning_low.value_u32;
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT32:
                warningLow = pdr->warning_low.value_s32;
                break;
            case PLDM_RANGE_FIELD_FORMAT_REAL32:
                warningLow = pdr->warning_low.value_f32;
                break;
        }
    }

    if (pdr->range_field_support.bits.bit3 &&
        pdr->supported_thresholds.bits.bit1)
    {
        hasCriticalThresholds = true;
        switch (pdr->range_field_format)
        {
            case PLDM_RANGE_FIELD_FORMAT_UINT8:
                criticalHigh = pdr->critical_high.value_u8;
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT8:
                criticalHigh = pdr->critical_high.value_s8;
                break;
            case PLDM_RANGE_FIELD_FORMAT_UINT16:
                criticalHigh = pdr->critical_high.value_u16;
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT16:
                criticalHigh = pdr->critical_high.value_s16;
                break;
            case PLDM_RANGE_FIELD_FORMAT_UINT32:
                criticalHigh = pdr->critical_high.value_u32;
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT32:
                criticalHigh = pdr->critical_high.value_s32;
                break;
            case PLDM_RANGE_FIELD_FORMAT_REAL32:
                criticalHigh = pdr->critical_high.value_f32;
                break;
        }
    }

    if (pdr->range_field_support.bits.bit4 &&
        pdr->supported_thresholds.bits.bit4)
    {
        hasCriticalThresholds = true;
        switch (pdr->range_field_format)
        {
            case PLDM_RANGE_FIELD_FORMAT_UINT8:
                criticalLow = pdr->critical_low.value_u8;
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT8:
                criticalLow = pdr->critical_low.value_s8;
                break;
            case PLDM_RANGE_FIELD_FORMAT_UINT16:
                criticalLow = pdr->critical_low.value_u16;
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT16:
                criticalLow = pdr->critical_low.value_s16;
                break;
            case PLDM_RANGE_FIELD_FORMAT_UINT32:
                criticalLow = pdr->critical_low.value_u32;
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT32:
                criticalLow = pdr->critical_low.value_s32;
                break;
            case PLDM_RANGE_FIELD_FORMAT_REAL32:
                criticalLow = pdr->critical_low.value_f32;
                break;
        }
    }

    if (pdr->range_field_support.bits.bit5 &&
        pdr->supported_thresholds.bits.bit2)
    {
        hasFatalThresholds = true;
        switch (pdr->range_field_format)
        {
            case PLDM_RANGE_FIELD_FORMAT_UINT8:
                fatalHigh = pdr->fatal_high.value_u8;
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT8:
                fatalHigh = pdr->fatal_high.value_s8;
                break;
            case PLDM_RANGE_FIELD_FORMAT_UINT16:
                fatalHigh = pdr->fatal_high.value_u16;
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT16:
                fatalHigh = pdr->fatal_high.value_s16;
                break;
            case PLDM_RANGE_FIELD_FORMAT_UINT32:
                fatalHigh = pdr->fatal_high.value_u32;
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT32:
                fatalHigh = pdr->fatal_high.value_s32;
                break;
            case PLDM_RANGE_FIELD_FORMAT_REAL32:
                fatalHigh = pdr->fatal_high.value_f32;
                break;
        }
    }

    if (pdr->range_field_support.bits.bit6 &&
        pdr->supported_thresholds.bits.bit5)
    {
        hasFatalThresholds = true;
        switch (pdr->range_field_format)
        {
            case PLDM_RANGE_FIELD_FORMAT_UINT8:
                fatalLow = pdr->fatal_low.value_u8;
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT8:
                fatalLow = pdr->fatal_low.value_s8;
                break;
            case PLDM_RANGE_FIELD_FORMAT_UINT16:
                fatalLow = pdr->fatal_low.value_u16;
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT16:
                fatalLow = pdr->fatal_low.value_s16;
                break;
            case PLDM_RANGE_FIELD_FORMAT_UINT32:
                fatalLow = pdr->fatal_low.value_u32;
                break;
            case PLDM_RANGE_FIELD_FORMAT_SINT32:
                fatalLow = pdr->fatal_low.value_s32;
                break;
            case PLDM_RANGE_FIELD_FORMAT_REAL32:
                fatalLow = pdr->fatal_low.value_f32;
                break;
        }
    }

    resolution = pdr->resolution;
    offset = pdr->offset;
    baseUnitModifier = pdr->unit_modifier;

    updateTime = std::numeric_limits<uint64_t>::max();
    if (!std::isnan(pdr->update_interval))
    {
        updateTime = pdr->update_interval * 1000000;
    }

    if (hasValueIntf)
    {
        valueIntf = std::make_unique<ValueIntf>(bus, path.c_str());
        maxValue = unitModifier(conversionFormula(maxValue));
        valueIntf->maxValue(maxValue);
        minValue = unitModifier(conversionFormula(minValue));
        valueIntf->minValue(minValue);
        valueIntf->unit(sensorUnit);
    }

    hysteresis = unitModifier(conversionFormula(hysteresis));

    availabilityIntf = std::make_unique<AvailabilityIntf>(bus, path.c_str());
    availabilityIntf->available(true);

    operationalStatusIntf =
        std::make_unique<OperationalStatusIntf>(bus, path.c_str());
    operationalStatusIntf->functional(!sensorDisabled);

    if (hasWarningThresholds)
    {
        thresholdWarningIntf =
            std::make_unique<ThresholdWarningIntf>(bus, path.c_str());
        thresholdWarningIntf->warningHigh(unitModifier(warningHigh));
        thresholdWarningIntf->warningLow(unitModifier(warningLow));
    }

    if (hasCriticalThresholds)
    {
        thresholdCriticalIntf =
            std::make_unique<ThresholdCriticalIntf>(bus, path.c_str());
        thresholdCriticalIntf->criticalHigh(unitModifier(criticalHigh));
        thresholdCriticalIntf->criticalLow(unitModifier(criticalLow));
    }

    if (hasFatalThresholds)
    {
        thresholdFatalIntf =
            std::make_unique<ThresholdFatalIntf>(bus, path.c_str());
        thresholdFatalIntf->hardShutdownHigh(unitModifier(fatalHigh));
        thresholdFatalIntf->hardShutdownLow(unitModifier(fatalLow));
    }

    inventoryDecoratorAreaIntf =
        std::make_unique<InventoryDecoratorAreaIntf>(bus, path.c_str());
    inventoryDecoratorAreaIntf->physicalContext(
        PhysicalContextType::SystemBoard);
}

#ifdef OEM_NVIDIA
NumericSensor::NumericSensor(
    const tid_t tid, const bool sensorDisabled,
    std::shared_ptr<pldm_oem_energycount_numeric_sensor_value_pdr> pdr,
    std::string& sensorName, std::string& associationPath,
    uint8_t oemIndicator) :
    tid(tid),
    sensorId(pdr->sensor_id),
    entityInfo(ContainerID(pdr->container_id), EntityType(pdr->entity_type),
               EntityInstance(pdr->entity_instance_num)),
    inSensorMetrics(false), isPriority(false), baseUnit(pdr->base_unit),
    pollingIndicator(oemIndicator), sensorName(sensorName)
{
    sensorUnit = SensorUnit::DegreesC;
    hasValueIntf = true;
    switch (baseUnit)
    {
        case PLDM_SENSOR_UNIT_DEGRESS_C:
            sensorNameSpace = "/xyz/openbmc_project/sensors/temperature/";
            sensorUnit = SensorUnit::DegreesC;
            break;
        case PLDM_SENSOR_UNIT_VOLTS:
            sensorNameSpace = "/xyz/openbmc_project/sensors/voltage/";
            sensorUnit = SensorUnit::Volts;
            break;
        case PLDM_SENSOR_UNIT_AMPS:
            sensorNameSpace = "/xyz/openbmc_project/sensors/current/";
            sensorUnit = SensorUnit::Amperes;
            break;
        case PLDM_SENSOR_UNIT_RPM:
            sensorNameSpace = "/xyz/openbmc_project/sensors/fan_pwm/";
            sensorUnit = SensorUnit::RPMS;
            break;
        case PLDM_SENSOR_UNIT_WATTS:
            sensorNameSpace = "/xyz/openbmc_project/sensors/power/";
            sensorUnit = SensorUnit::Watts;
            break;
        case PLDM_SENSOR_UNIT_JOULES:
            sensorNameSpace = "/xyz/openbmc_project/sensors/energy/";
            sensorUnit = SensorUnit::Joules;
            break;
        case PLDM_SENSOR_UNIT_HERTZ:
            sensorNameSpace = "/xyz/openbmc_project/sensors/frequency/";
            sensorUnit = SensorUnit::Hertz;
            break;
        case PLDM_SENSOR_UNIT_PERCENTAGE:
            sensorNameSpace = "/xyz/openbmc_project/sensors/utilization/";
            sensorUnit = SensorUnit::Percent;
            break;
        case PLDM_SENSOR_UNIT_COUNTS:
            sensorNameSpace = "/xyz/openbmc_project/sensors/counter/";
            sensorUnit = SensorUnit::Counts;
            break;
        default:
            hasValueIntf = false;
            sensorNameSpace = "/xyz/openbmc_project/sensors/none/";
            lg2::error(
                "SensorID={SENSORID}, baseUnit({BASEUNIT}) is not supported by value PDI.",
                "SENSORID", sensorId, "BASEUNIT", pdr->base_unit);
            break;
    }

    path = sensorNameSpace + sensorName;
    path = std::regex_replace(path, std::regex("[^a-zA-Z0-9_/]+"), "_");

    auto& bus = pldm::utils::DBusHandler::getBus();
    associationDefinitionsIntf =
        std::make_unique<AssociationDefinitionsInft>(bus, path.c_str());
    associationDefinitionsIntf->associations(
        {{"chassis", "all_sensors", associationPath.c_str()}});

    maxValue = std::numeric_limits<double>::quiet_NaN();
    minValue = std::numeric_limits<double>::quiet_NaN();

    switch (pdr->sensor_data_size)
    {
        case PLDM_SENSOR_DATA_SIZE_UINT8:
            maxValue = pdr->max_readable.value_u8;
            minValue = pdr->min_readable.value_u8;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT8:
            maxValue = pdr->max_readable.value_s8;
            minValue = pdr->min_readable.value_s8;
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT16:
            maxValue = pdr->max_readable.value_u16;
            minValue = pdr->min_readable.value_u16;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT16:
            maxValue = pdr->max_readable.value_s16;
            minValue = pdr->min_readable.value_s16;
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT32:
            maxValue = pdr->max_readable.value_u32;
            minValue = pdr->min_readable.value_u32;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT32:
            maxValue = pdr->max_readable.value_s32;
            minValue = pdr->min_readable.value_s32;
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT64:
            maxValue = pdr->max_readable.value_u64;
            minValue = pdr->min_readable.value_u64;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT64:
            maxValue = pdr->max_readable.value_s64;
            minValue = pdr->min_readable.value_s64;
            break;
        default:
            lg2::error(
                "SensorID={SENSORID}, sensor_data_size({SENSORDATASIZE}) is not a valid value.",
                "SENSORID", sensorId, "SENSORDATASIZE", pdr->sensor_data_size);
            break;
    }

    // resloution and offset not provided in pdr
    resolution = 1;
    offset = 0;
    baseUnitModifier = pdr->unit_modifier;

    updateTime = std::numeric_limits<uint64_t>::max();
    if (!std::isnan(pdr->update_interval))
    {
        updateTime = pdr->update_interval * 1000000;
    }

    if (hasValueIntf)
    {
        valueIntf = std::make_unique<ValueIntf>(bus, path.c_str());
        maxValue = unitModifier(conversionFormula(maxValue));
        valueIntf->maxValue(maxValue);
        minValue = unitModifier(conversionFormula(minValue));
        valueIntf->minValue(minValue);
        valueIntf->unit(sensorUnit);
    }

    availabilityIntf = std::make_unique<AvailabilityIntf>(bus, path.c_str());
    availabilityIntf->available(true);

    operationalStatusIntf =
        std::make_unique<OperationalStatusIntf>(bus, path.c_str());
    operationalStatusIntf->functional(!sensorDisabled);

    inventoryDecoratorAreaIntf =
        std::make_unique<InventoryDecoratorAreaIntf>(bus, path.c_str());
    inventoryDecoratorAreaIntf->physicalContext(
        PhysicalContextType::SystemBoard);
}
#endif

double NumericSensor::conversionFormula(double value)
{
    double convertedValue = value;
    convertedValue *= std::isnan(resolution) ? 1 : resolution;
    convertedValue += std::isnan(offset) ? 0 : offset;
    return convertedValue;
}

double NumericSensor::unitModifier(double value)
{
    return value * std::pow(10, baseUnitModifier);
}

void NumericSensor::updateReading(bool available, bool functional, double value)
{
    rawValue = value;
    availabilityIntf->available(available);
    operationalStatusIntf->functional(functional);

    if (!valueIntf)
    {
        return;
    }

    if (functional && available)
    {
        valueIntf->value(unitModifier(conversionFormula(value)));
        updateThresholds();
    }
    else
    {
        valueIntf->value(std::numeric_limits<double>::quiet_NaN());
    }

    std::string propertyName = "Value";
    std::string objPath = path;
    std::string ifaceName = valueIntf->interface;
    // TODO: update retCode for errors once error code handling defined for pldm
    uint16_t retCode = 0;

    uint64_t steadyTimeStamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    // tal telemetry update for all Numeric Sensors.
    double convertVal = valueIntf->value();
    std::vector<uint8_t> rawSmbpbiData(sizeof(double));
    std::memcpy(rawSmbpbiData.data(), &convertVal, sizeof(double));

    DbusVariantType propValue = convertVal;

    std::string endpoint{};
    auto definitions = associationDefinitionsIntf->associations();
    if (definitions.size() > 0)
    {
        endpoint = std::get<2>(definitions[0]);
        if (endpoint.size() > 0)
        {
            tal::TelemetryAggregator::updateTelemetry(
                objPath, ifaceName, propertyName, rawSmbpbiData,
                steadyTimeStamp, retCode, propValue, endpoint);
        }
    }
}

void NumericSensor::handleErrGetSensorReading()
{
    updateReading(true, false, std::numeric_limits<double>::quiet_NaN());
}

bool NumericSensor::checkThreshold(bool alarm, bool direction, double value,
                                   double threshold, double hyst)
{
    if (direction)
    {
        if (value >= threshold)
        {
            return true;
        }
        else if (value < (threshold - hyst))
        {
            return false;
        }
    }
    else
    {
        if (value <= threshold)
        {
            return true;
        }
        else if (value > (threshold + hyst))
        {
            return false;
        }
    }
    return alarm;
}

void NumericSensor::updateThresholds()
{
    auto value = getReading();

    if (thresholdWarningIntf &&
        !std::isnan(thresholdWarningIntf->warningHigh()))
    {
        auto threshold = thresholdWarningIntf->warningHigh();
        auto alarm = thresholdWarningIntf->warningAlarmHigh();
        auto newAlarm =
            checkThreshold(alarm, true, value, threshold, hysteresis);
        if (alarm != newAlarm)
        {
            thresholdWarningIntf->warningAlarmHigh(newAlarm);
            if (newAlarm)
            {
                thresholdWarningIntf->warningHighAlarmAsserted(value);
            }
            else
            {
                thresholdWarningIntf->warningHighAlarmDeasserted(value);
            }
        }
    }

    if (thresholdWarningIntf && !std::isnan(thresholdWarningIntf->warningLow()))
    {
        auto threshold = thresholdWarningIntf->warningLow();
        auto alarm = thresholdWarningIntf->warningAlarmLow();
        auto newAlarm =
            checkThreshold(alarm, false, value, threshold, hysteresis);
        if (alarm != newAlarm)
        {
            thresholdWarningIntf->warningAlarmLow(newAlarm);
            if (newAlarm)
            {
                thresholdWarningIntf->warningLowAlarmAsserted(value);
            }
            else
            {
                thresholdWarningIntf->warningLowAlarmDeasserted(value);
            }
        }
    }

    if (thresholdCriticalIntf &&
        !std::isnan(thresholdCriticalIntf->criticalHigh()))
    {
        auto threshold = thresholdCriticalIntf->criticalHigh();
        auto alarm = thresholdCriticalIntf->criticalAlarmHigh();
        auto newAlarm =
            checkThreshold(alarm, true, value, threshold, hysteresis);
        if (alarm != newAlarm)
        {
            thresholdCriticalIntf->criticalAlarmHigh(newAlarm);
            if (newAlarm)
            {
                thresholdCriticalIntf->criticalHighAlarmAsserted(value);
            }
            else
            {
                thresholdCriticalIntf->criticalHighAlarmDeasserted(value);
            }
        }
    }

    if (thresholdCriticalIntf &&
        !std::isnan(thresholdCriticalIntf->criticalLow()))
    {
        auto threshold = thresholdCriticalIntf->criticalLow();
        auto alarm = thresholdCriticalIntf->criticalAlarmLow();
        auto newAlarm =
            checkThreshold(alarm, false, value, threshold, hysteresis);
        if (alarm != newAlarm)
        {
            thresholdCriticalIntf->criticalAlarmLow(newAlarm);
            if (newAlarm)
            {
                thresholdCriticalIntf->criticalLowAlarmAsserted(value);
            }
            else
            {
                thresholdCriticalIntf->criticalLowAlarmDeasserted(value);
            }
        }
    }

    if (thresholdFatalIntf &&
        !std::isnan(thresholdFatalIntf->hardShutdownHigh()))
    {
        auto threshold = thresholdFatalIntf->hardShutdownHigh();
        auto alarm = thresholdFatalIntf->hardShutdownAlarmHigh();
        auto newAlarm =
            checkThreshold(alarm, true, value, threshold, hysteresis);
        if (alarm != newAlarm)
        {
            thresholdFatalIntf->hardShutdownAlarmHigh(newAlarm);
            if (newAlarm)
            {
                thresholdFatalIntf->hardShutdownHighAlarmAsserted(value);
            }
            else
            {
                thresholdFatalIntf->hardShutdownHighAlarmDeasserted(value);
            }
        }
    }

    if (thresholdFatalIntf &&
        !std::isnan(thresholdFatalIntf->hardShutdownLow()))
    {
        auto threshold = thresholdFatalIntf->hardShutdownLow();
        auto alarm = thresholdFatalIntf->hardShutdownAlarmLow();
        auto newAlarm =
            checkThreshold(alarm, false, value, threshold, hysteresis);
        if (alarm != newAlarm)
        {
            thresholdFatalIntf->hardShutdownAlarmLow(newAlarm);
            if (newAlarm)
            {
                thresholdFatalIntf->hardShutdownLowAlarmAsserted(value);
            }
            else
            {
                thresholdFatalIntf->hardShutdownLowAlarmDeasserted(value);
            }
        }
    }
}

void NumericSensor::updateSensorName(std::string name)
{
    if (sensorName == name)
    {
        return;
    }

    sensorName = name;
    path = sensorNameSpace + sensorName;
    path = std::regex_replace(path, std::regex("[^a-zA-Z0-9_/]+"), "_");

    auto& bus = pldm::utils::DBusHandler::getBus();

    // update new object path to D-Bus
    if (hasValueIntf)
    {
        associationDefinitionsIntf =
            std::make_unique<AssociationDefinitionsInft>(bus, path.c_str());
    }

    if (hasValueIntf)
    {
        skipPolling = false;
        valueIntf = std::make_unique<ValueIntf>(bus, path.c_str());
        valueIntf->maxValue(maxValue);
        valueIntf->minValue(minValue);
        valueIntf->unit(sensorUnit);
    }

    if (availabilityIntf)
    {
        auto available = availabilityIntf->available();
        availabilityIntf =
            std::make_unique<AvailabilityIntf>(bus, path.c_str());
        availabilityIntf->available(available);
    }

    if (operationalStatusIntf)
    {
        auto functional = operationalStatusIntf->functional();
        operationalStatusIntf =
            std::make_unique<OperationalStatusIntf>(bus, path.c_str());
        operationalStatusIntf->functional(functional);
    }

    if (thresholdWarningIntf)
    {
        auto warningHigh = thresholdWarningIntf->warningHigh();
        auto warningLow = thresholdWarningIntf->warningLow();
        thresholdWarningIntf =
            std::make_unique<ThresholdWarningIntf>(bus, path.c_str());
        thresholdWarningIntf->warningHigh(warningHigh);
        thresholdWarningIntf->warningLow(warningLow);
    }

    if (thresholdCriticalIntf)
    {
        auto criticalHigh = thresholdCriticalIntf->criticalHigh();
        auto criticalLow = thresholdCriticalIntf->criticalLow();
        thresholdCriticalIntf =
            std::make_unique<ThresholdCriticalIntf>(bus, path.c_str());
        thresholdCriticalIntf->criticalHigh(criticalHigh);
        thresholdCriticalIntf->criticalLow(criticalLow);
    }

    if (thresholdFatalIntf)
    {
        auto fatalHigh = thresholdFatalIntf->hardShutdownHigh();
        auto fatalLow = thresholdFatalIntf->hardShutdownLow();
        thresholdFatalIntf =
            std::make_unique<ThresholdFatalIntf>(bus, path.c_str());
        thresholdFatalIntf->hardShutdownHigh(fatalHigh);
        thresholdFatalIntf->hardShutdownLow(fatalLow);
    }

    if (inventoryDecoratorAreaIntf)
    {
        auto physicalContext = inventoryDecoratorAreaIntf->physicalContext();
        inventoryDecoratorAreaIntf =
            std::make_unique<InventoryDecoratorAreaIntf>(bus, path.c_str());
        inventoryDecoratorAreaIntf->physicalContext(physicalContext);
    }
}

void NumericSensor::removeValueIntf()
{
    if (hasValueIntf)
    {
        skipPolling = true;
        valueIntf = nullptr;
    }
    associationDefinitionsIntf = nullptr;
}

} // namespace platform_mc
} // namespace pldm
