/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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

#include "oem/nvidia/platform-mc/memoryPageRetirementCount.hpp"
#include "platform-mc/numeric_sensor.hpp"
#include "platform-mc/terminus.hpp"
#include "platform-mc/terminus_manager.hpp"

#include <gtest/gtest.h>

using namespace pldm::platform_mc;

class NumericSensorTest : public testing::Test
{
  protected:
    NumericSensorTest() :
        bus(pldm::utils::DBusHandler::getBus()),
        event(sdeventplus::Event::get_default()),
        dbusImplRequester(bus, "/xyz/openbmc_project/pldm"),
        reqHandler(event, dbusImplRequester, sockManager, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, dbusImplRequester, termini, 0x8,
                        nullptr)
    {}

    sdbusplus::bus::bus& bus;
    sdeventplus::Event event;
    pldm::dbus_api::Requester dbusImplRequester;
    pldm::mctp_socket::Manager sockManager;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    pldm::platform_mc::TerminusManager terminusManager;
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
};

TEST_F(NumericSensorTest, conversionFormula)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1, terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                         // record handle
        0x1,                         // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,     // PDRType
        0x0,
        0x0,                         // recordChangeNumber
        0x0,
        56,                          // dataLength
        0,
        0,                           // PLDMTerminusHandle
        0x1,
        0x0,                         // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                           // entityType=Power Supply(120)
        1,
        0,                           // entityInstanceNumber
        0x1,
        0x0,                         // containerID=1
        PLDM_NO_INIT,                // sensorInit
        false,                       // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,  // baseUint(2)=degrees C
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

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    auto numericSensorPdr = t1.numericSensorPdrs[0];
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());

    std::string sensorName{"test1"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventroy/Item/Board/PLDM_device_1"};
    NumericSensor sensor(0x01, true, numericSensorPdr, sensorName,
                         inventoryPath);
    auto convertedValue = sensor.conversionFormula(40);
    // (40*1.5 + 1.0 ) * 10^0 = 61
    EXPECT_EQ(61, convertedValue);
}

TEST_F(NumericSensorTest, checkThreshold)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1, terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                         // record handle
        0x1,                         // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,     // PDRType
        0x0,
        0x0,                         // recordChangeNumber
        0x0,
        56,                          // dataLength
        0,
        0,                           // PLDMTerminusHandle
        0x1,
        0x0,                         // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                           // entityType=Power Supply(120)
        1,
        0,                           // entityInstanceNumber
        0x1,
        0x0,                         // containerID=1
        PLDM_NO_INIT,                // sensorInit
        false,                       // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,  // baseUint(2)=degrees C
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

    t1.pdrs.emplace_back(pdr1);
    t1.parsePDRs();
    auto numericSensorPdr = t1.numericSensorPdrs[0];
    std::string sensorName{"test1"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventroy/Item/Board/PLDM_device_1"};
    NumericSensor sensor(0x01, true, numericSensorPdr, sensorName,
                         inventoryPath);

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

TEST_F(NumericSensorTest, MemeoryPageRetirementSensor)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1, terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                         // record handle
        0x1,                         // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,     // PDRType
        0x0,
        0x0,                         // recordChangeNumber
        0x0,
        56,                          // dataLength
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
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());
    EXPECT_EQ(1, t1.numericSensors.size());

    auto numericSensor = t1.numericSensors[0];
    EXPECT_EQ(1, numericSensor->oemIntfs.size());

    auto memoryPageRetirementCount =
        dynamic_pointer_cast<OemMemoryPageRetirementCountInft>(
            numericSensor->oemIntfs[0]);

    // Should be the same as value in updateReading()
    numericSensor->updateReading(true, true, 10);
    EXPECT_EQ(10, memoryPageRetirementCount->memoryPageRetirementCount());

    // Should be zero for nan value
    numericSensor->updateReading(
        true, true, std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(0, memoryPageRetirementCount->memoryPageRetirementCount());
}