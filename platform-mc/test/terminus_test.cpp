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
#include "mock_terminus_manager.hpp"
#include "platform-mc/platform_manager.hpp"
#include "platform-mc/sensor_manager.hpp"
#include "platform-mc/terminus.hpp"
#include "test/test_instance_id.hpp"

#include <sdeventplus/event.hpp>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

using namespace pldm::platform_mc;
const uint8_t localEid = 0x08;
class TerminusTest : public testing::Test
{
  protected:
    TerminusTest() :
        bus(pldm::utils::DBusHandler::getBus()),
        event(sdeventplus::Event::get_default()),
        reqHandler(event, instanceIdDb, sockManager, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, instanceIdDb, termini, localEid,
                        nullptr),
        sensorManager(event, terminusManager, termini, nullptr),
        platformManager(terminusManager, termini)
    {
        reqHandler.setSocketHandler(nullptr);
    }

    void runEventLoopForMilliseconds(uint64_t msec)
    {
        uint64_t t0 = 0;
        uint64_t t1 = 0;
        uint64_t usec = msec * 1000;
        uint64_t elapsed = 0;
        sd_event_now(event.get(), CLOCK_MONOTONIC, &t0);
        do
        {
            sd_event_run(event.get(), usec - elapsed);
            sd_event_now(event.get(), CLOCK_MONOTONIC, &t1);
            elapsed = t1 - t0;
        } while (elapsed < usec);
    }

    void setupResponsesForDiscoverTerminus()
    {
        auto rc = terminusManager.clearQueuedResponses();
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> getTidResp0{0x00, PLDM_BASE, PLDM_GET_TID,
                                         PLDM_SUCCESS, 0x00};
        rc = terminusManager.enqueueResponse(getTidResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> setTidResp0{0x00, PLDM_BASE, PLDM_SET_TID,
                                         PLDM_SUCCESS};
        rc = terminusManager.enqueueResponse(setTidResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);

        // support pldm type0 and type2
        std::vector<uint8_t> getPldmTypesResp0{
            0x00, PLDM_BASE, 0x04, 0x00, 0x05, 0x00,
            0x00, 0x00,      0x00, 0x00, 0x00, 0x00};
        rc = terminusManager.enqueueResponse(getPldmTypesResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> getTerminusUidResp0{
            0x00, PLDM_PLATFORM, PLDM_GET_TERMINUS_UID,
            PLDM_ERROR_UNSUPPORTED_PLDM_CMD};
        rc = terminusManager.enqueueResponse(getTerminusUidResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);
    }

    void setupResponsesForInitTerminus()
    {
        auto rc = terminusManager.clearQueuedResponses();
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> eventMessageBufferSizeResp0{
            0x00, PLDM_PLATFORM, PLDM_EVENT_MESSAGE_BUFFER_SIZE,
            PLDM_ERROR_UNSUPPORTED_PLDM_CMD};
        rc = terminusManager.enqueueResponse(eventMessageBufferSizeResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> eventMessageSupportedResp0{
            0x00, PLDM_PLATFORM, PLDM_EVENT_MESSAGE_SUPPORTED,
            PLDM_ERROR_UNSUPPORTED_PLDM_CMD};
        rc = terminusManager.enqueueResponse(eventMessageSupportedResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> getPDRRepositoryInfoResp0{
            0x00, PLDM_PLATFORM, PLDM_EVENT_MESSAGE_SUPPORTED,
            PLDM_ERROR_UNSUPPORTED_PLDM_CMD};
        rc = terminusManager.enqueueResponse(getPDRRepositoryInfoResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> getPdrResp0{
            0x00,
            PLDM_PLATFORM,
            PLDM_GET_PDR,
            PLDM_SUCCESS,
            0x00,
            0x00,
            0x00,
            0x00, // nextRecordHandle
            0x00,
            0x00,
            0x00,
            0x00, // nextDataTransferHandle
            0x05, // startAndEnd
            69,
            0,    // responseCount
            0x00,
            0x00,
            0x00,
            0x01,                        // record handle
            0x01,                        // PDRHeaderVersion
            PLDM_NUMERIC_SENSOR_PDR,     // PDRType
            0x00,
            0x00,                        // recordChangeNumber
            34,
            0,                           // dataLength
            0x00,
            0x00,                        // PLDMTerminusHandle
            0x01,
            0x00,                        // sensorID=1
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
        rc = terminusManager.enqueueResponse(getPdrResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);
    }

    void setupResponsesForStartPolling()
    {
        auto rc = terminusManager.clearQueuedResponses();
        EXPECT_EQ(rc, PLDM_SUCCESS);

        std::vector<uint8_t> getSensorReadingResp0{
            0x00,
            PLDM_PLATFORM,
            PLDM_GET_SENSOR_READING,
            PLDM_SUCCESS,
            PLDM_SENSOR_DATA_SIZE_UINT8,
            PLDM_SENSOR_ENABLED,
            PLDM_NO_EVENT_GENERATION,
            PLDM_SENSOR_NORMAL,
            PLDM_SENSOR_NORMAL,
            PLDM_SENSOR_NORMAL,
            0x12};
        rc = terminusManager.enqueueResponse(getSensorReadingResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);
        rc = terminusManager.enqueueResponse(getSensorReadingResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);
        rc = terminusManager.enqueueResponse(getSensorReadingResp0);
        EXPECT_EQ(rc, PLDM_SUCCESS);
    }

    sdbusplus::bus::bus& bus;
    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    pldm::mctp_socket::Manager sockManager;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    pldm::platform_mc::MockTerminusManager terminusManager;
    pldm::platform_mc::SensorManager sensorManager;
    pldm::platform_mc::PlatformManager platformManager;
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
};

TEST_F(TerminusTest, supportedTypeTest)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    std::string uuid2("00000000-0000-0000-0000-000000000002");
    auto t1 = Terminus(1, 1 << PLDM_BASE, uuid1, terminusManager);
    auto t2 = Terminus(2, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid2,
                       terminusManager);

    EXPECT_EQ(true, t1.doesSupport(PLDM_BASE));
    EXPECT_EQ(false, t1.doesSupport(PLDM_PLATFORM));
    EXPECT_EQ(true, t2.doesSupport(PLDM_BASE));
    EXPECT_EQ(true, t2.doesSupport(PLDM_PLATFORM));
}

TEST_F(TerminusTest, getTidTest)
{
    const pldm::tid_t tid = 1;
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(tid, 1 << PLDM_BASE, uuid1, terminusManager);

    EXPECT_EQ(tid, t1.getTid());
}

TEST_F(TerminusTest, parseSensorAuxiliaryNamesPDRTest)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                             // record handle
        0x1,                             // PDRHeaderVersion
        PLDM_SENSOR_AUXILIARY_NAMES_PDR, // PDRType
        0x0,
        0x0,                             // recordChangeNumber
        21,
        0,                               // dataLength
        0,
        0x0,                             // PLDMTerminusHandle
        0x1,
        0x0,                             // sensorID
        0x1,                             // sensorCount
        0x1,                             // nameStringCount
        'e',
        'n',
        0x0, // nameLanguageTag
        0x0,
        'T',
        0x0,
        'E',
        0x0,
        'M',
        0x0,
        'P',
        0x0,
        '1',
        0x0,
        0x0 // sensorName
    };

    std::vector<uint8_t> pdr2{
        0x0, 0x0, 0x0,
        0x1,                             // record handle
        0x1,                             // PDRHeaderVersion
        PLDM_SENSOR_AUXILIARY_NAMES_PDR, // PDRType
        0x0,
        0x0,                             // recordChangeNumber
        21,
        0,                               // dataLength
        0,
        0x0,                             // PLDMTerminusHandle
        0x2,
        0x0,                             // sensorID
        0x2,                             // sensorCount
                                         // sensor0
        0x0,                             // nameStringCount
                                         // sensor1
        0x1,                             // nameStringCount
        'e', 'n',
        0x0,                             // nameLanguageTag
        0x0, 'T', 0x0, 'E', 0x0, 'M', 0x0, 'P', 0x0, '2', 0x0,
        0x0                              // sensorName
    };

    t1.pdrs.emplace_back(pdr1);
    t1.pdrs.emplace_back(pdr2);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);

    auto sensorAuxNames = t1.getSensorAuxiliaryNames(0);
    EXPECT_EQ(nullptr, sensorAuxNames);

    sensorAuxNames = t1.getSensorAuxiliaryNames(1);
    EXPECT_NE(nullptr, sensorAuxNames);

    const auto& [sensorId, sensorCnt, names] = *sensorAuxNames;
    EXPECT_EQ(1, sensorId);
    EXPECT_EQ(1, sensorCnt);
    EXPECT_EQ(1, names.size());
    EXPECT_EQ(1, names[0].size());
    EXPECT_EQ("en", names[0][0].first);
    EXPECT_EQ("TEMP1", names[0][0].second);

    sensorAuxNames = t1.getSensorAuxiliaryNames(2);
    EXPECT_NE(nullptr, sensorAuxNames);

    const auto& [sensorId2, sensorCnt2, names2] = *sensorAuxNames;
    EXPECT_EQ(2, sensorId2);
    EXPECT_EQ(2, sensorCnt2);
    EXPECT_EQ(2, names2.size());
    EXPECT_EQ(0, names2[0].size());
    EXPECT_EQ(1, names2[1].size());
    EXPECT_EQ("en", names2[1][0].first);
    EXPECT_EQ("TEMP2", names2[1][0].second);
}

TEST_F(TerminusTest, addNumericSensorTest)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                             // record handle
        0x1,                             // PDRHeaderVersion
        PLDM_SENSOR_AUXILIARY_NAMES_PDR, // PDRType
        0x0,
        0x0,                             // recordChangeNumber
        21,
        0,                               // dataLength
        0,
        0x0,                             // PLDMTerminusHandle
        0x1,
        0x0,                             // sensorID
        0x1,                             // sensorCount
        0x1,                             // nameStringCount
        'e',
        'n',
        0x0, // nameLanguageTag
        0x0,
        'T',
        0x0,
        'E',
        0x0,
        'M',
        0x0,
        'P',
        0x0,
        '1',
        0x0,
        0x0 // sensorName
    };

    std::vector<uint8_t> pdr2{
        0x0,
        0x0,
        0x0,
        0x1,                         // record handle
        0x1,                         // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,     // PDRType
        0x0,
        0x0,                         // recordChangeNumber
        56,
        0,                           // dataLength
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
        true,                        // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,  // baseUint(2)=degrees C
        0,                           // unitModifier
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
        0,
        0, // resolution
        0,
        0,
        0,
        0, // offset
        0,
        0, // accuracy
        0, // plusTolerance
        0, // minusTolerance
        0, // hysteresis
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
    t1.pdrs.emplace_back(pdr2);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());
    EXPECT_EQ(1, t1.numericSensors.size());
}

TEST_F(TerminusTest, parseNumericSensorPdrTest)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                         // record handle
        0x1,                         // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,     // PDRType
        0x0,
        0x0,                         // recordChangeNumber
        56,
        0,                           // dataLength
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
        0,                           // unitModifier
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
        0,
        0, // resolution
        0,
        0,
        0,
        0, // offset
        0,
        0, // accuracy
        0, // plusTolerance
        0, // minusTolerance
        3, // hysteresis = 3
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
        50,                            // nominalValue = 50
        60,                            // normalMax = 60
        40,                            // normalMin = 40
        70,                            // warningHigh = 70
        30,                            // warningLow = 30
        80,                            // criticalHigh = 80
        20,                            // criticalLow = 20
        90,                            // fatalHigh = 90
        10                             // fatalLow = 10
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());

    auto numericSensorPdrs = t1.numericSensorPdrs[0];
    EXPECT_EQ(1, numericSensorPdrs->sensor_id);
    EXPECT_EQ(PLDM_SENSOR_DATA_SIZE_UINT8, numericSensorPdrs->sensor_data_size);
    EXPECT_EQ(PLDM_ENTITY_POWER_SUPPLY, numericSensorPdrs->entity_type);
    EXPECT_EQ(2, numericSensorPdrs->base_unit);
    EXPECT_EQ(0.0, numericSensorPdrs->offset);
    EXPECT_EQ(3, numericSensorPdrs->hysteresis.value_u8);
    EXPECT_EQ(1.0, numericSensorPdrs->update_interval);
    EXPECT_EQ(255, numericSensorPdrs->max_readable.value_u8);
    EXPECT_EQ(0, numericSensorPdrs->min_readable.value_u8);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_UINT8,
              numericSensorPdrs->range_field_format);
    EXPECT_EQ(0, numericSensorPdrs->range_field_support.byte);
    EXPECT_EQ(50, numericSensorPdrs->nominal_value.value_u8);
    EXPECT_EQ(60, numericSensorPdrs->normal_max.value_u8);
    EXPECT_EQ(40, numericSensorPdrs->normal_min.value_u8);
    EXPECT_EQ(70, numericSensorPdrs->warning_high.value_u8);
    EXPECT_EQ(30, numericSensorPdrs->warning_low.value_u8);
    EXPECT_EQ(80, numericSensorPdrs->critical_high.value_u8);
    EXPECT_EQ(20, numericSensorPdrs->critical_low.value_u8);
    EXPECT_EQ(90, numericSensorPdrs->fatal_high.value_u8);
    EXPECT_EQ(10, numericSensorPdrs->fatal_low.value_u8);
}

TEST_F(TerminusTest, parseNumericSensorPdrSint8Test)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                           // record handle
        0x1,                           // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,       // PDRType
        0x0,
        0x0,                           // recordChangeNumber
        56,
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
        0,                             // unitModifier
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
        0,
        0, // resolution
        0,
        0,
        0,
        0, // offset
        0,
        0, // accuracy
        0, // plusTolerance
        0, // minusTolerance
        3, // hysteresis = 3
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
        0x64,                          // maxReadable = 100
        0x9c,                          // minReadable = -100
        PLDM_RANGE_FIELD_FORMAT_SINT8, // rangeFieldFormat
        0,                             // rangeFieldsupport
        0,                             // nominalValue = 0
        5,                             // normalMax = 5
        0xfb,                          // normalMin = -5
        10,                            // warningHigh = 10
        0xf6,                          // warningLow = -10
        20,                            // criticalHigh = 20
        0xec,                          // criticalLow = -20
        30,                            // fatalHigh = 30
        0xe2                           // fatalLow = -30
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());

    auto numericSensorPdrs = t1.numericSensorPdrs[0];
    EXPECT_EQ(1, numericSensorPdrs->sensor_id);
    EXPECT_EQ(PLDM_SENSOR_DATA_SIZE_SINT8, numericSensorPdrs->sensor_data_size);
    EXPECT_EQ(PLDM_ENTITY_POWER_SUPPLY, numericSensorPdrs->entity_type);
    EXPECT_EQ(2, numericSensorPdrs->base_unit);
    EXPECT_EQ(0.0, numericSensorPdrs->offset);
    EXPECT_EQ(3, numericSensorPdrs->hysteresis.value_s8);
    EXPECT_EQ(1.0, numericSensorPdrs->update_interval);
    EXPECT_EQ(100, numericSensorPdrs->max_readable.value_s8);
    EXPECT_EQ(-100, numericSensorPdrs->min_readable.value_s8);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_SINT8,
              numericSensorPdrs->range_field_format);
    EXPECT_EQ(0, numericSensorPdrs->range_field_support.byte);
    EXPECT_EQ(0, numericSensorPdrs->nominal_value.value_s8);
    EXPECT_EQ(5, numericSensorPdrs->normal_max.value_s8);
    EXPECT_EQ(-5, numericSensorPdrs->normal_min.value_s8);
    EXPECT_EQ(10, numericSensorPdrs->warning_high.value_s8);
    EXPECT_EQ(-10, numericSensorPdrs->warning_low.value_s8);
    EXPECT_EQ(20, numericSensorPdrs->critical_high.value_s8);
    EXPECT_EQ(-20, numericSensorPdrs->critical_low.value_s8);
    EXPECT_EQ(30, numericSensorPdrs->fatal_high.value_s8);
    EXPECT_EQ(-30, numericSensorPdrs->fatal_low.value_s8);
}

TEST_F(TerminusTest, parseNumericSensorPdrUint16Test)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                          // record handle
        0x1,                          // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,      // PDRType
        0x0,
        0x0,                          // recordChangeNumber
        56,
        0,                            // dataLength
        0,
        0,                            // PLDMTerminusHandle
        0x1,
        0x0,                          // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                            // entityType=Power Supply(120)
        1,
        0,                            // entityInstanceNumber
        0x1,
        0x0,                          // containerID=1
        PLDM_NO_INIT,                 // sensorInit
        false,                        // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,   // baseUint(2)=degrees C
        0,                            // unitModifier
        0,                            // rateUnit
        0,                            // baseOEMUnitHandle
        0,                            // auxUnit
        0,                            // auxUnitModifier
        0,                            // auxRateUnit
        0,                            // rel
        0,                            // auxOEMUnitHandle
        true,                         // isLinear
        PLDM_SENSOR_DATA_SIZE_UINT16, // sensorDataSize
        0,
        0,
        0,
        0, // resolution
        0,
        0,
        0,
        0, // offset
        0,
        0, // accuracy
        0, // plusTolerance
        0, // minusTolerance
        3,
        0, // hysteresis = 3
        0, // supportedThresholds
        0, // thresholdAndHysteresisVolatility
        0,
        0,
        0x80,
        0x3f, // stateTransistionInterval=1.0
        0,
        0,
        0x80,
        0x3f,                           // updateInverval=1.0
        0,
        0x10,                           // maxReadable = 4096
        0,
        0,                              // minReadable = 0
        PLDM_RANGE_FIELD_FORMAT_UINT16, // rangeFieldFormat
        0,                              // rangeFieldsupport
        0x88,
        0x13,                           // nominalValue = 5,000
        0x70,
        0x17,                           // normalMax = 6,000
        0xa0,
        0x0f,                           // normalMin = 4,000
        0x58,
        0x1b,                           // warningHigh = 7,000
        0xb8,
        0x0b,                           // warningLow = 3,000
        0x40,
        0x1f,                           // criticalHigh = 8,000
        0xd0,
        0x07,                           // criticalLow = 2,000
        0x28,
        0x23,                           // fatalHigh = 9,000
        0xe8,
        0x03                            // fatalLow = 1,000
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());

    auto numericSensorPdrs = t1.numericSensorPdrs[0];
    EXPECT_EQ(1, numericSensorPdrs->sensor_id);
    EXPECT_EQ(PLDM_SENSOR_DATA_SIZE_UINT16,
              numericSensorPdrs->sensor_data_size);
    EXPECT_EQ(PLDM_ENTITY_POWER_SUPPLY, numericSensorPdrs->entity_type);
    EXPECT_EQ(2, numericSensorPdrs->base_unit);
    EXPECT_EQ(0.0, numericSensorPdrs->offset);
    EXPECT_EQ(3, numericSensorPdrs->hysteresis.value_u16);
    EXPECT_EQ(1.0, numericSensorPdrs->update_interval);
    EXPECT_EQ(4096, numericSensorPdrs->max_readable.value_u16);
    EXPECT_EQ(0, numericSensorPdrs->min_readable.value_u16);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_UINT16,
              numericSensorPdrs->range_field_format);
    EXPECT_EQ(0, numericSensorPdrs->range_field_support.byte);
    EXPECT_EQ(5000, numericSensorPdrs->nominal_value.value_u16);
    EXPECT_EQ(6000, numericSensorPdrs->normal_max.value_u16);
    EXPECT_EQ(4000, numericSensorPdrs->normal_min.value_u16);
    EXPECT_EQ(7000, numericSensorPdrs->warning_high.value_u16);
    EXPECT_EQ(3000, numericSensorPdrs->warning_low.value_u16);
    EXPECT_EQ(8000, numericSensorPdrs->critical_high.value_u16);
    EXPECT_EQ(2000, numericSensorPdrs->critical_low.value_u16);
    EXPECT_EQ(9000, numericSensorPdrs->fatal_high.value_u16);
    EXPECT_EQ(1000, numericSensorPdrs->fatal_low.value_u16);
}

TEST_F(TerminusTest, parseNumericSensorPdrSint16Test)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                          // record handle
        0x1,                          // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,      // PDRType
        0x0,
        0x0,                          // recordChangeNumber
        56,
        0,                            // dataLength
        0,
        0,                            // PLDMTerminusHandle
        0x1,
        0x0,                          // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                            // entityType=Power Supply(120)
        1,
        0,                            // entityInstanceNumber
        0x1,
        0x0,                          // containerID=1
        PLDM_NO_INIT,                 // sensorInit
        false,                        // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,   // baseUint(2)=degrees C
        0,                            // unitModifier
        0,                            // rateUnit
        0,                            // baseOEMUnitHandle
        0,                            // auxUnit
        0,                            // auxUnitModifier
        0,                            // auxRateUnit
        0,                            // rel
        0,                            // auxOEMUnitHandle
        true,                         // isLinear
        PLDM_SENSOR_DATA_SIZE_SINT16, // sensorDataSize
        0,
        0,
        0,
        0, // resolution
        0,
        0,
        0,
        0, // offset
        0,
        0, // accuracy
        0, // plusTolerance
        0, // minusTolerance
        3,
        0, // hysteresis
        0, // supportedThresholds
        0, // thresholdAndHysteresisVolatility
        0,
        0,
        0x80,
        0x3f, // stateTransistionInterval=1.0
        0,
        0,
        0x80,
        0x3f,                           // updateInverval=1.0
        0xe8,
        0x03,                           // maxReadable = 1000
        0x18,
        0xfc,                           // minReadable = -1000
        PLDM_RANGE_FIELD_FORMAT_SINT16, // rangeFieldFormat
        0,                              // rangeFieldsupport
        0,
        0,                              // nominalValue = 0
        0xf4,
        0x01,                           // normalMax = 500
        0x0c,
        0xfe,                           // normalMin = -500
        0xe8,
        0x03,                           // warningHigh = 1,000
        0x18,
        0xfc,                           // warningLow = -1,000
        0xd0,
        0x07,                           // criticalHigh = 2,000
        0x30,
        0xf8,                           // criticalLow = -2,000
        0xb8,
        0x0b,                           // fatalHigh = 3,000
        0x48,
        0xf4                            // fatalLow = -3,000
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());

    auto numericSensorPdrs = t1.numericSensorPdrs[0];
    EXPECT_EQ(1, numericSensorPdrs->sensor_id);
    EXPECT_EQ(PLDM_SENSOR_DATA_SIZE_SINT16,
              numericSensorPdrs->sensor_data_size);
    EXPECT_EQ(PLDM_ENTITY_POWER_SUPPLY, numericSensorPdrs->entity_type);
    EXPECT_EQ(2, numericSensorPdrs->base_unit);
    EXPECT_EQ(0.0, numericSensorPdrs->offset);
    EXPECT_EQ(3, numericSensorPdrs->hysteresis.value_s16);
    EXPECT_EQ(1.0, numericSensorPdrs->update_interval);
    EXPECT_EQ(1000, numericSensorPdrs->max_readable.value_s16);
    EXPECT_EQ(-1000, numericSensorPdrs->min_readable.value_s16);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_SINT16,
              numericSensorPdrs->range_field_format);
    EXPECT_EQ(0, numericSensorPdrs->range_field_support.byte);
    EXPECT_EQ(0, numericSensorPdrs->nominal_value.value_s16);
    EXPECT_EQ(500, numericSensorPdrs->normal_max.value_s16);
    EXPECT_EQ(-500, numericSensorPdrs->normal_min.value_s16);
    EXPECT_EQ(1000, numericSensorPdrs->warning_high.value_s16);
    EXPECT_EQ(-1000, numericSensorPdrs->warning_low.value_s16);
    EXPECT_EQ(2000, numericSensorPdrs->critical_high.value_s16);
    EXPECT_EQ(-2000, numericSensorPdrs->critical_low.value_s16);
    EXPECT_EQ(3000, numericSensorPdrs->fatal_high.value_s16);
    EXPECT_EQ(-3000, numericSensorPdrs->fatal_low.value_s16);
}

TEST_F(TerminusTest, parseNumericSensorPdrUint32Test)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                          // record handle
        0x1,                          // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,      // PDRType
        0x0,
        0x0,                          // recordChangeNumber
        56,
        0,                            // dataLength
        0,
        0,                            // PLDMTerminusHandle
        0x1,
        0x0,                          // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                            // entityType=Power Supply(120)
        1,
        0,                            // entityInstanceNumber
        0x1,
        0x0,                          // containerID=1
        PLDM_NO_INIT,                 // sensorInit
        false,                        // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,   // baseUint(2)=degrees C
        0,                            // unitModifier
        0,                            // rateUnit
        0,                            // baseOEMUnitHandle
        0,                            // auxUnit
        0,                            // auxUnitModifier
        0,                            // auxRateUnit
        0,                            // rel
        0,                            // auxOEMUnitHandle
        true,                         // isLinear
        PLDM_SENSOR_DATA_SIZE_UINT32, // sensorDataSize
        0,
        0,
        0,
        0, // resolution
        0,
        0,
        0,
        0, // offset
        0,
        0, // accuracy
        0, // plusTolerance
        0, // minusTolerance
        3,
        0,
        0,
        0, // hysteresis
        0, // supportedThresholds
        0, // thresholdAndHysteresisVolatility
        0,
        0,
        0x80,
        0x3f, // stateTransistionInterval=1.0
        0,
        0,
        0x80,
        0x3f, // updateInverval=1.0
        0,
        0x10,
        0,
        0, // maxReadable = 4096
        0,
        0,
        0,
        0,                              // minReadable = 0
        PLDM_RANGE_FIELD_FORMAT_UINT32, // rangeFieldFormat
        0,                              // rangeFieldsupport
        0x40,
        0x4b,
        0x4c,
        0x00, // nominalValue = 5,000,000
        0x80,
        0x8d,
        0x5b,
        0x00, // normalMax = 6,000,000
        0x00,
        0x09,
        0x3d,
        0x00, // normalMin = 4,000,000
        0xc0,
        0xcf,
        0x6a,
        0x00, // warningHigh = 7,000,000
        0xc0,
        0xc6,
        0x2d,
        0x00, // warningLow = 3,000,000
        0x00,
        0x12,
        0x7a,
        0x00, // criticalHigh = 8,000,000
        0x80,
        0x84,
        0x1e,
        0x00, // criticalLow = 2,000,000
        0x40,
        0x54,
        0x89,
        0x00, // fatalHigh = 9,000,000
        0x40,
        0x42,
        0x0f,
        0x00 // fatalLow = 1,000,000
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());

    auto numericSensorPdrs = t1.numericSensorPdrs[0];
    EXPECT_EQ(1, numericSensorPdrs->sensor_id);
    EXPECT_EQ(PLDM_SENSOR_DATA_SIZE_UINT32,
              numericSensorPdrs->sensor_data_size);
    EXPECT_EQ(PLDM_ENTITY_POWER_SUPPLY, numericSensorPdrs->entity_type);
    EXPECT_EQ(2, numericSensorPdrs->base_unit);
    EXPECT_EQ(0.0, numericSensorPdrs->offset);
    EXPECT_EQ(3, numericSensorPdrs->hysteresis.value_u32);
    EXPECT_EQ(1.0, numericSensorPdrs->update_interval);
    EXPECT_EQ(4096, numericSensorPdrs->max_readable.value_u32);
    EXPECT_EQ(0, numericSensorPdrs->min_readable.value_u32);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_UINT32,
              numericSensorPdrs->range_field_format);
    EXPECT_EQ(0, numericSensorPdrs->range_field_support.byte);
    EXPECT_EQ(5000000, numericSensorPdrs->nominal_value.value_u32);
    EXPECT_EQ(6000000, numericSensorPdrs->normal_max.value_u32);
    EXPECT_EQ(4000000, numericSensorPdrs->normal_min.value_u32);
    EXPECT_EQ(7000000, numericSensorPdrs->warning_high.value_u32);
    EXPECT_EQ(3000000, numericSensorPdrs->warning_low.value_u32);
    EXPECT_EQ(8000000, numericSensorPdrs->critical_high.value_u32);
    EXPECT_EQ(2000000, numericSensorPdrs->critical_low.value_u32);
    EXPECT_EQ(9000000, numericSensorPdrs->fatal_high.value_u32);
    EXPECT_EQ(1000000, numericSensorPdrs->fatal_low.value_u32);
}

TEST_F(TerminusTest, parseNumericSensorPdrSint32Test)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                          // record handle
        0x1,                          // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,      // PDRType
        0x0,
        0x0,                          // recordChangeNumber
        56,
        0,                            // dataLength
        0,
        0,                            // PLDMTerminusHandle
        0x1,
        0x0,                          // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                            // entityType=Power Supply(120)
        1,
        0,                            // entityInstanceNumber
        0x1,
        0x0,                          // containerID=1
        PLDM_NO_INIT,                 // sensorInit
        false,                        // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,   // baseUint(2)=degrees C
        0,                            // unitModifier
        0,                            // rateUnit
        0,                            // baseOEMUnitHandle
        0,                            // auxUnit
        0,                            // auxUnitModifier
        0,                            // auxRateUnit
        0,                            // rel
        0,                            // auxOEMUnitHandle
        true,                         // isLinear
        PLDM_SENSOR_DATA_SIZE_SINT32, // sensorDataSize
        0,
        0,
        0,
        0, // resolution
        0,
        0,
        0,
        0, // offset
        0,
        0, // accuracy
        0, // plusTolerance
        0, // minusTolerance
        3,
        0,
        0,
        0, // hysteresis
        0, // supportedThresholds
        0, // thresholdAndHysteresisVolatility
        0,
        0,
        0x80,
        0x3f, // stateTransistionInterval=1.0
        0,
        0,
        0x80,
        0x3f, // updateInverval=1.0
        0xa0,
        0x86,
        0x01,
        0x00, // maxReadable = 100000
        0x60,
        0x79,
        0xfe,
        0xff,                           // minReadable = -10000
        PLDM_RANGE_FIELD_FORMAT_SINT32, // rangeFieldFormat
        0,                              // rangeFieldsupport
        0,
        0,
        0,
        0, // nominalValue = 0
        0x20,
        0xa1,
        0x07,
        0x00, // normalMax = 500,000
        0xe0,
        0x5e,
        0xf8,
        0xff, // normalMin = -500,000
        0x40,
        0x42,
        0x0f,
        0x00, // warningHigh = 1,000,000
        0xc0,
        0xbd,
        0xf0,
        0xff, // warningLow = -1,000,000
        0x80,
        0x84,
        0x1e,
        0x00, // criticalHigh = 2,000,000
        0x80,
        0x7b,
        0xe1,
        0xff, // criticalLow = -2,000,000
        0xc0,
        0xc6,
        0x2d,
        0x00, // fatalHigh = 3,000,000
        0x40,
        0x39,
        0xd2,
        0xff // fatalLow = -3,000,000
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());

    auto numericSensorPdrs = t1.numericSensorPdrs[0];
    EXPECT_EQ(1, numericSensorPdrs->sensor_id);
    EXPECT_EQ(PLDM_SENSOR_DATA_SIZE_SINT32,
              numericSensorPdrs->sensor_data_size);
    EXPECT_EQ(PLDM_ENTITY_POWER_SUPPLY, numericSensorPdrs->entity_type);
    EXPECT_EQ(2, numericSensorPdrs->base_unit);
    EXPECT_EQ(0.0, numericSensorPdrs->offset);
    EXPECT_EQ(3, numericSensorPdrs->hysteresis.value_s32);
    EXPECT_EQ(1.0, numericSensorPdrs->update_interval);
    EXPECT_EQ(100000, numericSensorPdrs->max_readable.value_s32);
    EXPECT_EQ(-100000, numericSensorPdrs->min_readable.value_s32);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_SINT32,
              numericSensorPdrs->range_field_format);
    EXPECT_EQ(0, numericSensorPdrs->range_field_support.byte);
    EXPECT_EQ(0, numericSensorPdrs->nominal_value.value_s32);
    EXPECT_EQ(500000, numericSensorPdrs->normal_max.value_s32);
    EXPECT_EQ(-500000, numericSensorPdrs->normal_min.value_s32);
    EXPECT_EQ(1000000, numericSensorPdrs->warning_high.value_s32);
    EXPECT_EQ(-1000000, numericSensorPdrs->warning_low.value_s32);
    EXPECT_EQ(2000000, numericSensorPdrs->critical_high.value_s32);
    EXPECT_EQ(-2000000, numericSensorPdrs->critical_low.value_s32);
    EXPECT_EQ(3000000, numericSensorPdrs->fatal_high.value_s32);
    EXPECT_EQ(-3000000, numericSensorPdrs->fatal_low.value_s32);
}

TEST_F(TerminusTest, parseNumericSensorPdrReal32Test)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                          // record handle
        0x1,                          // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,      // PDRType
        0x0,
        0x0,                          // recordChangeNumber
        56,
        0,                            // dataLength
        0,
        0,                            // PLDMTerminusHandle
        0x1,
        0x0,                          // sensorID=1
        PLDM_ENTITY_POWER_SUPPLY,
        0,                            // entityType=Power Supply(120)
        1,
        0,                            // entityInstanceNumber
        0x1,
        0x0,                          // containerID=1
        PLDM_NO_INIT,                 // sensorInit
        false,                        // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C,   // baseUint(2)=degrees C
        0,                            // unitModifier
        0,                            // rateUnit
        0,                            // baseOEMUnitHandle
        0,                            // auxUnit
        0,                            // auxUnitModifier
        0,                            // auxRateUnit
        0,                            // rel
        0,                            // auxOEMUnitHandle
        true,                         // isLinear
        PLDM_SENSOR_DATA_SIZE_SINT32, // sensorDataSize
        0,
        0,
        0,
        0, // resolution
        0,
        0,
        0,
        0, // offset
        0,
        0, // accuracy
        0, // plusTolerance
        0, // minusTolerance
        3,
        0,
        0,
        0, // hysteresis
        0, // supportedThresholds
        0, // thresholdAndHysteresisVolatility
        0,
        0,
        0x80,
        0x3f, // stateTransistionInterval=1.0
        0,
        0,
        0x80,
        0x3f, // updateInverval=1.0
        0xa0,
        0x86,
        0x01,
        0x00, // maxReadable = 100000
        0x60,
        0x79,
        0xfe,
        0xff,                           // minReadable = -10000
        PLDM_RANGE_FIELD_FORMAT_REAL32, // rangeFieldFormat
        0,                              // rangeFieldsupport
        0,
        0,
        0,
        0, // nominalValue = 0.0
        0x33,
        0x33,
        0x48,
        0x42, // normalMax = 50.05
        0x33,
        0x33,
        0x48,
        0xc2, // normalMin = -50.05
        0x83,
        0x00,
        0xc8,
        0x42, // warningHigh = 100.001
        0x83,
        0x00,
        0xc8,
        0xc2, // warningLow = -100.001
        0x83,
        0x00,
        0x48,
        0x43, // criticalHigh = 200.002
        0x83,
        0x00,
        0x48,
        0xc3, // criticalLow = -200.002
        0x62,
        0x00,
        0x96,
        0x43, // fatalHigh = 300.003
        0x62,
        0x00,
        0x96,
        0xc3 // fatalLow = -300.003
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericSensorPdrs.size());

    auto numericSensorPdrs = t1.numericSensorPdrs[0];
    EXPECT_EQ(1, numericSensorPdrs->sensor_id);
    EXPECT_EQ(PLDM_SENSOR_DATA_SIZE_SINT32,
              numericSensorPdrs->sensor_data_size);
    EXPECT_EQ(PLDM_ENTITY_POWER_SUPPLY, numericSensorPdrs->entity_type);
    EXPECT_EQ(2, numericSensorPdrs->base_unit);
    EXPECT_EQ(0.0, numericSensorPdrs->offset);
    EXPECT_EQ(3, numericSensorPdrs->hysteresis.value_s32);
    EXPECT_EQ(1.0, numericSensorPdrs->update_interval);
    EXPECT_EQ(100000, numericSensorPdrs->max_readable.value_s32);
    EXPECT_EQ(-100000, numericSensorPdrs->min_readable.value_s32);
    EXPECT_EQ(PLDM_RANGE_FIELD_FORMAT_REAL32,
              numericSensorPdrs->range_field_format);
    EXPECT_FLOAT_EQ(0, numericSensorPdrs->range_field_support.byte);
    EXPECT_FLOAT_EQ(0, numericSensorPdrs->nominal_value.value_f32);
    EXPECT_FLOAT_EQ(50.05f, numericSensorPdrs->normal_max.value_f32);
    EXPECT_FLOAT_EQ(-50.05f, numericSensorPdrs->normal_min.value_f32);
    EXPECT_FLOAT_EQ(100.001f, numericSensorPdrs->warning_high.value_f32);
    EXPECT_FLOAT_EQ(-100.001f, numericSensorPdrs->warning_low.value_f32);
    EXPECT_FLOAT_EQ(200.002f, numericSensorPdrs->critical_high.value_f32);
    EXPECT_FLOAT_EQ(-200.002f, numericSensorPdrs->critical_low.value_f32);
    EXPECT_FLOAT_EQ(300.003f, numericSensorPdrs->fatal_high.value_f32);
    EXPECT_FLOAT_EQ(-300.003f, numericSensorPdrs->fatal_low.value_f32);
}

TEST_F(TerminusTest, parseNumericSensorPDRInvalidSizeTest)
{
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    // A corrupted PDR. The data after plusTolerance missed.
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                         // record handle
        0x1,                         // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,     // PDRType
        0x0,
        0x0,                         // recordChangeNumber
        34,
        0,                           // dataLength
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
        2,                           // baseUint(2)=degrees C
        0,                           // unitModifier
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
        0,
        0, // resolution
        0,
        0,
        0,
        0, // offset
        0,
        0, // accuracy
        0  // plusTolerance
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(0, t1.numericSensorPdrs.size());
}

TEST_F(TerminusTest, TerminusOnOffLineTest)
{
    pldm::UUID uuidBad{"f72d6f90-5675-11ed-9b6a-0242ac120003"};
    pldm::UUID uuid{"f72d6f90-5675-11ed-9b6a-0242ac120002"};
    pldm::MctpInfos mctpInfos{pldm::MctpInfo(
        12, uuid, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1,
        std::nullopt,
        "xyz.openbmc_project.MCTP.Endpoint.BindingTypes.PCIe",
        std::nullopt)};

    /* 1. test discoverMctpTerminus(): check if terminus is discovered
     * successfully by mock responses */
    setupResponsesForDiscoverTerminus();
    terminusManager.discoverMctpTerminus(mctpInfos);
    EXPECT_EQ(1, termini.size());

    /* 2. test getTerminus(): check if terminus can be found by uuid */
    auto terminus = terminusManager.getTerminus(uuidBad);
    EXPECT_EQ(nullptr, terminus);

    terminus = terminusManager.getTerminus(uuid);
    EXPECT_NE(nullptr, terminus);
    EXPECT_EQ(uuid, terminus->getUuid());

<<<<<<< HEAD
    /* 3. test initTerminus(): check if sensor is created successfully by mock
     * response */
    setupResponsesForInitTerminus();
    platformManager.initTerminus();
    EXPECT_EQ(1, terminus->numericSensorPdrs.size());

    /* 4. test updateReading(): check if sensor PDIs are good */
    auto numericSensor = terminus->numericSensors[0];
    numericSensor->updateReading(true, true, 10);
    EXPECT_EQ(true, numericSensor->availabilityIntf->available());
    EXPECT_EQ(true, numericSensor->operationalStatusIntf->functional());
    // raw = 10, converted value= 10*1.5 + 1 = 16
    EXPECT_EQ(16, numericSensor->valueIntf->value());

    /* 5. test setOffline(): check if sensor PDIs are in offline state*/
    sensorManager.setOffline(terminus->getTid());
    EXPECT_EQ(false, numericSensor->operationalStatusIntf->functional());
    EXPECT_THAT(numericSensor->valueIntf->value(), testing::IsNan());

    /* 6. test setOnline(): check if sensor PDIs are in online state */
    setupResponsesForStartPolling();
    sensorManager.setOnline(terminus->getTid());
    runEventLoopForMilliseconds(2000);
    EXPECT_EQ(true, numericSensor->operationalStatusIntf->functional());
    // raw = 18, converted value= 18*1.5 + 1 = 28
    EXPECT_EQ(28, numericSensor->valueIntf->value());
||||||| constructed merge base
    EXPECT_TRUE(terminus.parsePDRs());
    EXPECT_EQ(cases.size() + 1, terminus.numericEffecterPdrs.size());
    EXPECT_EQ(cases.size() + 1, terminus.numericEffecters.size());
    EXPECT_NE(std::string::npos,
              terminus.numericEffecters.front()->path.find("TerminusEffecter"));
}

TEST_F(TerminusTest, addStateSensorAndEffecterCoverage)
{
    constexpr uint16_t sensorId = 0x211;
    constexpr uint16_t effecterId = 0x311;
    std::string uuid("00000000-0000-0000-0000-0000000000F0");
    Terminus terminus(0x10, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusState");

    auto sensorAuxPdr = makeSensorAuxNamePdr(sensorId);
    auto stateSensorPdr = makeStateSensorPdr(
        sensorId, PLDM_ENTITY_SYS_BOARD, PLDM_STATESET_ID_HEALTHSTATE, true);
    auto effecterAuxPdr =
        makeAuxNamePdr(effecterId, PLDM_EFFECTER_AUXILIARY_NAMES_PDR);
    auto stateEffecterPdr = makeStateEffecterPdr(
        effecterId, PLDM_ENTITY_SYS_BOARD, PLDM_STATESET_ID_BOOT_REQUEST);

    terminus.pdrs.emplace_back(sensorAuxPdr);
    terminus.pdrs.emplace_back(stateSensorPdr);
    terminus.pdrs.emplace_back(effecterAuxPdr);
    terminus.pdrs.emplace_back(stateEffecterPdr);

    ASSERT_TRUE(terminus.parsePDRs());
    ASSERT_EQ(1u, terminus.stateSensors.size());
    ASSERT_EQ(1u, terminus.stateEffecters.size());
}

TEST_F(TerminusTest, privateFindInventoryAndPhysicalContextCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000155");
    Terminus terminus(0x55, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis55";
    terminus.setInstance(7);

    const std::string cpu0{
        "/xyz/openbmc_project/inventory/system/chassis/chassis55/cpu0"};
    const std::string cpu1{
        "/xyz/openbmc_project/inventory/system/chassis/chassis55/cpu1"};
    const std::string dimm0{
        "/xyz/openbmc_project/inventory/system/chassis/chassis55/dimm0"};
    const std::string dimm1{
        "/xyz/openbmc_project/inventory/system/chassis/chassis55/dimm1"};

    terminus.inventories.emplace_back(cpu0, PLDM_ENTITY_PROC, 7);
    terminus.inventories.emplace_back(cpu1, PLDM_ENTITY_PROC, 7);
    terminus.inventories.emplace_back(dimm0, PLDM_ENTITY_MEMORY_CONTROLLER, 2);
    terminus.inventories.emplace_back(dimm1, PLDM_ENTITY_MEMORY_CONTROLLER, 2);
    terminus.inventoryParentMap[dimm0] = cpu0;
    terminus.inventoryParentMap[dimm1] = cpu1;

    EntityInfo containerEntity{overallSystemCotainerId, PLDM_ENTITY_PROC, 1};
    terminus.entityAssociations.emplace(
        1, std::make_pair(containerEntity, std::set<EntityInfo>{}));

    auto overallPaths = terminus.findInventory(overallSystemCotainerId, false);
    ASSERT_EQ(1u, overallPaths.size());
    EXPECT_EQ(terminus.systemInventoryPath, overallPaths.front());

    auto unknownContainerPaths =
        terminus.findInventory(static_cast<ContainerID>(0x9FFF), false);
    ASSERT_EQ(1u, unknownContainerPaths.size());
    EXPECT_EQ(terminus.systemInventoryPath, unknownContainerPaths.front());

    EntityInfo cpuEntity{1, PLDM_ENTITY_PROC, 1};
    auto cpuPaths = terminus.findInventory(cpuEntity, false);
    EXPECT_EQ(2u, cpuPaths.size());

    terminus.entities.clear();
    EntityInfo dimmEntity{1, PLDM_ENTITY_MEMORY_CONTROLLER, 2};
    auto dimmPaths = terminus.findInventory(dimmEntity, false);
    EXPECT_EQ(2u, dimmPaths.size());

    terminus.entities.clear();
    terminus.inventoryParentMap.clear();
    auto dimmFallback = terminus.findInventory(dimmEntity, false);
    EXPECT_EQ(1u, dimmFallback.size());
    EXPECT_EQ(dimm0, dimmFallback.front());

    terminus.entities.clear();
    EntityInfo missingEntity{1, PLDM_ENTITY_NETWORK_CONTROLLER, 99};
    auto closest = terminus.findInventory(missingEntity, true);
    EXPECT_EQ(2u, closest.size());

    terminus.entities.clear();
    auto missingWithoutClosest = terminus.findInventory(missingEntity, false);
    EXPECT_TRUE(missingWithoutClosest.empty());

    EXPECT_EQ(PhysicalContextType::Memory,
              terminus.toPhysicalContextType(PLDM_ENTITY_MEMORY_CONTROLLER));
    EXPECT_EQ(PhysicalContextType::CPU,
              terminus.toPhysicalContextType(PLDM_ENTITY_PROC_MODULE));
    EXPECT_EQ(PhysicalContextType::CPU,
              terminus.toPhysicalContextType(PLDM_ENTITY_PROC_IO_MODULE));
    EXPECT_EQ(PhysicalContextType::VoltageRegulator,
              terminus.toPhysicalContextType(PLDM_ENTITY_DC_DC_CONVERTER));
    EXPECT_EQ(PhysicalContextType::NetworkingDevice,
              terminus.toPhysicalContextType(PLDM_ENTITY_NETWORK_CONTROLLER));
    EXPECT_EQ(PhysicalContextType::SystemBoard,
              terminus.toPhysicalContextType(PLDM_ENTITY_SYS_BOARD));
}

TEST_F(TerminusTest, privateGetterAndUpdateAssociationsCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000156");
    Terminus terminus(0x56, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusPrivate");
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis56";

    const std::string boardPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis56/board0"};
    terminus.inventories.emplace_back(boardPath, PLDM_ENTITY_SYS_BOARD, 1);
    terminus.inventoryParentMap[boardPath] = terminus.systemInventoryPath;
    EntityInfo containerEntity{overallSystemCotainerId, PLDM_ENTITY_SYS_BOARD,
                               1};
    terminus.entityAssociations.emplace(
        1, std::make_pair(containerEntity, std::set<EntityInfo>{}));

    AuxiliaryNames pdrAuxNames{{{"en", "AuxSensor"}}};
    terminus.sensorAuxiliaryNamesTbl.emplace_back(
        std::make_shared<SensorAuxiliaryNames>(0x52, 1, pdrAuxNames));

    AuxiliaryNames overwriteAuxNames{{{"en", "OverwriteSensor"}}};
    terminus.sensorAuxNameOverwriteTbl[0x53] = std::make_tuple(
        overwriteAuxNames,
        "/xyz/openbmc_project/inventory/system/chassis/chassis53");
    terminus.sensorAuxNameOverwriteTbl[0x54] =
        std::make_tuple(overwriteAuxNames, "/tmp/not_inventory");

#ifdef OEM_NVIDIA
    std::vector<pldm::dbus::PathAssociation> associations{
        {"chassis", "all_states", terminus.systemInventoryPath}};
    terminus.sensorPortInfoOverwriteTbl[0x53] = std::make_tuple(
        PortType::UpstreamPort, std::string("PCIe"), 32000, associations);
    terminus.sensorEventInfoOverwriteTbl[0x53] =
        std::make_shared<pldm::utils::SensorEventInfo>(
            "CPU56", std::unordered_map<std::string, std::string>{
                         {"PLDM_SENSOR_UPPERCRITICAL", "EID56"}});
#endif

    auto overwriteNames = terminus.getSensorAuxiliaryNames(0x53);
    ASSERT_NE(nullptr, overwriteNames);
    EXPECT_TRUE(terminus.getInventoryPath(0x53).has_value());
    EXPECT_FALSE(terminus.getInventoryPath(0x54).has_value());

#ifdef OEM_NVIDIA
    EXPECT_NE(nullptr, terminus.getSensorPortInfo(0x53));
    EXPECT_EQ(nullptr, terminus.getSensorPortInfo(0xFF));
    EXPECT_NE(nullptr, terminus.getSensorEventInfo(0x53));
    EXPECT_EQ(nullptr, terminus.getSensorEventInfo(0xFF));
#endif

    std::string associationPath = terminus.systemInventoryPath;
    auto pdrNoAux = makeNumericSensorValuePdrStruct(0x51);
    auto pdrWithAux = makeNumericSensorValuePdrStruct(0x52);
    auto pdrOverwrite = makeNumericSensorValuePdrStruct(0x53);
    std::string noAuxName{"sensor_no_aux_56"};
    std::string auxName{"sensor_aux_56"};
    std::string overwriteName{"sensor_overwrite_56"};
    auto noAuxSensor =
        std::make_shared<NumericSensor>(terminus.getTid(), false, pdrNoAux,
                                        noAuxName, associationPath, nullptr);
    auto auxSensor =
        std::make_shared<NumericSensor>(terminus.getTid(), false, pdrWithAux,
                                        auxName, associationPath, nullptr);
    auto overwriteSensor = std::make_shared<NumericSensor>(
        terminus.getTid(), false, pdrOverwrite, overwriteName, associationPath,
        nullptr);
    terminus.numericSensors.emplace_back(noAuxSensor);
    terminus.numericSensors.emplace_back(auxSensor);
    terminus.numericSensors.emplace_back(overwriteSensor);

    auto effecterPdr = makeNumericEffecterValuePdrStruct(0x61);
    std::string effecterName{"effecter_56"};
    auto effecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, effecterPdr, effecterName, associationPath,
        terminusManager);
    terminus.numericEffecters.emplace_back(effecter);

    auto stateInfo = makeSimpleStateSetInfo();
    auto stateSensor =
        std::make_shared<StateSensor>(terminus.getTid(), false, 0x62, stateInfo,
                                      nullptr, associationPath, nullptr);
    auto stateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x63, stateInfo, nullptr, associationPath,
        terminusManager);
    terminus.stateSensors.emplace_back(stateSensor);
    terminus.stateEffecters.emplace_back(stateEffecter);

    terminusManager.numericSensorsWithoutAuxName = false;
    auto updateRc = stdexec::sync_wait(terminus.updateAssociations());
    ASSERT_TRUE(updateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateRc));

    EXPECT_EQ(nullptr, noAuxSensor->valueIntf);
    EXPECT_NE(std::string::npos, auxSensor->path.find("TerminusPrivate_"));
    EXPECT_NE(std::string::npos, overwriteSensor->path.find("OverwriteSensor"));
}

#ifdef OEM_NVIDIA
TEST_F(TerminusTest, nvidiaInitTerminusPowerCapAndStorageCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000157");
    Terminus terminus(0x57, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis57"};

    auto numericEffecterPdr = makeNumericEffecterValuePdrStruct(0x710);
    std::string numericEffecterName{"oem_numeric_effecter_710"};
    auto numericEffecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, numericEffecterPdr, numericEffecterName,
        associationPath, terminusManager);
    terminus.numericEffecters.emplace_back(numericEffecter);

    auto stateInfo = makeSingleStateSetInfo(PLDM_ENTITY_SYS_BOARD,
                                            PLDM_STATESET_ID_BOOT_REQUEST);
    auto stateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x711, stateInfo, nullptr, associationPath,
        terminusManager);
    terminus.stateEffecters.emplace_back(stateEffecter);

    nvidia::nvidia_oem_effecter_powercap_pdr powerCapPdr{};
    powerCapPdr.terminus_handle = 1;
    powerCapPdr.oem_pdr_type = static_cast<uint8_t>(
        nvidia::NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_POWERCAP);
    powerCapPdr.oem_effecter_powercap = static_cast<uint8_t>(
        nvidia::OemPowerCapPersistence::OEM_POWERCAP_TDP_NONVOLATILE);
    powerCapPdr.associated_effecterid = 0x710;

    nvidia::nvidia_oem_effecter_storage_pdr storagePdr{};
    storagePdr.terminus_handle = 1;
    storagePdr.oem_pdr_type = static_cast<uint8_t>(
        nvidia::NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_STORAGE);
    storagePdr.oem_effecter_storage = static_cast<uint8_t>(
        nvidia::OemStorageSecureState::OEM_STORAGE_SECURE_VARIABLE);
    storagePdr.associated_effecterid = 0x711;

    auto truncatedPdrData = structToBytes(powerCapPdr);
    truncatedPdrData.resize(sizeof(nvidia::nvidia_oem_pdr));

    nvidia::nvidia_oem_pdr unknownTypePdr{};
    unknownTypePdr.terminus_handle = 1;
    unknownTypePdr.oem_pdr_type = 0xFF;

    terminus.oemPdrs.emplace_back(static_cast<uint32_t>(0xFFFF),
                                  static_cast<OemRecordId>(1),
                                  structToBytes(powerCapPdr));
    terminus.oemPdrs.emplace_back(
        nvidia::NvidiaIana, static_cast<OemRecordId>(2), truncatedPdrData);
    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(3),
                                  structToBytes(powerCapPdr));
    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(4),
                                  structToBytes(storagePdr));
    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(5),
                                  structToBytes(unknownTypePdr));

    nvidia::nvidiaInitTerminus(terminus);

    ASSERT_EQ(1u, numericEffecter->oemIntfs.size());
    auto persistenceIntf =
        std::dynamic_pointer_cast<nvidia::OemPersistenceIntf>(
            numericEffecter->oemIntfs[0]);
    ASSERT_NE(nullptr, persistenceIntf);
    EXPECT_TRUE(persistenceIntf->persistent());

    ASSERT_EQ(1u, stateEffecter->oemIntfs.size());
    auto* storageIntf =
        dynamic_cast<nvidia::OemStorageIntf*>(stateEffecter->oemIntfs[0].get());
    ASSERT_NE(nullptr, storageIntf);
    EXPECT_TRUE(storageIntf->secure());
}

TEST_F(TerminusTest, nvidiaRemoteDebugAndStaticPowerHintCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000158");
    Terminus terminus(0x58, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis58"};

    auto debugEffecterInfo = makeDebugStateSetInfo(PLDM_ENTITY_SYS_BOARD, 6);
    auto debugSensorInfo = makeDebugStateSetInfo(PLDM_ENTITY_SYS_BOARD, 6);
    auto debugStateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x720, debugEffecterInfo, nullptr,
        associationPath, terminusManager);
    auto debugStateSensor = std::make_shared<StateSensor>(
        terminus.getTid(), false, 0x721, debugSensorInfo, nullptr,
        associationPath, nullptr);
    terminus.stateEffecters.emplace_back(debugStateEffecter);
    terminus.stateSensors.emplace_back(debugStateSensor);

    auto createNumericEffecter =
        [&](uint16_t effecterId, uint8_t baseUnit, uint8_t minValue,
            uint8_t maxValue, const std::string& effecterName) {
            auto pdr = makeNumericEffecterValuePdrStruct(effecterId);
            pdr->base_unit = baseUnit;
            pdr->min_settable.value_u8 = minValue;
            pdr->max_settable.value_u8 = maxValue;
            std::string name = effecterName;
            auto effecter = std::make_shared<NumericEffecter>(
                terminus.getTid(), false, pdr, name, associationPath,
                terminusManager);
            terminus.numericEffecters.emplace_back(effecter);
            return effecter;
        };

    auto remoteDebugNumericEffecter = createNumericEffecter(
        0x722, PLDM_SENSOR_UNIT_MINUTES, 1, 60, "remote_debug_timeout");
    auto staticPowerTemperatureEffecter = createNumericEffecter(
        0x723, PLDM_SENSOR_UNIT_DEGRESS_C, 10, 80, "static_power_temp");
    auto staticPowerWorkloadEffecter = createNumericEffecter(
        0x724, PLDM_SENSOR_UNIT_NONE, 1, 100, "static_power_workload");
    auto staticPowerClockEffecter = createNumericEffecter(
        0x725, PLDM_SENSOR_UNIT_HERTZ, 20, 200, "static_power_clock");
    auto staticPowerPowerEffecter = createNumericEffecter(
        0x726, PLDM_SENSOR_UNIT_WATTS, 5, 250, "static_power_power");

    nvidia::nvidiaInitTerminus(terminus);

    ASSERT_EQ(1u, debugStateEffecter->oemIntfs.size());
    auto* remoteDebugIntf = dynamic_cast<oem_nvidia::OemRemoteDebugIntf*>(
        debugStateEffecter->oemIntfs[0].get());
    ASSERT_NE(nullptr, remoteDebugIntf);

    debugStateSensor->stateSets[0]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_ENABLED);
    debugStateSensor->stateSets[1]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_DISABLED);
    debugStateSensor->stateSets[2]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_OFFLINE);
    debugStateSensor->stateSets[3]->setValue(0xFE);
    debugStateSensor->stateSets[4]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_ENABLED);
    debugStateSensor->stateSets[5]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_ENABLED);
    for (auto& stateSet : debugStateEffecter->stateSets)
    {
        stateSet->setOpState(EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING);
    }
    debugStateEffecter->stateSets[4]->setOpState(
        EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING);

    EXPECT_EQ(oem_nvidia::DebugState::Enabled, remoteDebugIntf->jtagDebug());
    EXPECT_EQ(oem_nvidia::DebugState::Disabled, remoteDebugIntf->deviceDebug());
    EXPECT_EQ(oem_nvidia::DebugState::Offline,
              remoteDebugIntf->securePrivilegeNonInvasiveDebug());
    EXPECT_EQ(oem_nvidia::DebugState::Unknown,
              remoteDebugIntf->securePrivilegeInvasiveDebug());
    EXPECT_EQ(oem_nvidia::DebugState::Pending,
              remoteDebugIntf->nonInvasiveDebug());
    EXPECT_EQ(oem_nvidia::DebugState::Enabled,
              remoteDebugIntf->invasiveDebug());
    EXPECT_EQ(0, remoteDebugIntf->toCompId(oem_nvidia::DebugPolicy::JtagDebug));
    EXPECT_EQ(255, remoteDebugIntf->toCompId(
                       static_cast<oem_nvidia::DebugPolicy>(0xFF)));
    EXPECT_EQ(oem_nvidia::DebugState::Unknown,
              remoteDebugIntf->toDebugState(0xFF));

    remoteDebugIntf->timeout(17, true);
    (void)remoteDebugIntf->timeout();

    EXPECT_THROW(
        remoteDebugIntf->enable({static_cast<oem_nvidia::DebugPolicy>(0xFF)}),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);
    EXPECT_THROW(
        remoteDebugIntf->disable({static_cast<oem_nvidia::DebugPolicy>(0xFF)}),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);
    EXPECT_THROW(
        remoteDebugIntf->enable(
            {oem_nvidia::DebugPolicy::SecurePrivilegeNonInvasiveDebug}),
        sdbusplus::xyz::openbmc_project::Common::Error::NotAllowed);

    debugStateSensor->stateSets[2]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_ENABLED);
    EXPECT_NO_THROW(remoteDebugIntf->enable(
        {oem_nvidia::DebugPolicy::JtagDebug,
         oem_nvidia::DebugPolicy::DeviceDebug}));
    EXPECT_NO_THROW(remoteDebugIntf->disable(
        {oem_nvidia::DebugPolicy::JtagDebug,
         oem_nvidia::DebugPolicy::DeviceDebug}));

    ASSERT_EQ(1u, staticPowerPowerEffecter->oemIntfs.size());
    auto staticPowerIntf = std::dynamic_pointer_cast<OemStaticPowerHintInft>(
        staticPowerPowerEffecter->oemIntfs[0]);
    ASSERT_NE(nullptr, staticPowerIntf);

    const auto maxClock = staticPowerIntf->maxCpuClockFrequency();
    const auto minClock = staticPowerIntf->minCpuClockFrequency();
    const auto maxWorkload = staticPowerIntf->maxWorkloadFactor();
    const auto minWorkload = staticPowerIntf->minWorkloadFactor();
    const auto maxTemperature = staticPowerIntf->maxTemperature();
    const auto minTemperature = staticPowerIntf->minTemperature();

    EXPECT_GT(maxClock, minClock);
    EXPECT_GT(maxWorkload, minWorkload);
    EXPECT_GT(maxTemperature, minTemperature);

    EXPECT_THROW(
        staticPowerIntf->estimatePower(maxClock + 1, minWorkload,
                                       minTemperature),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);
    EXPECT_THROW(
        staticPowerIntf->estimatePower(minClock, maxWorkload + 1,
                                       minTemperature),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);
    EXPECT_THROW(
        staticPowerIntf->estimatePower(minClock, minWorkload,
                                       maxTemperature + 1),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);

    const double validClock = (maxClock + minClock) / 2.0;
    const double validWorkload = (maxWorkload + minWorkload) / 2.0;
    const double validTemperature = (maxTemperature + minTemperature) / 2.0;

    EXPECT_NO_THROW(staticPowerIntf->estimatePower(validClock, validWorkload,
                                                   validTemperature));
    runEventLoopForMilliseconds(10);
    try
    {
        staticPowerIntf->estimatePower(validClock, validWorkload,
                                       validTemperature);
    }
    catch (const sdbusplus::xyz::openbmc_project::Common::Error::Unavailable&)
    {}

    EXPECT_NE(nullptr, remoteDebugNumericEffecter);
    EXPECT_NE(nullptr, staticPowerTemperatureEffecter);
    EXPECT_NE(nullptr, staticPowerWorkloadEffecter);
    EXPECT_NE(nullptr, staticPowerClockEffecter);
}

TEST_F(TerminusTest, nvidiaUpdateAssociationsOemCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000159");
    Terminus terminus(0x59, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis59"};

    StateSetData linkStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_LINKSTATE),
                        PossibleStates{PLDM_STATESET_LINK_STATE_DISCONNECTED,
                                       PLDM_STATESET_LINK_STATE_CONNECTED});
    StateSetData performanceStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_PERFORMANCE),
                        PossibleStates{PLDM_STATESET_PERFORMANCE_NORMAL,
                                       PLDM_STATESET_PERFORMANCE_THROTTLED});

    auto createStateSensor = [&](uint16_t sensorId, EntityType entityType,
                                 const StateSetData& stateSetData) {
        StateSetInfo info =
            std::make_tuple(EntityInfo{1, entityType, 1},
                            std::vector<StateSetData>{stateSetData});
        auto sensor = std::make_shared<StateSensor>(
            terminus.getTid(), false, sensorId, info, nullptr, associationPath,
            nullptr);
        terminus.stateSensors.emplace_back(sensor);
        return sensor;
    };

    auto ethSensor =
        createStateSensor(0x801, PLDM_ENTITY_ETHERNET, linkStateData);
    auto ethSensorFallback =
        createStateSensor(0x802, PLDM_ENTITY_ETHERNET, linkStateData);
    auto ibSensor =
        createStateSensor(0x803, PLDM_ENTITY_INFINIBAND, linkStateData);
    auto memSensorCpu0 = createStateSensor(0x804, PLDM_ENTITY_MEMORY_CONTROLLER,
                                           performanceStateData);
    auto memSensorUnknown = createStateSensor(
        0x805, PLDM_ENTITY_MEMORY_CONTROLLER, performanceStateData);

    ethSensor->setInventoryPaths({associationPath}, false);
    ethSensorFallback->setInventoryPaths({associationPath}, false);
    ibSensor->setInventoryPaths({associationPath}, false);
    memSensorCpu0->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis59/CPU_0"},
        false);
    memSensorUnknown->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis59/cpu_unknown"},
        false);

    std::vector<pldm::dbus::PathAssociation> portAssociations{
        {"chassis", "all_states", associationPath},
        {"associated_port", "associated_port",
         "/xyz/openbmc_project/inventory/system/fabrics/fabric0/port0"}};
    const std::string ethernetProtocol{
        "xyz.openbmc_project.Inventory.Decorator.PortInfo.PortProtocol.Ethernet"};
    terminus.sensorPortInfoOverwriteTbl[0x801] = std::make_tuple(
        PortType::UpstreamPort, ethernetProtocol, 25000, portAssociations);
    terminus.sensorPortInfoOverwriteTbl[0x802] = std::make_tuple(
        PortType::BidirectionalPort, std::string(""), 10000, portAssociations);
    terminus.sensorPortInfoOverwriteTbl[0x803] = std::make_tuple(
        PortType::DownstreamPort, std::string(""), 50000, portAssociations);

    auto sensorEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    sensorEventInfo->impactedComponent = "CoverageComponent";
    sensorEventInfo->eventIdsMap["LinkDown"] = "ResourceEvent.1.0.LinkDown";
    terminus.sensorEventInfoOverwriteTbl[0x801] = sensorEventInfo;
    terminus.sensorEventInfoOverwriteTbl[0x803] = sensorEventInfo;

    auto numericSensorPdr = makeNumericSensorValuePdrStruct(0x901);
    std::string numericSensorName{"oem_numeric_sensor_901"};
    auto numericSensor = std::make_shared<NumericSensor>(
        terminus.getTid(), false, numericSensorPdr, numericSensorName,
        associationPath, nullptr);
    terminus.numericSensors.emplace_back(numericSensor);
    terminus.sensorEventInfoOverwriteTbl[0x901] = sensorEventInfo;

    std::string switchType{
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet"};
    std::vector<std::string> switchProtocols{
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet",
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.PCIe"};
    std::vector<pldm::dbus::PathAssociation> switchAssociations{
        {"chassis", "all_states", associationPath}};
    terminus.switchBandwidthSensor =
        std::make_shared<oem_nvidia::SwitchBandwidthSensor>(
            terminus.getTid(), "switch_bandwidth_cov", switchType,
            switchProtocols, switchAssociations);

    auto updateRc =
        stdexec::sync_wait(nvidia::nvidiaUpdateAssociations(terminus));
    ASSERT_TRUE(updateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateRc));

    auto ethStateSet = std::dynamic_pointer_cast<StateSetEthIBPortLinkState>(
        ethSensor->stateSets[0]);
    auto ethFallbackStateSet =
        std::dynamic_pointer_cast<StateSetEthIBPortLinkState>(
            ethSensorFallback->stateSets[0]);
    auto ibStateSet = std::dynamic_pointer_cast<StateSetEthIBPortLinkState>(
        ibSensor->stateSets[0]);
    ASSERT_NE(nullptr, ethStateSet);
    ASSERT_NE(nullptr, ethFallbackStateSet);
    ASSERT_NE(nullptr, ibStateSet);
    EXPECT_TRUE(ethStateSet->isDerivedSensorAssociated());
    EXPECT_TRUE(ethFallbackStateSet->isDerivedSensorAssociated());
    EXPECT_TRUE(ibStateSet->isDerivedSensorAssociated());
    EXPECT_EQ("switch_bandwidth_cov",
              terminus.switchBandwidthSensor->getSensorName());
    EXPECT_GT(terminus.switchBandwidthSensor->switchIntf->maxBandwidth(), 0);

    EXPECT_NE(nullptr, ethSensor->getSensorEventInfo());
    EXPECT_NE(nullptr, ibSensor->getSensorEventInfo());
    EXPECT_NE(nullptr, numericSensor->getSensorEventInfo());

    auto updateSecondRc =
        stdexec::sync_wait(nvidia::nvidiaUpdateAssociations(terminus));
    ASSERT_TRUE(updateSecondRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateSecondRc));

    auto memPerformanceStateSet =
        std::dynamic_pointer_cast<StateSetMemoryPerformance>(
            memSensorCpu0->stateSets[0]);
    ASSERT_NE(nullptr, memPerformanceStateSet);
    memPerformanceStateSet->updateShmemReading("Value");
}

TEST_F(TerminusTest, nvidiaEnergyCountPdrParserCoverageMatrix)
{
    auto createVendorData = [](uint8_t sensorDataSize) {
        pldm_oem_energycount_numeric_sensor_value_pdr pdr{};
        pdr.terminus_handle = 1;
        pdr.nvidia_oem_pdr_type = static_cast<uint8_t>(
            nvidia::NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_SENSOR_ENERGYCOUNT);
        pdr.sensor_id = 0x77;
        pdr.entity_type = PLDM_ENTITY_SYS_BOARD;
        pdr.entity_instance_num = 1;
        pdr.container_id = 1;
        pdr.base_unit = PLDM_SENSOR_UNIT_WATTS;
        pdr.sensor_data_size = sensorDataSize;
        pdr.update_interval = 1.0f;
        switch (sensorDataSize)
        {
            case PLDM_SENSOR_DATA_SIZE_UINT8:
            case PLDM_SENSOR_DATA_SIZE_SINT8:
                pdr.max_readable.value_u8 = 0x7F;
                pdr.min_readable.value_u8 = 0x01;
                break;
            case PLDM_SENSOR_DATA_SIZE_UINT16:
            case PLDM_SENSOR_DATA_SIZE_SINT16:
                pdr.max_readable.value_u16 = 0x1234;
                pdr.min_readable.value_u16 = 0x10;
                break;
            case PLDM_SENSOR_DATA_SIZE_UINT32:
            case PLDM_SENSOR_DATA_SIZE_SINT32:
                pdr.max_readable.value_u32 = 0x12345678;
                pdr.min_readable.value_u32 = 0x1000;
                break;
            case PLDM_SENSOR_DATA_SIZE_UINT64:
            case PLDM_SENSOR_DATA_SIZE_SINT64:
                pdr.max_readable.value_u64 = 0x123456789ABCDEF0ull;
                pdr.min_readable.value_u64 = 0x10000ull;
                break;
            default:
                break;
        }
        return structToBytes(pdr);
    };

    const std::array<uint8_t, 9> sensorDataSizes{
        PLDM_SENSOR_DATA_SIZE_UINT8,
        PLDM_SENSOR_DATA_SIZE_SINT8,
        PLDM_SENSOR_DATA_SIZE_UINT16,
        PLDM_SENSOR_DATA_SIZE_SINT16,
        PLDM_SENSOR_DATA_SIZE_UINT32,
        PLDM_SENSOR_DATA_SIZE_SINT32,
        PLDM_SENSOR_DATA_SIZE_UINT64,
        PLDM_SENSOR_DATA_SIZE_SINT64,
        0xFF};

    for (auto sensorDataSize : sensorDataSizes)
    {
        auto vendorData = createVendorData(sensorDataSize);
        auto parsed = nvidia::parseOEMEnergyCountNumericSensorPDR(vendorData);
        ASSERT_NE(nullptr, parsed);
    }

    std::vector<uint8_t> tooSmall(
        PLDM_PDR_OEM_ENERGYCOUNT_NUMERIC_SENSOR_PDR_MIN_LENGTH - 1, 0);
    EXPECT_EQ(nullptr, nvidia::parseOEMEnergyCountNumericSensorPDR(tooSmall));

    auto truncatedUint64VendorData =
        createVendorData(PLDM_SENSOR_DATA_SIZE_UINT64);
    truncatedUint64VendorData.resize(
        PLDM_PDR_OEM_ENERGYCOUNT_NUMERIC_SENSOR_PDR_MIN_LENGTH);
    EXPECT_EQ(nullptr, nvidia::parseOEMEnergyCountNumericSensorPDR(
                           truncatedUint64VendorData));
}

TEST_F(TerminusTest, switchBandwidthSensorCoverage)
{
    std::string switchType{
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet"};
    std::vector<std::string> switchProtocols{
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet",
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.PCIe"};
    std::vector<pldm::dbus::PathAssociation> associations{
        {"chassis", "all_states",
         "/xyz/openbmc_project/inventory/system/chassis/chassis60"}};

    oem_nvidia::SwitchBandwidthSensor sensor(
        0x60, "switch_bandwidth_sensor_cov", switchType, switchProtocols,
        associations);
    EXPECT_EQ("switch_bandwidth_sensor_cov", sensor.getSensorName());

    sensor.updateCurrentBandwidth(std::numeric_limits<double>::quiet_NaN(),
                                  12.5);
    sensor.updateCurrentBandwidth(2.5,
                                  std::numeric_limits<double>::quiet_NaN());
    sensor.updateMaxBandwidth(100.0);
    sensor.addAssociatedSensorID(0x1234);
    sensor.updateOnSharedMemory();

    EXPECT_GT(sensor.switchIntf->maxBandwidth(), 0);
=======
    EXPECT_TRUE(terminus.parsePDRs());
    EXPECT_EQ(cases.size() + 1, terminus.numericEffecterPdrs.size());
    EXPECT_EQ(cases.size() + 1, terminus.numericEffecters.size());
    EXPECT_NE(std::string::npos,
              terminus.numericEffecters.front()->path.find("TerminusEffecter"));
}

TEST_F(TerminusTest, addStateSensorAndEffecterCoverage)
{
    constexpr uint16_t sensorId = 0x211;
    constexpr uint16_t effecterId = 0x311;
    std::string uuid("00000000-0000-0000-0000-0000000000F0");
    Terminus terminus(0x10, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusState");

    auto sensorAuxPdr = makeSensorAuxNamePdr(sensorId);
    auto stateSensorPdr = makeStateSensorPdr(
        sensorId, PLDM_ENTITY_SYS_BOARD, PLDM_STATESET_ID_HEALTHSTATE, true);
    auto effecterAuxPdr =
        makeAuxNamePdr(effecterId, PLDM_EFFECTER_AUXILIARY_NAMES_PDR);
    auto stateEffecterPdr = makeStateEffecterPdr(
        effecterId, PLDM_ENTITY_SYS_BOARD, PLDM_STATESET_ID_BOOT_REQUEST);

    terminus.pdrs.emplace_back(sensorAuxPdr);
    terminus.pdrs.emplace_back(stateSensorPdr);
    terminus.pdrs.emplace_back(effecterAuxPdr);
    terminus.pdrs.emplace_back(stateEffecterPdr);

    ASSERT_TRUE(terminus.parsePDRs());
    ASSERT_EQ(1u, terminus.stateSensors.size());
    ASSERT_EQ(1u, terminus.stateEffecters.size());
}

TEST_F(TerminusTest, privateFindInventoryAndPhysicalContextCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000155");
    Terminus terminus(0x55, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis55";
    terminus.setInstance(7);

    const std::string cpu0{
        "/xyz/openbmc_project/inventory/system/chassis/chassis55/cpu0"};
    const std::string cpu1{
        "/xyz/openbmc_project/inventory/system/chassis/chassis55/cpu1"};
    const std::string dimm0{
        "/xyz/openbmc_project/inventory/system/chassis/chassis55/dimm0"};
    const std::string dimm1{
        "/xyz/openbmc_project/inventory/system/chassis/chassis55/dimm1"};

    terminus.inventories.emplace_back(cpu0, PLDM_ENTITY_PROC, 7);
    terminus.inventories.emplace_back(cpu1, PLDM_ENTITY_PROC, 7);
    terminus.inventories.emplace_back(dimm0, PLDM_ENTITY_MEMORY_CONTROLLER, 2);
    terminus.inventories.emplace_back(dimm1, PLDM_ENTITY_MEMORY_CONTROLLER, 2);
    terminus.inventoryParentMap[dimm0] = cpu0;
    terminus.inventoryParentMap[dimm1] = cpu1;

    EntityInfo containerEntity{overallSystemCotainerId, PLDM_ENTITY_PROC, 1};
    terminus.entityAssociations.emplace(
        1, std::make_pair(containerEntity, std::set<EntityInfo>{}));

    auto overallPaths = terminus.findInventory(overallSystemCotainerId, false);
    ASSERT_EQ(1u, overallPaths.size());
    EXPECT_EQ(terminus.systemInventoryPath, overallPaths.front());

    auto unknownContainerPaths =
        terminus.findInventory(static_cast<ContainerID>(0x9FFF), false);
    ASSERT_EQ(1u, unknownContainerPaths.size());
    EXPECT_EQ(terminus.systemInventoryPath, unknownContainerPaths.front());

    EntityInfo cpuEntity{1, PLDM_ENTITY_PROC, 1};
    auto cpuPaths = terminus.findInventory(cpuEntity, false);
    EXPECT_EQ(2u, cpuPaths.size());

    terminus.entities.clear();
    EntityInfo dimmEntity{1, PLDM_ENTITY_MEMORY_CONTROLLER, 2};
    auto dimmPaths = terminus.findInventory(dimmEntity, false);
    EXPECT_EQ(2u, dimmPaths.size());

    terminus.entities.clear();
    terminus.inventoryParentMap.clear();
    auto dimmFallback = terminus.findInventory(dimmEntity, false);
    EXPECT_EQ(1u, dimmFallback.size());
    EXPECT_EQ(dimm0, dimmFallback.front());

    terminus.entities.clear();
    EntityInfo missingEntity{1, PLDM_ENTITY_NETWORK_CONTROLLER, 99};
    auto closest = terminus.findInventory(missingEntity, true);
    EXPECT_EQ(2u, closest.size());

    terminus.entities.clear();
    auto missingWithoutClosest = terminus.findInventory(missingEntity, false);
    EXPECT_TRUE(missingWithoutClosest.empty());

    EXPECT_EQ(PhysicalContextType::Memory,
              terminus.toPhysicalContextType(PLDM_ENTITY_MEMORY_CONTROLLER));
    EXPECT_EQ(PhysicalContextType::CPU,
              terminus.toPhysicalContextType(PLDM_ENTITY_PROC_MODULE));
    EXPECT_EQ(PhysicalContextType::CPU,
              terminus.toPhysicalContextType(PLDM_ENTITY_PROC_IO_MODULE));
    EXPECT_EQ(PhysicalContextType::VoltageRegulator,
              terminus.toPhysicalContextType(PLDM_ENTITY_DC_DC_CONVERTER));
    EXPECT_EQ(PhysicalContextType::NetworkingDevice,
              terminus.toPhysicalContextType(PLDM_ENTITY_NETWORK_CONTROLLER));
    EXPECT_EQ(PhysicalContextType::SystemBoard,
              terminus.toPhysicalContextType(PLDM_ENTITY_SYS_BOARD));
}

TEST_F(TerminusTest, privateGetterAndUpdateAssociationsCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000156");
    Terminus terminus(0x56, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    terminus.setTerminusName("TerminusPrivate");
    terminus.systemInventoryPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis56";

    const std::string boardPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis56/board0"};
    terminus.inventories.emplace_back(boardPath, PLDM_ENTITY_SYS_BOARD, 1);
    terminus.inventoryParentMap[boardPath] = terminus.systemInventoryPath;
    EntityInfo containerEntity{overallSystemCotainerId, PLDM_ENTITY_SYS_BOARD,
                               1};
    terminus.entityAssociations.emplace(
        1, std::make_pair(containerEntity, std::set<EntityInfo>{}));

    AuxiliaryNames pdrAuxNames{{{"en", "AuxSensor"}}};
    terminus.sensorAuxiliaryNamesTbl.emplace_back(
        std::make_shared<SensorAuxiliaryNames>(0x52, 1, pdrAuxNames));

    AuxiliaryNames overwriteAuxNames{{{"en", "OverwriteSensor"}}};
    terminus.sensorAuxNameOverwriteTbl[0x53] = std::make_tuple(
        overwriteAuxNames,
        "/xyz/openbmc_project/inventory/system/chassis/chassis53");
    terminus.sensorAuxNameOverwriteTbl[0x54] =
        std::make_tuple(overwriteAuxNames, "/tmp/not_inventory");

#ifdef OEM_NVIDIA
    std::vector<pldm::dbus::PathAssociation> associations{
        {"chassis", "all_states", terminus.systemInventoryPath}};
    terminus.sensorPortInfoOverwriteTbl[0x53] = std::make_tuple(
        PortType::UpstreamPort, std::string("PCIe"), 32000, associations);
    terminus.sensorEventInfoOverwriteTbl[0x53] =
        std::make_shared<pldm::utils::SensorEventInfo>(
            "CPU56", std::unordered_map<std::string, std::string>{
                         {"PLDM_SENSOR_UPPERCRITICAL", "EID56"}});
#endif

    auto overwriteNames = terminus.getSensorAuxiliaryNames(0x53);
    ASSERT_NE(nullptr, overwriteNames);
    EXPECT_TRUE(terminus.getInventoryPath(0x53).has_value());
    EXPECT_FALSE(terminus.getInventoryPath(0x54).has_value());

#ifdef OEM_NVIDIA
    EXPECT_NE(nullptr, terminus.getSensorPortInfo(0x53));
    EXPECT_EQ(nullptr, terminus.getSensorPortInfo(0xFF));
    EXPECT_NE(nullptr, terminus.getSensorEventInfo(0x53));
    EXPECT_EQ(nullptr, terminus.getSensorEventInfo(0xFF));
#endif

    std::string associationPath = terminus.systemInventoryPath;
    auto pdrNoAux = makeNumericSensorValuePdrStruct(0x51);
    auto pdrWithAux = makeNumericSensorValuePdrStruct(0x52);
    auto pdrOverwrite = makeNumericSensorValuePdrStruct(0x53);
    std::string noAuxName{"sensor_no_aux_56"};
    std::string auxName{"sensor_aux_56"};
    std::string overwriteName{"sensor_overwrite_56"};
    auto noAuxSensor =
        std::make_shared<NumericSensor>(terminus.getTid(), false, pdrNoAux,
                                        noAuxName, associationPath, nullptr);
    auto auxSensor =
        std::make_shared<NumericSensor>(terminus.getTid(), false, pdrWithAux,
                                        auxName, associationPath, nullptr);
    auto overwriteSensor = std::make_shared<NumericSensor>(
        terminus.getTid(), false, pdrOverwrite, overwriteName, associationPath,
        nullptr);
    terminus.numericSensors.emplace_back(noAuxSensor);
    terminus.numericSensors.emplace_back(auxSensor);
    terminus.numericSensors.emplace_back(overwriteSensor);

    auto effecterPdr = makeNumericEffecterValuePdrStruct(0x61);
    std::string effecterName{"effecter_56"};
    auto effecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, effecterPdr, effecterName, associationPath,
        terminusManager);
    terminus.numericEffecters.emplace_back(effecter);

    auto stateInfo = makeSimpleStateSetInfo();
    auto stateSensor =
        std::make_shared<StateSensor>(terminus.getTid(), false, 0x62, stateInfo,
                                      nullptr, associationPath, nullptr);
    auto stateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x63, stateInfo, nullptr, associationPath,
        terminusManager);
    terminus.stateSensors.emplace_back(stateSensor);
    terminus.stateEffecters.emplace_back(stateEffecter);

    terminusManager.numericSensorsWithoutAuxName = false;
    auto updateRc = stdexec::sync_wait(terminus.updateAssociations());
    ASSERT_TRUE(updateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateRc));

    EXPECT_EQ(nullptr, noAuxSensor->valueIntf);
    EXPECT_NE(std::string::npos, auxSensor->path.find("TerminusPrivate_"));
    EXPECT_NE(std::string::npos, overwriteSensor->path.find("OverwriteSensor"));
}

#ifdef OEM_NVIDIA
TEST_F(TerminusTest, nvidiaInitTerminusPowerCapAndStorageCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000157");
    Terminus terminus(0x57, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis57"};

    auto numericEffecterPdr = makeNumericEffecterValuePdrStruct(0x710);
    std::string numericEffecterName{"oem_numeric_effecter_710"};
    auto numericEffecter = std::make_shared<NumericEffecter>(
        terminus.getTid(), false, numericEffecterPdr, numericEffecterName,
        associationPath, terminusManager);
    terminus.numericEffecters.emplace_back(numericEffecter);

    auto stateInfo = makeSingleStateSetInfo(PLDM_ENTITY_SYS_BOARD,
                                            PLDM_STATESET_ID_BOOT_REQUEST);
    auto stateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x711, stateInfo, nullptr, associationPath,
        terminusManager);
    terminus.stateEffecters.emplace_back(stateEffecter);

    nvidia::nvidia_oem_effecter_powercap_pdr powerCapPdr{};
    powerCapPdr.terminus_handle = 1;
    powerCapPdr.oem_pdr_type = static_cast<uint8_t>(
        nvidia::NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_POWERCAP);
    powerCapPdr.oem_effecter_powercap = static_cast<uint8_t>(
        nvidia::OemPowerCapPersistence::OEM_POWERCAP_TDP_NONVOLATILE);
    powerCapPdr.associated_effecterid = 0x710;

    nvidia::nvidia_oem_effecter_storage_pdr storagePdr{};
    storagePdr.terminus_handle = 1;
    storagePdr.oem_pdr_type = static_cast<uint8_t>(
        nvidia::NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_STORAGE);
    storagePdr.oem_effecter_storage = static_cast<uint8_t>(
        nvidia::OemStorageSecureState::OEM_STORAGE_SECURE_VARIABLE);
    storagePdr.associated_effecterid = 0x711;

    auto truncatedPdrData = structToBytes(powerCapPdr);
    truncatedPdrData.resize(sizeof(nvidia::nvidia_oem_pdr));

    nvidia::nvidia_oem_pdr unknownTypePdr{};
    unknownTypePdr.terminus_handle = 1;
    unknownTypePdr.oem_pdr_type = 0xFF;

    // OemRecordId must match the effecter IDs (0x710/0x711) because
    // processEffecterPowerCapPdr/processEffecterStoragePdr now compare
    // effecter->effecterId against the PLDM OEM PDR header's oemRecordId
    // (not the vendor-specific associated_effecterid).
    terminus.oemPdrs.emplace_back(static_cast<uint32_t>(0xFFFF),
                                  static_cast<OemRecordId>(0x710),
                                  structToBytes(powerCapPdr));
    terminus.oemPdrs.emplace_back(
        nvidia::NvidiaIana, static_cast<OemRecordId>(2), truncatedPdrData);
    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(0x710),
                                  structToBytes(powerCapPdr));
    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(0x711),
                                  structToBytes(storagePdr));
    terminus.oemPdrs.emplace_back(nvidia::NvidiaIana,
                                  static_cast<OemRecordId>(5),
                                  structToBytes(unknownTypePdr));

    nvidia::nvidiaInitTerminus(terminus);

    ASSERT_EQ(1u, numericEffecter->oemIntfs.size());
    auto persistenceIntf =
        std::dynamic_pointer_cast<nvidia::OemPersistenceIntf>(
            numericEffecter->oemIntfs[0]);
    ASSERT_NE(nullptr, persistenceIntf);
    EXPECT_TRUE(persistenceIntf->persistent());

    ASSERT_EQ(1u, stateEffecter->oemIntfs.size());
    auto* storageIntf =
        dynamic_cast<nvidia::OemStorageIntf*>(stateEffecter->oemIntfs[0].get());
    ASSERT_NE(nullptr, storageIntf);
    EXPECT_TRUE(storageIntf->secure());
}

TEST_F(TerminusTest, nvidiaRemoteDebugAndStaticPowerHintCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000158");
    Terminus terminus(0x58, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis58"};

    auto debugEffecterInfo = makeDebugStateSetInfo(PLDM_ENTITY_SYS_BOARD, 6);
    auto debugSensorInfo = makeDebugStateSetInfo(PLDM_ENTITY_SYS_BOARD, 6);
    auto debugStateEffecter = std::make_shared<StateEffecter>(
        terminus.getTid(), false, 0x720, debugEffecterInfo, nullptr,
        associationPath, terminusManager);
    auto debugStateSensor = std::make_shared<StateSensor>(
        terminus.getTid(), false, 0x721, debugSensorInfo, nullptr,
        associationPath, nullptr);
    terminus.stateEffecters.emplace_back(debugStateEffecter);
    terminus.stateSensors.emplace_back(debugStateSensor);

    auto createNumericEffecter =
        [&](uint16_t effecterId, uint8_t baseUnit, uint8_t minValue,
            uint8_t maxValue, const std::string& effecterName) {
            auto pdr = makeNumericEffecterValuePdrStruct(effecterId);
            pdr->base_unit = baseUnit;
            pdr->min_settable.value_u8 = minValue;
            pdr->max_settable.value_u8 = maxValue;
            std::string name = effecterName;
            auto effecter = std::make_shared<NumericEffecter>(
                terminus.getTid(), false, pdr, name, associationPath,
                terminusManager);
            terminus.numericEffecters.emplace_back(effecter);
            return effecter;
        };

    auto remoteDebugNumericEffecter = createNumericEffecter(
        0x722, PLDM_SENSOR_UNIT_MINUTES, 1, 60, "remote_debug_timeout");
    auto staticPowerTemperatureEffecter = createNumericEffecter(
        0x723, PLDM_SENSOR_UNIT_DEGRESS_C, 10, 80, "static_power_temp");
    auto staticPowerWorkloadEffecter = createNumericEffecter(
        0x724, PLDM_SENSOR_UNIT_NONE, 1, 100, "static_power_workload");
    auto staticPowerClockEffecter = createNumericEffecter(
        0x725, PLDM_SENSOR_UNIT_HERTZ, 20, 200, "static_power_clock");
    auto staticPowerPowerEffecter = createNumericEffecter(
        0x726, PLDM_SENSOR_UNIT_WATTS, 5, 250, "static_power_power");

    nvidia::nvidiaInitTerminus(terminus);

    ASSERT_EQ(1u, debugStateEffecter->oemIntfs.size());
    auto* remoteDebugIntf = dynamic_cast<oem_nvidia::OemRemoteDebugIntf*>(
        debugStateEffecter->oemIntfs[0].get());
    ASSERT_NE(nullptr, remoteDebugIntf);

    debugStateSensor->stateSets[0]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_ENABLED);
    debugStateSensor->stateSets[1]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_DISABLED);
    debugStateSensor->stateSets[2]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_OFFLINE);
    debugStateSensor->stateSets[3]->setValue(0xFE);
    debugStateSensor->stateSets[4]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_ENABLED);
    debugStateSensor->stateSets[5]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_ENABLED);
    for (auto& stateSet : debugStateEffecter->stateSets)
    {
        stateSet->setOpState(EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING);
    }
    debugStateEffecter->stateSets[4]->setOpState(
        EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING);

    EXPECT_EQ(oem_nvidia::DebugState::Enabled, remoteDebugIntf->jtagDebug());
    EXPECT_EQ(oem_nvidia::DebugState::Disabled, remoteDebugIntf->deviceDebug());
    EXPECT_EQ(oem_nvidia::DebugState::Offline,
              remoteDebugIntf->securePrivilegeNonInvasiveDebug());
    EXPECT_EQ(oem_nvidia::DebugState::Unknown,
              remoteDebugIntf->securePrivilegeInvasiveDebug());
    EXPECT_EQ(oem_nvidia::DebugState::Pending,
              remoteDebugIntf->nonInvasiveDebug());
    EXPECT_EQ(oem_nvidia::DebugState::Enabled,
              remoteDebugIntf->invasiveDebug());
    EXPECT_EQ(0, remoteDebugIntf->toCompId(oem_nvidia::DebugPolicy::JtagDebug));
    EXPECT_EQ(255, remoteDebugIntf->toCompId(
                       static_cast<oem_nvidia::DebugPolicy>(0xFF)));
    EXPECT_EQ(oem_nvidia::DebugState::Unknown,
              remoteDebugIntf->toDebugState(0xFF));

    remoteDebugIntf->timeout(17, true);
    (void)remoteDebugIntf->timeout();

    EXPECT_THROW(
        remoteDebugIntf->enable({static_cast<oem_nvidia::DebugPolicy>(0xFF)}),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);
    EXPECT_THROW(
        remoteDebugIntf->disable({static_cast<oem_nvidia::DebugPolicy>(0xFF)}),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);
    EXPECT_THROW(
        remoteDebugIntf->enable(
            {oem_nvidia::DebugPolicy::SecurePrivilegeNonInvasiveDebug}),
        sdbusplus::xyz::openbmc_project::Common::Error::NotAllowed);

    debugStateSensor->stateSets[2]->setValue(
        PLDM_STATE_SET_DEBUG_STATE_ENABLED);
    EXPECT_NO_THROW(remoteDebugIntf->enable(
        {oem_nvidia::DebugPolicy::JtagDebug,
         oem_nvidia::DebugPolicy::DeviceDebug}));
    EXPECT_NO_THROW(remoteDebugIntf->disable(
        {oem_nvidia::DebugPolicy::JtagDebug,
         oem_nvidia::DebugPolicy::DeviceDebug}));

    ASSERT_EQ(1u, staticPowerPowerEffecter->oemIntfs.size());
    auto staticPowerIntf = std::dynamic_pointer_cast<OemStaticPowerHintInft>(
        staticPowerPowerEffecter->oemIntfs[0]);
    ASSERT_NE(nullptr, staticPowerIntf);

    const auto maxClock = staticPowerIntf->maxCpuClockFrequency();
    const auto minClock = staticPowerIntf->minCpuClockFrequency();
    const auto maxWorkload = staticPowerIntf->maxWorkloadFactor();
    const auto minWorkload = staticPowerIntf->minWorkloadFactor();
    const auto maxTemperature = staticPowerIntf->maxTemperature();
    const auto minTemperature = staticPowerIntf->minTemperature();

    EXPECT_GT(maxClock, minClock);
    EXPECT_GT(maxWorkload, minWorkload);
    EXPECT_GT(maxTemperature, minTemperature);

    EXPECT_THROW(
        staticPowerIntf->estimatePower(maxClock + 1, minWorkload,
                                       minTemperature),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);
    EXPECT_THROW(
        staticPowerIntf->estimatePower(minClock, maxWorkload + 1,
                                       minTemperature),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);
    EXPECT_THROW(
        staticPowerIntf->estimatePower(minClock, minWorkload,
                                       maxTemperature + 1),
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument);

    const double validClock = (maxClock + minClock) / 2.0;
    const double validWorkload = (maxWorkload + minWorkload) / 2.0;
    const double validTemperature = (maxTemperature + minTemperature) / 2.0;

    EXPECT_NO_THROW(staticPowerIntf->estimatePower(validClock, validWorkload,
                                                   validTemperature));
    runEventLoopForMilliseconds(10);
    try
    {
        staticPowerIntf->estimatePower(validClock, validWorkload,
                                       validTemperature);
    }
    catch (const sdbusplus::xyz::openbmc_project::Common::Error::Unavailable&)
    {}

    EXPECT_NE(nullptr, remoteDebugNumericEffecter);
    EXPECT_NE(nullptr, staticPowerTemperatureEffecter);
    EXPECT_NE(nullptr, staticPowerWorkloadEffecter);
    EXPECT_NE(nullptr, staticPowerClockEffecter);
}

TEST_F(TerminusTest, nvidiaUpdateAssociationsOemCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000159");
    Terminus terminus(0x59, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis59"};

    StateSetData linkStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_LINKSTATE),
                        PossibleStates{PLDM_STATESET_LINK_STATE_DISCONNECTED,
                                       PLDM_STATESET_LINK_STATE_CONNECTED});
    StateSetData performanceStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_PERFORMANCE),
                        PossibleStates{PLDM_STATESET_PERFORMANCE_NORMAL,
                                       PLDM_STATESET_PERFORMANCE_THROTTLED});

    auto createStateSensor = [&](uint16_t sensorId, EntityType entityType,
                                 const StateSetData& stateSetData) {
        StateSetInfo info =
            std::make_tuple(EntityInfo{1, entityType, 1},
                            std::vector<StateSetData>{stateSetData});
        auto sensor = std::make_shared<StateSensor>(
            terminus.getTid(), false, sensorId, info, nullptr, associationPath,
            nullptr);
        terminus.stateSensors.emplace_back(sensor);
        return sensor;
    };

    auto ethSensor =
        createStateSensor(0x801, PLDM_ENTITY_ETHERNET, linkStateData);
    auto ethSensorFallback =
        createStateSensor(0x802, PLDM_ENTITY_ETHERNET, linkStateData);
    auto ibSensor =
        createStateSensor(0x803, PLDM_ENTITY_INFINIBAND, linkStateData);
    auto memSensorCpu0 = createStateSensor(0x804, PLDM_ENTITY_MEMORY_CONTROLLER,
                                           performanceStateData);
    auto memSensorUnknown = createStateSensor(
        0x805, PLDM_ENTITY_MEMORY_CONTROLLER, performanceStateData);

    ethSensor->setInventoryPaths({associationPath}, false);
    ethSensorFallback->setInventoryPaths({associationPath}, false);
    ibSensor->setInventoryPaths({associationPath}, false);
    memSensorCpu0->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis59/CPU_0"},
        false);
    memSensorUnknown->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis59/cpu_unknown"},
        false);

    std::vector<pldm::dbus::PathAssociation> portAssociations{
        {"chassis", "all_states", associationPath},
        {"associated_port", "associated_port",
         "/xyz/openbmc_project/inventory/system/fabrics/fabric0/port0"}};
    const std::string ethernetProtocol{
        "xyz.openbmc_project.Inventory.Decorator.PortInfo.PortProtocol.Ethernet"};
    terminus.sensorPortInfoOverwriteTbl[0x801] = std::make_tuple(
        PortType::UpstreamPort, ethernetProtocol, 25000, portAssociations);
    terminus.sensorPortInfoOverwriteTbl[0x802] = std::make_tuple(
        PortType::BidirectionalPort, std::string(""), 10000, portAssociations);
    terminus.sensorPortInfoOverwriteTbl[0x803] = std::make_tuple(
        PortType::DownstreamPort, std::string(""), 50000, portAssociations);

    auto sensorEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    sensorEventInfo->impactedComponent = "CoverageComponent";
    sensorEventInfo->eventIdsMap["LinkDown"] = "ResourceEvent.1.0.LinkDown";
    terminus.sensorEventInfoOverwriteTbl[0x801] = sensorEventInfo;
    terminus.sensorEventInfoOverwriteTbl[0x803] = sensorEventInfo;

    auto numericSensorPdr = makeNumericSensorValuePdrStruct(0x901);
    std::string numericSensorName{"oem_numeric_sensor_901"};
    auto numericSensor = std::make_shared<NumericSensor>(
        terminus.getTid(), false, numericSensorPdr, numericSensorName,
        associationPath, nullptr);
    terminus.numericSensors.emplace_back(numericSensor);
    terminus.sensorEventInfoOverwriteTbl[0x901] = sensorEventInfo;

    std::string switchType{
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet"};
    std::vector<std::string> switchProtocols{
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet",
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.PCIe"};
    std::vector<pldm::dbus::PathAssociation> switchAssociations{
        {"chassis", "all_states", associationPath}};
    terminus.switchBandwidthSensor =
        std::make_shared<oem_nvidia::SwitchBandwidthSensor>(
            terminus.getTid(), "switch_bandwidth_cov", switchType,
            switchProtocols, switchAssociations);

    auto updateRc =
        stdexec::sync_wait(nvidia::nvidiaUpdateAssociations(terminus));
    ASSERT_TRUE(updateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateRc));

    auto ethStateSet = std::dynamic_pointer_cast<StateSetEthIBPortLinkState>(
        ethSensor->stateSets[0]);
    auto ethFallbackStateSet =
        std::dynamic_pointer_cast<StateSetEthIBPortLinkState>(
            ethSensorFallback->stateSets[0]);
    auto ibStateSet = std::dynamic_pointer_cast<StateSetEthIBPortLinkState>(
        ibSensor->stateSets[0]);
    ASSERT_NE(nullptr, ethStateSet);
    ASSERT_NE(nullptr, ethFallbackStateSet);
    ASSERT_NE(nullptr, ibStateSet);
    EXPECT_TRUE(ethStateSet->isDerivedSensorAssociated());
    EXPECT_TRUE(ethFallbackStateSet->isDerivedSensorAssociated());
    EXPECT_TRUE(ibStateSet->isDerivedSensorAssociated());
    EXPECT_EQ("switch_bandwidth_cov",
              terminus.switchBandwidthSensor->getSensorName());
    EXPECT_GT(terminus.switchBandwidthSensor->switchIntf->maxBandwidth(), 0);

    EXPECT_NE(nullptr, ethSensor->getSensorEventInfo());
    EXPECT_NE(nullptr, ibSensor->getSensorEventInfo());
    EXPECT_NE(nullptr, numericSensor->getSensorEventInfo());

    auto updateSecondRc =
        stdexec::sync_wait(nvidia::nvidiaUpdateAssociations(terminus));
    ASSERT_TRUE(updateSecondRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*updateSecondRc));

    auto memPerformanceStateSet =
        std::dynamic_pointer_cast<StateSetMemoryPerformance>(
            memSensorCpu0->stateSets[0]);
    ASSERT_NE(nullptr, memPerformanceStateSet);
    memPerformanceStateSet->updateShmemReading("Value");
}

TEST_F(TerminusTest, nvidiaEnergyCountPdrParserCoverageMatrix)
{
    auto createVendorData = [](uint8_t sensorDataSize) {
        pldm_oem_energycount_numeric_sensor_value_pdr pdr{};
        pdr.terminus_handle = 1;
        pdr.nvidia_oem_pdr_type = static_cast<uint8_t>(
            nvidia::NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_SENSOR_ENERGYCOUNT);
        pdr.sensor_id = 0x77;
        pdr.entity_type = PLDM_ENTITY_SYS_BOARD;
        pdr.entity_instance_num = 1;
        pdr.container_id = 1;
        pdr.base_unit = PLDM_SENSOR_UNIT_WATTS;
        pdr.sensor_data_size = sensorDataSize;
        pdr.update_interval = 1.0f;
        switch (sensorDataSize)
        {
            case PLDM_SENSOR_DATA_SIZE_UINT8:
            case PLDM_SENSOR_DATA_SIZE_SINT8:
                pdr.max_readable.value_u8 = 0x7F;
                pdr.min_readable.value_u8 = 0x01;
                break;
            case PLDM_SENSOR_DATA_SIZE_UINT16:
            case PLDM_SENSOR_DATA_SIZE_SINT16:
                pdr.max_readable.value_u16 = 0x1234;
                pdr.min_readable.value_u16 = 0x10;
                break;
            case PLDM_SENSOR_DATA_SIZE_UINT32:
            case PLDM_SENSOR_DATA_SIZE_SINT32:
                pdr.max_readable.value_u32 = 0x12345678;
                pdr.min_readable.value_u32 = 0x1000;
                break;
            case PLDM_SENSOR_DATA_SIZE_UINT64:
            case PLDM_SENSOR_DATA_SIZE_SINT64:
                pdr.max_readable.value_u64 = 0x123456789ABCDEF0ull;
                pdr.min_readable.value_u64 = 0x10000ull;
                break;
            default:
                break;
        }
        return structToBytes(pdr);
    };

    const std::array<uint8_t, 9> sensorDataSizes{
        PLDM_SENSOR_DATA_SIZE_UINT8,
        PLDM_SENSOR_DATA_SIZE_SINT8,
        PLDM_SENSOR_DATA_SIZE_UINT16,
        PLDM_SENSOR_DATA_SIZE_SINT16,
        PLDM_SENSOR_DATA_SIZE_UINT32,
        PLDM_SENSOR_DATA_SIZE_SINT32,
        PLDM_SENSOR_DATA_SIZE_UINT64,
        PLDM_SENSOR_DATA_SIZE_SINT64,
        0xFF};

    for (auto sensorDataSize : sensorDataSizes)
    {
        auto vendorData = createVendorData(sensorDataSize);
        auto parsed = nvidia::parseOEMEnergyCountNumericSensorPDR(vendorData);
        ASSERT_NE(nullptr, parsed);
    }

    std::vector<uint8_t> tooSmall(
        PLDM_PDR_OEM_ENERGYCOUNT_NUMERIC_SENSOR_PDR_MIN_LENGTH - 1, 0);
    EXPECT_EQ(nullptr, nvidia::parseOEMEnergyCountNumericSensorPDR(tooSmall));

    auto truncatedUint64VendorData =
        createVendorData(PLDM_SENSOR_DATA_SIZE_UINT64);
    truncatedUint64VendorData.resize(
        PLDM_PDR_OEM_ENERGYCOUNT_NUMERIC_SENSOR_PDR_MIN_LENGTH);
    EXPECT_EQ(nullptr, nvidia::parseOEMEnergyCountNumericSensorPDR(
                           truncatedUint64VendorData));
}

TEST_F(TerminusTest, switchBandwidthSensorCoverage)
{
    std::string switchType{
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet"};
    std::vector<std::string> switchProtocols{
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet",
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.PCIe"};
    std::vector<pldm::dbus::PathAssociation> associations{
        {"chassis", "all_states",
         "/xyz/openbmc_project/inventory/system/chassis/chassis60"}};

    oem_nvidia::SwitchBandwidthSensor sensor(
        0x60, "switch_bandwidth_sensor_cov", switchType, switchProtocols,
        associations);
    EXPECT_EQ("switch_bandwidth_sensor_cov", sensor.getSensorName());

    sensor.updateCurrentBandwidth(std::numeric_limits<double>::quiet_NaN(),
                                  12.5);
    sensor.updateCurrentBandwidth(2.5,
                                  std::numeric_limits<double>::quiet_NaN());
    sensor.updateMaxBandwidth(100.0);
    sensor.addAssociatedSensorID(0x1234);
    sensor.updateOnSharedMemory();

    EXPECT_GT(sensor.switchIntf->maxBandwidth(), 0);
>>>>>>> platform-mc: fix OEM PDR effecter match using oemRecordId
}
