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

#include "libpldm/entity.h"

#include "common/instance_id.hpp"
#include "platform-mc/numeric_sensor.hpp"
#include "platform-mc/terminus.hpp"
#include "platform-mc/terminus_manager.hpp"
#include "test/test_instance_id.hpp"

#include <array>
#include <cmath>

#include <gtest/gtest.h>

using namespace pldm::platform_mc;

class NumericSensorTest : public testing::Test
{
  protected:
    NumericSensorTest() :
        bus(pldm::utils::DBusHandler::getBus()),
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, instanceIdDb, termini, 0x8, nullptr)
    {}

    sdbusplus::bus::bus& bus;
    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    pldm::platform_mc::TerminusManager terminusManager;
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
};

static std::shared_ptr<pldm_numeric_sensor_value_pdr> makeNumericSensorValuePdr(
    uint8_t sensorDataSize, uint8_t rangeFieldFormat,
    uint8_t baseUnit = PLDM_SENSOR_UNIT_DEGRESS_C, int8_t unitModifier = 0,
    bool enableThresholds = true)
{
    auto pdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    pdr->sensor_id = 0x21;
    pdr->entity_type = PLDM_ENTITY_POWER_SUPPLY;
    pdr->entity_instance_num = 1;
    pdr->container_id = 1;
    pdr->base_unit = baseUnit;
    pdr->unit_modifier = unitModifier;
    pdr->sensor_data_size = sensorDataSize;
    pdr->resolution = 1.25f;
    pdr->offset = 2.0f;
    pdr->update_interval = 1.0f;

    switch (sensorDataSize)
    {
        case PLDM_SENSOR_DATA_SIZE_UINT8:
            pdr->max_readable.value_u8 = 100;
            pdr->min_readable.value_u8 = 1;
            pdr->hysteresis.value_u8 = 3;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT8:
            pdr->max_readable.value_s8 = 100;
            pdr->min_readable.value_s8 = -100;
            pdr->hysteresis.value_s8 = 3;
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT16:
            pdr->max_readable.value_u16 = 1000;
            pdr->min_readable.value_u16 = 10;
            pdr->hysteresis.value_u16 = 3;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT16:
            pdr->max_readable.value_s16 = 1000;
            pdr->min_readable.value_s16 = -1000;
            pdr->hysteresis.value_s16 = 3;
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT32:
            pdr->max_readable.value_u32 = 100000;
            pdr->min_readable.value_u32 = 100;
            pdr->hysteresis.value_u32 = 3;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT32:
            pdr->max_readable.value_s32 = 100000;
            pdr->min_readable.value_s32 = -100000;
            pdr->hysteresis.value_s32 = 3;
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT64:
            pdr->max_readable.value_u64 = 1000000;
            pdr->min_readable.value_u64 = 1000;
            pdr->hysteresis.value_u64 = 3;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT64:
        default:
            pdr->max_readable.value_s64 = 1000000;
            pdr->min_readable.value_s64 = -1000000;
            pdr->hysteresis.value_s64 = 3;
            break;
    }

    pdr->range_field_format = rangeFieldFormat;
    if (enableThresholds)
    {
        pdr->supported_thresholds.byte = 0x3F;
        pdr->range_field_support.byte = 0x78;
    }
    else
    {
        pdr->supported_thresholds.byte = 0;
        pdr->range_field_support.byte = 0;
    }

    switch (rangeFieldFormat)
    {
        case PLDM_RANGE_FIELD_FORMAT_UINT8:
            pdr->warning_high.value_u8 = 80;
            pdr->warning_low.value_u8 = 20;
            pdr->critical_high.value_u8 = 90;
            pdr->critical_low.value_u8 = 10;
            pdr->fatal_high.value_u8 = 100;
            pdr->fatal_low.value_u8 = 5;
            break;
        case PLDM_RANGE_FIELD_FORMAT_SINT8:
            pdr->warning_high.value_s8 = 40;
            pdr->warning_low.value_s8 = -40;
            pdr->critical_high.value_s8 = 50;
            pdr->critical_low.value_s8 = -50;
            pdr->fatal_high.value_s8 = 60;
            pdr->fatal_low.value_s8 = -60;
            break;
        case PLDM_RANGE_FIELD_FORMAT_UINT16:
            pdr->warning_high.value_u16 = 800;
            pdr->warning_low.value_u16 = 200;
            pdr->critical_high.value_u16 = 900;
            pdr->critical_low.value_u16 = 100;
            pdr->fatal_high.value_u16 = 1000;
            pdr->fatal_low.value_u16 = 50;
            break;
        case PLDM_RANGE_FIELD_FORMAT_SINT16:
            pdr->warning_high.value_s16 = 400;
            pdr->warning_low.value_s16 = -400;
            pdr->critical_high.value_s16 = 500;
            pdr->critical_low.value_s16 = -500;
            pdr->fatal_high.value_s16 = 600;
            pdr->fatal_low.value_s16 = -600;
            break;
        case PLDM_RANGE_FIELD_FORMAT_UINT32:
            pdr->warning_high.value_u32 = 80000;
            pdr->warning_low.value_u32 = 20000;
            pdr->critical_high.value_u32 = 90000;
            pdr->critical_low.value_u32 = 10000;
            pdr->fatal_high.value_u32 = 100000;
            pdr->fatal_low.value_u32 = 5000;
            break;
        case PLDM_RANGE_FIELD_FORMAT_SINT32:
            pdr->warning_high.value_s32 = 40000;
            pdr->warning_low.value_s32 = -40000;
            pdr->critical_high.value_s32 = 50000;
            pdr->critical_low.value_s32 = -50000;
            pdr->fatal_high.value_s32 = 60000;
            pdr->fatal_low.value_s32 = -60000;
            break;
        case PLDM_RANGE_FIELD_FORMAT_REAL32:
            pdr->warning_high.value_f32 = 40.5f;
            pdr->warning_low.value_f32 = -40.5f;
            pdr->critical_high.value_f32 = 50.5f;
            pdr->critical_low.value_f32 = -50.5f;
            pdr->fatal_high.value_f32 = 60.5f;
            pdr->fatal_low.value_f32 = -60.5f;
            break;
        case PLDM_RANGE_FIELD_FORMAT_UINT64:
            pdr->warning_high.value_u64 = 800000;
            pdr->warning_low.value_u64 = 200000;
            pdr->critical_high.value_u64 = 900000;
            pdr->critical_low.value_u64 = 100000;
            pdr->fatal_high.value_u64 = 1000000;
            pdr->fatal_low.value_u64 = 50000;
            break;
        case PLDM_RANGE_FIELD_FORMAT_SINT64:
        default:
            pdr->warning_high.value_s64 = 400000;
            pdr->warning_low.value_s64 = -400000;
            pdr->critical_high.value_s64 = 500000;
            pdr->critical_low.value_s64 = -500000;
            pdr->fatal_high.value_s64 = 600000;
            pdr->fatal_low.value_s64 = -600000;
            break;
    }

    return pdr;
}

TEST_F(NumericSensorTest, conversionFormula)
{
    std::vector<uint8_t> pdr1{
        0x1,
        0x0,
        0x0,
        0x0,                     // record handle
        0x1,                     // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR, // PDRType
        0x0,
        0x0,                     // recordChangeNumber
        PLDM_PDR_NUMERIC_SENSOR_PDR_FIXED_LENGTH +
            PLDM_PDR_NUMERIC_SENSOR_PDR_VARIED_SENSOR_DATA_SIZE_MIN_LENGTH +
            PLDM_PDR_NUMERIC_SENSOR_PDR_VARIED_RANGE_FIELD_MIN_LENGTH,
        0,                             // dataLength
        0,
        0,                             // PLDMTerminusHandle
        0x1,
        0x0,                           // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                             // entityType=Power Supply(120)
        1,
        0,                             // entityInstanceNumber
        0x1,
        0x0,                           // containerID=1
        PLDM_NO_INIT,                  // sensorInit
        false,                         // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,    // baseUint(2)=degrees C
        1,                             // unitModifier = 1
        0,                             // rateUnit
        0,                             // baseOEMUnitHandle
        0,                             // auxUnit
        0,                             // auxUnitModifier
        0,                             // auxRateUnit
        0,                             // rel
        0,                             // auxOEMUnitHandle
        true,                          // isLinear
        PLDM_RANGE_FIELD_FORMAT_SINT8, // sensorDataSize
        0,
        0,
        0xc0,
        0x3f, // resolution=1.5
        0,
        0,
        0x80,
        0x3f, // offset=1.0
        0,
        0,    // accuracy
        0,    // plusTolerance
        0,    // minusTolerance
        2,    // hysteresis
        0,    // supportedThresholds
        0,    // thresholdAndHysteresisVolatility
        0,
        0,
        0x80,
        0x3f, // stateTransistionInterval=1.0
        0,
        0,
        0x80,
        0x3f,                          // updateInverval=1.0
        255,                           // maxReadable
        0,                             // minReadable
        PLDM_RANGE_FIELD_FORMAT_UINT8, // rangeFieldFormat
        0,                             // rangeFieldsupport
        0,                             // nominalValue
        0,                             // normalMax
        0,                             // normalMin
        0,                             // warningHigh
        0,                             // warningLow
        0,                             // criticalHigh
        0,                             // criticalLow
        0,                             // fatalHigh
        0                              // fatalLow
    };

    auto numericSensorPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    std::printf("pdr size=%ld\n", pdr1.size());
    auto rc = decode_numeric_sensor_pdr_data(pdr1.data(), pdr1.size(),
                                             numericSensorPdr.get());
    EXPECT_EQ(rc, PLDM_SUCCESS);

    std::string sensorName{"test1"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventroy/Item/Board/PLDM_device_1"};
    NumericSensor sensor(0x01, true, numericSensorPdr, sensorName,
                         inventoryPath, nullptr);
    double reading = 40.0;
    double convertedValue = 0;
    convertedValue = sensor.conversionFormula(reading);
    convertedValue = sensor.unitModifier(convertedValue);

    // (40*1.5 + 1.0 ) * 10^1 = 610
    EXPECT_EQ(610, convertedValue);
}

TEST_F(NumericSensorTest, checkThreshold)
{
    std::vector<uint8_t> pdr1{
        0x1,
        0x0,
        0x0,
        0x0,                     // record handle
        0x1,                     // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR, // PDRType
        0x0,
        0x0,                     // recordChangeNumber
        PLDM_PDR_NUMERIC_SENSOR_PDR_FIXED_LENGTH +
            PLDM_PDR_NUMERIC_SENSOR_PDR_VARIED_SENSOR_DATA_SIZE_MIN_LENGTH +
            PLDM_PDR_NUMERIC_SENSOR_PDR_VARIED_RANGE_FIELD_MIN_LENGTH,
        0,                             // dataLength
        0,
        0,                             // PLDMTerminusHandle
        0x1,
        0x0,                           // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                             // entityType=Power Supply(120)
        1,
        0,                             // entityInstanceNumber
        0x1,
        0x0,                           // containerID=1
        PLDM_NO_INIT,                  // sensorInit
        false,                         // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,    // baseUint(2)=degrees C
        1,                             // unitModifier = 1
        0,                             // rateUnit
        0,                             // baseOEMUnitHandle
        0,                             // auxUnit
        0,                             // auxUnitModifier
        0,                             // auxRateUnit
        0,                             // rel
        0,                             // auxOEMUnitHandle
        true,                          // isLinear
        PLDM_RANGE_FIELD_FORMAT_SINT8, // sensorDataSize
        0,
        0,
        0xc0,
        0x3f, // resolution=1.5
        0,
        0,
        0x80,
        0x3f, // offset=1.0
        0,
        0,    // accuracy
        0,    // plusTolerance
        0,    // minusTolerance
        2,    // hysteresis
        0,    // supportedThresholds
        0,    // thresholdAndHysteresisVolatility
        0,
        0,
        0x80,
        0x3f, // stateTransistionInterval=1.0
        0,
        0,
        0x80,
        0x3f,                          // updateInverval=1.0
        255,                           // maxReadable
        0,                             // minReadable
        PLDM_RANGE_FIELD_FORMAT_UINT8, // rangeFieldFormat
        0,                             // rangeFieldsupport
        0,                             // nominalValue
        0,                             // normalMax
        0,                             // normalMin
        0,                             // warningHigh
        0,                             // warningLow
        0,                             // criticalHigh
        0,                             // criticalLow
        0,                             // fatalHigh
        0                              // fatalLow
    };

    auto numericSensorPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    auto rc = decode_numeric_sensor_pdr_data(pdr1.data(), pdr1.size(),
                                             numericSensorPdr.get());
    EXPECT_EQ(rc, PLDM_SUCCESS);
    std::string sensorName{"test1"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventroy/Item/Board/PLDM_device_1"};
    pldm::platform_mc::NumericSensor sensor(0x01, true, numericSensorPdr,
                                            sensorName, inventoryPath, nullptr);

    bool highAlarm = false;
    bool lowAlarm = false;
    double highThreshold = 40;
    double lowThreshold = 30;
    double hysteresis = 2;

    // reading     35->40->45->38->35->30->25->32->35
    // highAlarm    F->T ->T ->T ->F ->F ->F -> F-> F
    // lowAlarm     F->F ->F ->F ->F ->T ->T -> T ->F
    double reading = 35;
    highAlarm = sensor.checkThreshold(highAlarm, true, reading, highThreshold,
                                      hysteresis);
    EXPECT_EQ(false, highAlarm);
    lowAlarm = sensor.checkThreshold(lowAlarm, false, reading, lowThreshold,
                                     hysteresis);
    EXPECT_EQ(false, lowAlarm);

    reading = 40;
    highAlarm = sensor.checkThreshold(highAlarm, true, reading, highThreshold,
                                      hysteresis);
    EXPECT_EQ(true, highAlarm);
    lowAlarm = sensor.checkThreshold(lowAlarm, false, reading, lowThreshold,
                                     hysteresis);
    EXPECT_EQ(false, lowAlarm);

    reading = 45;
    highAlarm = sensor.checkThreshold(highAlarm, true, reading, highThreshold,
                                      hysteresis);
    EXPECT_EQ(true, highAlarm);
    lowAlarm = sensor.checkThreshold(lowAlarm, false, reading, lowThreshold,
                                     hysteresis);
    EXPECT_EQ(false, lowAlarm);

    reading = 38;
    highAlarm = sensor.checkThreshold(highAlarm, true, reading, highThreshold,
                                      hysteresis);
    EXPECT_EQ(true, highAlarm);
    lowAlarm = sensor.checkThreshold(lowAlarm, false, reading, lowThreshold,
                                     hysteresis);
    EXPECT_EQ(false, lowAlarm);

    reading = 35;
    highAlarm = sensor.checkThreshold(highAlarm, true, reading, highThreshold,
                                      hysteresis);
    EXPECT_EQ(false, highAlarm);
    lowAlarm = sensor.checkThreshold(lowAlarm, false, reading, lowThreshold,
                                     hysteresis);
    EXPECT_EQ(false, lowAlarm);

    reading = 30;
    highAlarm = sensor.checkThreshold(highAlarm, true, reading, highThreshold,
                                      hysteresis);
    EXPECT_EQ(false, highAlarm);
    lowAlarm = sensor.checkThreshold(lowAlarm, false, reading, lowThreshold,
                                     hysteresis);
    EXPECT_EQ(true, lowAlarm);

    reading = 25;
    highAlarm = sensor.checkThreshold(highAlarm, true, reading, highThreshold,
                                      hysteresis);
    EXPECT_EQ(false, highAlarm);
    lowAlarm = sensor.checkThreshold(lowAlarm, false, reading, lowThreshold,
                                     hysteresis);
    EXPECT_EQ(true, lowAlarm);

    reading = 32;
    highAlarm = sensor.checkThreshold(highAlarm, true, reading, highThreshold,
                                      hysteresis);
    EXPECT_EQ(false, highAlarm);
    lowAlarm = sensor.checkThreshold(lowAlarm, false, reading, lowThreshold,
                                     hysteresis);
    EXPECT_EQ(true, lowAlarm);

    reading = 35;
    highAlarm = sensor.checkThreshold(highAlarm, true, reading, highThreshold,
                                      hysteresis);
    EXPECT_EQ(false, highAlarm);
    lowAlarm = sensor.checkThreshold(lowAlarm, false, reading, lowThreshold,
                                     hysteresis);
    EXPECT_EQ(false, lowAlarm);
}

TEST_F(NumericSensorTest, MemoryPageRetirementSensor)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    // Same PDR layout as checkThreshold/conversionFormula: LE record handle,
    // dataLength = MIN_LENGTH (69), body 61 bytes so decode accepts it.
    std::vector<uint8_t> pdr1{
        0x1,
        0x0,
        0x0,
        0x0,                     // record handle (little-endian)
        0x1,                     // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR, // PDRType
        0x0,
        0x0,                     // recordChangeNumber
        PLDM_PDR_NUMERIC_SENSOR_PDR_FIXED_LENGTH +
            PLDM_PDR_NUMERIC_SENSOR_PDR_VARIED_SENSOR_DATA_SIZE_MIN_LENGTH +
            PLDM_PDR_NUMERIC_SENSOR_PDR_VARIED_RANGE_FIELD_MIN_LENGTH,
        0,                           // dataLength
        0,
        0,                           // PLDMTerminusHandle
        0x1,
        0x0,                         // sensorID=1
        PLDM_ENTITY_MEMORY_CONTROLLER,
        0,                           // entityType=Memory Controller(143)
        1,
        0,                           // entityInstanceNumber
        0x1,
        0x0,                         // containerID=1
        PLDM_NO_INIT,                // sensorInit
        false,                       // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_COUNTS,     // baseUint(67)=counts
        0,                           // unitModifier = 0
        0,                           // rateUnit
        0,                           // baseOEMUnitHandle
        0,                           // auxUnit
        0,                           // auxUnitModifier
        0,                           // auxRateUnit
        0,                           // rel
        0,                           // auxOEMUnitHandle
        true,                        // isLinear
        PLDM_SENSOR_DATA_SIZE_UINT8, // sensorDataSize
        0,
        0,
        0x80,
        0x3f, // resolution=1.0
        0,
        0,
        0,
        0, // offset=0
        0,
        0, // accuracy
        0, // plusTolerance
        0, // minusTolerance
        2, // hysteresis
        0, // supportedThresholds
        0, // thresholdAndHysteresisVolatility
        0,
        0,
        0x80,
        0x3f, // stateTransistionInterval=1.0
        0,
        0,
        0x80,
        0x3f,                          // updateInverval=1.0
        255,                           // maxReadable
        0,                             // minReadable
        PLDM_RANGE_FIELD_FORMAT_UINT8, // rangeFieldFormat
        0,                             // rangeFieldsupport
        0,                             // nominalValue
        0,                             // normalMax
        0,                             // normalMin
        0,                             // warningHigh
        0,                             // warningLow
        0,                             // criticalHigh
        0,                             // criticalLow
        0,                             // fatalHigh
        0                              // fatalLow
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_TRUE(rc);
    EXPECT_EQ(1u, t1.numericSensorPdrs.size());
    EXPECT_EQ(1u, t1.numericSensors.size());

    auto numericSensor = t1.numericSensors[0];
    EXPECT_TRUE(numericSensor->oemIntfs.empty());

    // Should be the same as value in updateReading()
    numericSensor->updateReading(true, true, 10);
    EXPECT_EQ(10, numericSensor->getReading());

    // Count sensors now expose their metric value directly.
    numericSensor->updateReading(true, true,
                                 std::numeric_limits<double>::quiet_NaN());
    EXPECT_TRUE(std::isnan(numericSensor->getReading()));
}

TEST_F(NumericSensorTest, constructorCoverageAcrossDataSizesAndRanges)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis0"};

    const std::array<uint8_t, 8> sensorDataSizes{
        PLDM_SENSOR_DATA_SIZE_UINT8,  PLDM_SENSOR_DATA_SIZE_SINT8,
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_SENSOR_DATA_SIZE_SINT16,
        PLDM_SENSOR_DATA_SIZE_UINT32, PLDM_SENSOR_DATA_SIZE_SINT32,
        PLDM_SENSOR_DATA_SIZE_UINT64, PLDM_SENSOR_DATA_SIZE_SINT64};

    for (size_t i = 0; i < sensorDataSizes.size(); ++i)
    {
        auto pdr = makeNumericSensorValuePdr(sensorDataSizes[i],
                                             PLDM_RANGE_FIELD_FORMAT_UINT16);
        std::string sensorName = "sensor_data_size_" + std::to_string(i);
        NumericSensor sensor(0x01, false, pdr, sensorName, inventoryPath,
                             nullptr);

        sensor.updateReading(true, true, 25.0 + i);
        EXPECT_TRUE(std::isfinite(sensor.getReading()));
        sensor.handleErrGetSensorReading();
        EXPECT_TRUE(std::isnan(sensor.getReading()));
    }

    const std::array<uint8_t, 9> rangeFieldFormats{
        PLDM_RANGE_FIELD_FORMAT_UINT8,  PLDM_RANGE_FIELD_FORMAT_SINT8,
        PLDM_RANGE_FIELD_FORMAT_UINT16, PLDM_RANGE_FIELD_FORMAT_SINT16,
        PLDM_RANGE_FIELD_FORMAT_UINT32, PLDM_RANGE_FIELD_FORMAT_SINT32,
        PLDM_RANGE_FIELD_FORMAT_REAL32, PLDM_RANGE_FIELD_FORMAT_UINT64,
        PLDM_RANGE_FIELD_FORMAT_SINT64};

    for (size_t i = 0; i < rangeFieldFormats.size(); ++i)
    {
        auto pdr = makeNumericSensorValuePdr(
            PLDM_SENSOR_DATA_SIZE_UINT16, rangeFieldFormats[i],
            PLDM_SENSOR_UNIT_VOLTS);
        std::string sensorName = "sensor_range_format_" + std::to_string(i);
        NumericSensor sensor(0x02, false, pdr, sensorName, inventoryPath,
                             nullptr);

        sensor.updateReading(true, true, 200.0);
        sensor.updateReading(true, true, -200.0);
        sensor.updateReading(true, true, 0.0);

        EXPECT_TRUE(std::isfinite(sensor.getThresholdUpperWarning()));
        EXPECT_TRUE(std::isfinite(sensor.getThresholdLowerWarning()));
        EXPECT_TRUE(std::isfinite(sensor.getThresholdUpperCritical()));
        EXPECT_TRUE(std::isfinite(sensor.getThresholdLowerCritical()));
    }
}

TEST_F(NumericSensorTest, baseUnitNameUpdateAndPollingCoverage)
{
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis0"};

    const std::array<uint8_t, 10> baseUnits{
        PLDM_SENSOR_UNIT_DEGRESS_C, PLDM_SENSOR_UNIT_VOLTS,
        PLDM_SENSOR_UNIT_AMPS,      PLDM_SENSOR_UNIT_RPM,
        PLDM_SENSOR_UNIT_WATTS,     PLDM_SENSOR_UNIT_JOULES,
        PLDM_SENSOR_UNIT_HERTZ,     PLDM_SENSOR_UNIT_PERCENTAGE,
        PLDM_SENSOR_UNIT_COUNTS,    0xFF};

    for (size_t i = 0; i < baseUnits.size(); ++i)
    {
        auto pdr = makeNumericSensorValuePdr(
            PLDM_SENSOR_DATA_SIZE_UINT8, PLDM_RANGE_FIELD_FORMAT_UINT8,
            baseUnits[i], -1, false);
        std::string sensorName = "sensor_base_unit_" + std::to_string(i);
        NumericSensor sensor(0x03, false, pdr, sensorName, inventoryPath,
                             nullptr);

        sensor.isPriority = true;
        sensor.setLastUpdatedTimeStamp(0);
        EXPECT_TRUE(sensor.needsUpdate(2 * 1000 * 1000));

        sensor.removeValueIntf();
        if (baseUnits[i] != 0xFF)
        {
            EXPECT_FALSE(sensor.needsUpdate(3 * 1000 * 1000));
        }

        std::vector<std::string> assoc{
            "/xyz/openbmc_project/inventory/system/chassis/chassis0",
            "/xyz/openbmc_project/inventory/system/chassis/chassis1"};
        sensor.setInventoryPaths(assoc, false);
        sensor.setPhysicalContext(PhysicalContextType::CPU);
        sensor.updateReading(true, true, 10.0);
    }

    auto pdr = makeNumericSensorValuePdr(
        PLDM_SENSOR_DATA_SIZE_UINT16, PLDM_RANGE_FIELD_FORMAT_UINT16,
        PLDM_SENSOR_UNIT_WATTS, 0, true);
    std::string sensorName{"name.with symbols"};
    NumericSensor sensor(0x04, false, pdr, sensorName, inventoryPath, nullptr);

    auto oldPath = sensor.path;
    sensor.updateSensorName(sensorName);
    EXPECT_EQ(oldPath, sensor.path);

    sensor.updateSensorName("new sensor/name+#1");
    EXPECT_NE(oldPath, sensor.path);
    EXPECT_EQ(std::string::npos, sensor.path.find('#'));
    EXPECT_EQ(std::string::npos, sensor.path.find('+'));

    sensor.handleErrGetSensorReading();
    EXPECT_TRUE(std::isnan(sensor.getReading()));
}
