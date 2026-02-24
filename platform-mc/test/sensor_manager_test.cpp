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

#include "common/instance_id.hpp"
#include "mock_sensor_manager.hpp"
#include "mock_terminus_manager.hpp"
#include "platform-mc/pldmServiceReadyInterface.hpp"
#include "test/test_instance_id.hpp"

#include <sdeventplus/event.hpp>

#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

#include <gtest/gtest.h>

using namespace std::chrono;

using ::testing::_;
using ::testing::Between;
using ::testing::Return;

class SensorManagerTest : public testing::Test
{
  protected:
    SensorManagerTest() :
        bus(pldm::utils::DBusHandler::getBus()),
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, instanceIdDb, termini, 0x8, nullptr),
        sensorManager(event, terminusManager, termini, nullptr)
    {}

    void runEventLoopForSeconds(uint64_t sec)
    {
        uint64_t t0 = 0;
        uint64_t t1 = 0;
        uint64_t usec = sec * 1000000;
        uint64_t elapsed = 0;
        sd_event_now(event.get(), CLOCK_MONOTONIC, &t0);
        do
        {
            sd_event_run(event.get(), usec - elapsed);
            sd_event_now(event.get(), CLOCK_MONOTONIC, &t1);
            elapsed = t1 - t0;
        } while (elapsed < usec);
    }

    sdbusplus::bus::bus& bus;
    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    pldm::platform_mc::TerminusManager terminusManager;
    pldm::platform_mc::MockSensorManager sensorManager;
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
};

TEST_F(SensorManagerTest, sensorPollingTest)
{
    uint64_t seconds = 10;

    pldm::tid_t tid = 1;
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    termini[tid] = std::make_shared<pldm::platform_mc::Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1, terminusManager);

    EXPECT_CALL(sensorManager, doSensorPolling(tid))
        .Times(1)
        .WillRepeatedly(Return());
    sensorManager.startPolling();
    runEventLoopForSeconds(seconds);
}

TEST_F(SensorManagerTest, sensorManagerControlPathCoverage)
{
    constexpr auto serviceReadyPath =
        "/xyz/openbmc_project/state/service_ready/pldm_sensor_manager_coverage";
    EXPECT_NO_THROW(PldmServiceReadyIntf::initialize(bus, serviceReadyPath));

    pldm::tid_t tid = 2;
    std::string uuid("00000000-0000-0000-0000-000000000002");
    termini[tid] = std::make_shared<pldm::platform_mc::Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);

    EXPECT_CALL(sensorManager, doSensorPolling(tid))
        .Times(2)
        .WillRepeatedly(Return());

    sensorManager.startPolling(0x99);
    sensorManager.stopPolling(0x99);
    sensorManager.setOnline(0x99);
    sensorManager.setOffline(0x99);

    sensorManager.setOnline(tid);
    EXPECT_FALSE(termini[tid]->stopPolling);

    sensorManager.setOffline(tid);
    EXPECT_TRUE(termini[tid]->stopPolling);

    termini[tid]->initalized = true;
    termini[tid]->ready = false;
    sensorManager.checkAllTerminiReady();
    EXPECT_FALSE(termini[tid]->ready);

    termini[tid]->ready = true;
    sensorManager.checkAllTerminiReady();

    sensorManager.enableIntf->enabled(true);
    sensorManager.enableIntf->enabled(false);
}

static std::vector<uint8_t> makeNumericSensorPdr(uint16_t sensorId)
{
    return {
        0x1,
        0x0,
        0x0,
        0x0, // record handle
        0x1, // PDRHeaderVersion
        PLDM_NUMERIC_SENSOR_PDR,
        0x0,
        0x0, // recordChangeNumber
        PLDM_PDR_NUMERIC_SENSOR_PDR_FIXED_LENGTH +
            PLDM_PDR_NUMERIC_SENSOR_PDR_VARIED_SENSOR_DATA_SIZE_MIN_LENGTH +
            PLDM_PDR_NUMERIC_SENSOR_PDR_VARIED_RANGE_FIELD_MIN_LENGTH,
        0, // dataLength
        0,
        0, // PLDMTerminusHandle
        static_cast<uint8_t>(sensorId & 0xFF),
        static_cast<uint8_t>((sensorId >> 8) & 0xFF),
        PLDM_ENTITY_POWER_SUPPLY,
        0,                          // entityType=Power Supply(120)
        1,
        0,                          // entityInstanceNumber
        0x1,
        0x0,                        // containerID=1
        PLDM_NO_INIT,               // sensorInit
        false,                      // sensorAuxiliaryNamesPDR
        PLDM_SENSOR_UNIT_DEGRESS_C, // baseUint(2)=degrees C
        1,                          // unitModifier = 1
        0,                          // rateUnit
        0,                          // baseOEMUnitHandle
        0,                          // auxUnit
        0,                          // auxUnitModifier
        0,                          // auxRateUnit
        0,                          // rel
        0,                          // auxOEMUnitHandle
        true,                       // isLinear
        PLDM_RANGE_FIELD_FORMAT_SINT8,
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
}

static std::vector<uint8_t> makeStateSensorPdr(uint16_t sensorId)
{
    return {0x0,
            0x0,
            0x0,
            0x1,
            0x1,
            PLDM_STATE_SENSOR_PDR,
            0x0,
            0x0,
            0x0,
            0x11,
            0,
            0,
            static_cast<uint8_t>(sensorId & 0xFF),
            static_cast<uint8_t>((sensorId >> 8) & 0xFF),
            PLDM_ENTITY_SYS_BOARD,
            0,
            1,
            0,
            0x1,
            0x0,
            PLDM_NO_INIT,
            false,
            1,
            static_cast<uint8_t>(PLDM_STATESET_ID_HEALTHSTATE & 0xFF),
            static_cast<uint8_t>((PLDM_STATESET_ID_HEALTHSTATE >> 8) & 0xFF),
            0x1,
            0x3};
}

static std::vector<uint8_t> makeGetSensorReadingResp(
    uint8_t sensorDataSize, int64_t reading, uint8_t sensorOperState,
    uint8_t completionCode = PLDM_SUCCESS)
{
    union_sensor_data_size presentReading{};
    size_t payloadLen = PLDM_GET_SENSOR_READING_MIN_RESP_BYTES;

    switch (sensorDataSize)
    {
        case PLDM_SENSOR_DATA_SIZE_UINT8:
            presentReading.value_u8 = static_cast<uint8_t>(reading);
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT8:
            presentReading.value_s8 = static_cast<int8_t>(reading);
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT16:
            presentReading.value_u16 = static_cast<uint16_t>(reading);
            payloadLen += 1;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT16:
            presentReading.value_s16 = static_cast<int16_t>(reading);
            payloadLen += 1;
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT32:
            presentReading.value_u32 = static_cast<uint32_t>(reading);
            payloadLen += 3;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT32:
            presentReading.value_s32 = static_cast<int32_t>(reading);
            payloadLen += 3;
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT64:
            presentReading.value_u64 = static_cast<uint64_t>(reading);
            payloadLen += 7;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT64:
            presentReading.value_s64 = static_cast<int64_t>(reading);
            payloadLen += 7;
            break;
        default:
            presentReading.value_u8 = 0;
            break;
    }

    std::vector<uint8_t> response(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    auto rc = encode_get_sensor_reading_resp(
        0, completionCode, sensorDataSize, sensorOperState,
        PLDM_NO_EVENT_GENERATION, PLDM_SENSOR_NORMAL, PLDM_SENSOR_NORMAL,
        PLDM_SENSOR_NORMAL, reinterpret_cast<uint8_t*>(&presentReading),
        responseMsg, payloadLen);
    EXPECT_EQ(PLDM_SUCCESS, rc);
    return response;
}

class SensorManagerDataPathTest : public testing::Test
{
  protected:
    SensorManagerDataPathTest() :
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, instanceIdDb, termini, 0x8, nullptr),
        sensorManager(event, terminusManager, termini, nullptr)
    {}

    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    pldm::platform_mc::MockTerminusManager terminusManager;
    pldm::platform_mc::SensorManager sensorManager;
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
};

TEST_F(SensorManagerDataPathTest, sensorManagerDataPathCoverage)
{
    constexpr pldm::tid_t tid = 6;
    constexpr uint16_t numericSensorId = 0x01;
    constexpr uint16_t stateSensorId = 0x31;

    std::string uuid("00000000-0000-0000-0000-000000000006");
    termini[tid] = std::make_shared<pldm::platform_mc::Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);

    termini[tid]->pdrs.emplace_back(makeStateSensorPdr(stateSensorId));
    ASSERT_TRUE(termini[tid]->parsePDRs());
    ASSERT_EQ(1u, termini[tid]->stateSensors.size());

    auto numericSensorPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    auto numericPdr = makeNumericSensorPdr(numericSensorId);
    auto decodeRc = decode_numeric_sensor_pdr_data(
        numericPdr.data(), numericPdr.size(), numericSensorPdr.get());
    ASSERT_EQ(PLDM_SUCCESS, decodeRc);
    std::string sensorName{"sensor_data_path_coverage"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis6"};
    auto numericSensor = std::make_shared<pldm::platform_mc::NumericSensor>(
        tid, true, numericSensorPdr, sensorName, inventoryPath, nullptr);
    termini[tid]->numericSensors.emplace_back(numericSensor);

    pldm::MctpInfo mctpInfo(
        12, "f72d6f90-5675-11ed-9b6a-0242ac120112",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe");
    auto mappedTid = terminusManager.mapTid(mctpInfo, tid);
    ASSERT_TRUE(mappedTid.has_value());
    ASSERT_EQ(tid, mappedTid.value());

    std::vector<uint8_t> numericReadingResp{
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
        0x22};
    EXPECT_EQ(PLDM_SUCCESS,
              terminusManager.enqueueResponse(numericReadingResp));
    auto getNumericRc =
        stdexec::sync_wait(sensorManager.getSensorReading(numericSensor));
    ASSERT_TRUE(getNumericRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*getNumericRc));

    numericReadingResp[5] = PLDM_SENSOR_DISABLED;
    EXPECT_EQ(PLDM_SUCCESS,
              terminusManager.enqueueResponse(numericReadingResp));
    auto getDisabledRc =
        stdexec::sync_wait(sensorManager.getSensorReading(numericSensor));
    ASSERT_TRUE(getDisabledRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*getDisabledRc));

    numericReadingResp[5] = PLDM_SENSOR_UNAVAILABLE;
    EXPECT_EQ(PLDM_SUCCESS,
              terminusManager.enqueueResponse(numericReadingResp));
    auto getUnavailableRc =
        stdexec::sync_wait(sensorManager.getSensorReading(numericSensor));
    ASSERT_TRUE(getUnavailableRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*getUnavailableRc));

    std::vector<uint8_t> numericDecodeFailResp{
        0x00, PLDM_PLATFORM, PLDM_GET_SENSOR_READING, PLDM_SUCCESS};
    EXPECT_EQ(PLDM_SUCCESS,
              terminusManager.enqueueResponse(numericDecodeFailResp));
    auto getDecodeFailRc =
        stdexec::sync_wait(sensorManager.getSensorReading(numericSensor));
    ASSERT_TRUE(getDecodeFailRc.has_value());
    EXPECT_NE(PLDM_SUCCESS, std::get<0>(*getDecodeFailRc));

    numericReadingResp[3] = PLDM_ERROR;
    EXPECT_EQ(PLDM_SUCCESS,
              terminusManager.enqueueResponse(numericReadingResp));
    auto getCcErrRc =
        stdexec::sync_wait(sensorManager.getSensorReading(numericSensor));
    ASSERT_TRUE(getCcErrRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*getCcErrRc));
    numericReadingResp[3] = PLDM_SUCCESS;

    termini[tid]->stopPolling = true;
    EXPECT_EQ(PLDM_SUCCESS,
              terminusManager.enqueueResponse(numericReadingResp));
    auto getStopPollingRc =
        stdexec::sync_wait(sensorManager.getSensorReading(numericSensor));
    ASSERT_TRUE(getStopPollingRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*getStopPollingRc));
    termini[tid]->stopPolling = false;

    struct NumericDataCase
    {
        uint8_t sensorDataSize;
        int64_t value;
    };
    const std::array<NumericDataCase, 8> numericDataCases{{
        {PLDM_SENSOR_DATA_SIZE_UINT8, 17},
        {PLDM_SENSOR_DATA_SIZE_SINT8, -17},
        {PLDM_SENSOR_DATA_SIZE_UINT16, 1700},
        {PLDM_SENSOR_DATA_SIZE_SINT16, -1700},
        {PLDM_SENSOR_DATA_SIZE_UINT32, 170000},
        {PLDM_SENSOR_DATA_SIZE_SINT32, -170000},
        {PLDM_SENSOR_DATA_SIZE_UINT64, 1700000},
        {PLDM_SENSOR_DATA_SIZE_SINT64, -1700000},
    }};

    for (const auto& item : numericDataCases)
    {
        auto response = makeGetSensorReadingResp(
            item.sensorDataSize, item.value, PLDM_SENSOR_ENABLED);
        EXPECT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
        auto rc =
            stdexec::sync_wait(sensorManager.getSensorReading(numericSensor));
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
    }

    std::vector<uint8_t> stateReadingResp{
        0x00,
        PLDM_PLATFORM,
        PLDM_GET_STATE_SENSOR_READINGS,
        PLDM_SUCCESS,
        0x01,
        PLDM_SENSOR_ENABLED,
        PLDM_STATESET_HEALTH_STATE_NORMAL,
        PLDM_STATESET_HEALTH_STATE_NORMAL,
        PLDM_STATESET_HEALTH_STATE_NORMAL};
    EXPECT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(stateReadingResp));
    auto getStateRc = stdexec::sync_wait(
        sensorManager.getStateSensorReadings(termini[tid]->stateSensors[0]));
    ASSERT_TRUE(getStateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*getStateRc));

    stateReadingResp[3] = PLDM_ERROR;
    EXPECT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(stateReadingResp));
    auto getStateCcErrRc = stdexec::sync_wait(
        sensorManager.getStateSensorReadings(termini[tid]->stateSensors[0]));
    ASSERT_TRUE(getStateCcErrRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*getStateCcErrRc));
    stateReadingResp[3] = PLDM_SUCCESS;

    std::vector<uint8_t> stateDecodeFailResp{
        0x00, PLDM_PLATFORM, PLDM_GET_STATE_SENSOR_READINGS, PLDM_SUCCESS};
    EXPECT_EQ(PLDM_SUCCESS,
              terminusManager.enqueueResponse(stateDecodeFailResp));
    auto getStateDecodeFailRc = stdexec::sync_wait(
        sensorManager.getStateSensorReadings(termini[tid]->stateSensors[0]));
    ASSERT_TRUE(getStateDecodeFailRc.has_value());
    EXPECT_NE(PLDM_SUCCESS, std::get<0>(*getStateDecodeFailRc));

    termini[tid]->stopPolling = true;
    EXPECT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(stateReadingResp));
    auto getStateStopPollingRc = stdexec::sync_wait(
        sensorManager.getStateSensorReadings(termini[tid]->stateSensors[0]));
    ASSERT_TRUE(getStateStopPollingRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*getStateStopPollingRc));
    termini[tid]->stopPolling = false;

    auto priority = sensorManager.isPriority(numericSensor);
    (void)priority;

    sensorManager.initSensorList(tid);
    termini[tid]->initalized = true;
    termini[tid]->initSensorList = true;
    sensorManager.initSensorList(tid);
    EXPECT_FALSE(termini[tid]->initSensorList);

    termini[tid]->stopPolling = true;
    auto pollingTaskRc =
        stdexec::sync_wait(sensorManager.doSensorPollingTask(tid));
    ASSERT_TRUE(pollingTaskRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*pollingTaskRc));

    termini[tid]->stopPolling = true;
    termini[tid]->sensorPollingTaskRc.emplace(PLDM_SUCCESS);
    sensorManager.doSensorPolling(tid);
    EXPECT_TRUE(termini[tid]->sensorPollingTaskRc.has_value());

    termini[tid]->sensorPollingTaskRc.reset();
    sensorManager.doSensorPolling(tid);
    EXPECT_FALSE(termini[tid]->sensorPollingTaskRc.has_value());
}

TEST(SensorManagerStandaloneTest,
     sensorManagerConfigAndUnsupportedTerminusCoverage)
{
    auto event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler(
        nullptr, event, instanceIdDb, false, seconds(1), 2, milliseconds(100));
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
    pldm::platform_mc::MockTerminusManager terminusManager(
        event, reqHandler, instanceIdDb, termini, 0x8, nullptr);

    const auto invalidConfig = std::filesystem::temp_directory_path() /
                               "pldm_sensor_manager_invalid_config.json";
    const auto validConfig = std::filesystem::temp_directory_path() /
                             "pldm_sensor_manager_valid_config.json";

    {
        std::ofstream invalidFile(invalidConfig);
        invalidFile << "{ invalid json";
    }
    {
        std::ofstream validFile(validConfig);
        validFile
            << R"({"PrioritySensorNameSpaces":["/xyz/openbmc_project/sensors/voltage/"]})";
    }

    {
        pldm::platform_mc::SensorManager invalidConfigManager(
            event, terminusManager, termini, nullptr, false, invalidConfig);
        EXPECT_FALSE(invalidConfigManager.prioritySensorNameSpaces.empty());
    }

    {
        pldm::platform_mc::SensorManager validConfigManager(
            event, terminusManager, termini, nullptr, false, validConfig);
        ASSERT_EQ(1u, validConfigManager.prioritySensorNameSpaces.size());
        EXPECT_EQ("/xyz/openbmc_project/sensors/voltage/",
                  validConfigManager.prioritySensorNameSpaces.front());

        constexpr pldm::tid_t tid = 0x41;
        std::string uuid("00000000-0000-0000-0000-000000000041");
        termini[tid] = std::make_shared<pldm::platform_mc::Terminus>(
            tid, 1 << PLDM_BASE, uuid, terminusManager);
        termini[tid]->stopPolling = true;
        validConfigManager.startPolling(tid);
        EXPECT_TRUE(termini[tid]->stopPolling);
    }

    std::filesystem::remove(invalidConfig);
    std::filesystem::remove(validConfig);
}

TEST_F(SensorManagerDataPathTest, doSensorPollingTaskRoundRobinCoverage)
{
    constexpr pldm::tid_t tid = 0x42;
    constexpr uint16_t numericSensorId = 0x42;
    constexpr uint16_t stateSensorId = 0x43;

    std::string uuid("00000000-0000-0000-0000-000000000042");
    termini[tid] = std::make_shared<pldm::platform_mc::Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);

    auto numericSensorPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    auto numericPdr = makeNumericSensorPdr(numericSensorId);
    auto decodeRc = decode_numeric_sensor_pdr_data(
        numericPdr.data(), numericPdr.size(), numericSensorPdr.get());
    ASSERT_EQ(PLDM_SUCCESS, decodeRc);
    std::string sensorName{"round_robin_numeric"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis42"};
    auto numericSensor = std::make_shared<pldm::platform_mc::NumericSensor>(
        tid, true, numericSensorPdr, sensorName, inventoryPath, nullptr);
    termini[tid]->numericSensors.emplace_back(numericSensor);

    termini[tid]->pdrs.emplace_back(makeStateSensorPdr(stateSensorId));
    ASSERT_TRUE(termini[tid]->parsePDRs());
    ASSERT_EQ(1u, termini[tid]->stateSensors.size());
    auto stateSensor = termini[tid]->stateSensors.front();

    uint64_t now = 0;
    sd_event_now(event.get(), CLOCK_MONOTONIC, &now);
    numericSensor->setLastUpdatedTimeStamp(now);
    stateSensor->setLastUpdatedTimeStamp(now);
    numericSensor->setRefreshed(true);
    stateSensor->setRefreshed(false);

    termini[tid]->roundRobinSensors.push(numericSensor);
    termini[tid]->roundRobinSensors.push(stateSensor);
    termini[tid]->initSensorList = false;
    termini[tid]->ready = false;
    termini[tid]->stopPolling = false;
    sensorManager.pollingTime = 0;

    auto pollingFuture = std::async(std::launch::async, [&]() {
        return stdexec::sync_wait(sensorManager.doSensorPollingTask(tid));
    });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < deadline && !termini[tid]->ready)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    termini[tid]->stopPolling = true;

    auto pollingRc = pollingFuture.get();
    ASSERT_TRUE(pollingRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*pollingRc));
    EXPECT_TRUE(termini[tid]->ready);
}
