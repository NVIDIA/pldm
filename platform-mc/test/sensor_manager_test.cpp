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
#include <set>
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

    sdbusplus::bus_t& bus;
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

TEST_F(SensorManagerTest, sensorManagerPollingGuardCoverage)
{
    constexpr pldm::tid_t unsupportedTid = 0x62;
    std::string unsupportedUuid("00000000-0000-0000-0000-000000000062");
    termini[unsupportedTid] = std::make_shared<pldm::platform_mc::Terminus>(
        unsupportedTid, 1 << PLDM_BASE, unsupportedUuid, terminusManager);
    EXPECT_CALL(sensorManager, doSensorPolling(::testing::_)).Times(0);
    sensorManager.startPolling(unsupportedTid);
    EXPECT_FALSE(termini[unsupportedTid]->stopPolling);

    constexpr pldm::tid_t busyTid = 0x63;
    std::string busyUuid("00000000-0000-0000-0000-000000000063");
    termini[busyTid] = std::make_shared<pldm::platform_mc::Terminus>(
        busyTid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, busyUuid,
        terminusManager);
    termini[busyTid]->sensorPollingTaskRc.reset();
    sensorManager.SensorManager::doSensorPolling(busyTid);
    EXPECT_FALSE(termini[busyTid]->sensorPollingTaskRc.has_value());

    sensorManager.SensorManager::initSensorList(0x64);
    sensorManager.SensorManager::initSensorList(busyTid);
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

static std::vector<uint8_t> makeGetStateSensorReadingsResp(
    const std::vector<get_sensor_state_field>& stateFields,
    uint8_t completionCode = PLDM_SUCCESS)
{
    auto count = static_cast<uint8_t>(stateFields.size());
    size_t payloadLen = PLDM_GET_STATE_SENSOR_READINGS_MIN_RESP_BYTES +
                        stateFields.size() * sizeof(get_sensor_state_field);
    std::vector<uint8_t> response(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    auto rc = encode_get_state_sensor_readings_resp(
        0, completionCode, count,
        const_cast<get_sensor_state_field*>(stateFields.data()), responseMsg);
    EXPECT_EQ(PLDM_SUCCESS, rc);
    return response;
}

static std::shared_ptr<pldm_numeric_effecter_value_pdr>
    makeNumericEffecterValuePdrForPolling(uint16_t effecterId)
{
    auto pdr = std::make_shared<pldm_numeric_effecter_value_pdr>();
    pdr->effecter_id = effecterId;
    pdr->entity_type = PLDM_ENTITY_SYS_BOARD;
    pdr->entity_instance = 1;
    pdr->container_id = 1;
    pdr->base_unit = PLDM_SENSOR_UNIT_NONE;
    pdr->effecter_data_size = PLDM_EFFECTER_DATA_SIZE_UINT8;
    pdr->max_settable.value_u8 = 100;
    pdr->min_settable.value_u8 = 0;
    pdr->range_field_format = PLDM_RANGE_FIELD_FORMAT_UINT8;
    pdr->range_field_support.byte = 0x1F;
    pdr->nominal_value.value_u8 = 50;
    pdr->normal_max.value_u8 = 60;
    pdr->normal_min.value_u8 = 40;
    pdr->rated_max.value_u8 = 70;
    pdr->rated_min.value_u8 = 30;
    pdr->resolution = 1.0f;
    pdr->offset = 0.0f;
    return pdr;
}

static std::vector<uint8_t> makeGetNumericEffecterValueResp(
    uint8_t effecterDataSize, pldm_effecter_oper_state operState,
    uint8_t completionCode = PLDM_SUCCESS)
{
    union_effecter_data_size pending{};
    union_effecter_data_size present{};
    size_t payloadLen = PLDM_GET_NUMERIC_EFFECTER_VALUE_MIN_RESP_BYTES;

    switch (effecterDataSize)
    {
        case PLDM_EFFECTER_DATA_SIZE_UINT8:
            pending.value_u8 = 10;
            present.value_u8 = 20;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT8:
            pending.value_s8 = -10;
            present.value_s8 = 20;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT16:
            pending.value_u16 = 1000;
            present.value_u16 = 2000;
            payloadLen += 2;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT16:
            pending.value_s16 = -1000;
            present.value_s16 = 2000;
            payloadLen += 2;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT32:
            pending.value_u32 = 100000;
            present.value_u32 = 200000;
            payloadLen += 6;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT32:
            pending.value_s32 = -100000;
            present.value_s32 = 200000;
            payloadLen += 6;
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT64:
            pending.value_u64 = 1000000;
            present.value_u64 = 2000000;
            payloadLen += 14;
            break;
        case PLDM_EFFECTER_DATA_SIZE_SINT64:
        default:
            pending.value_s64 = -1000000;
            present.value_s64 = 2000000;
            payloadLen += 14;
            break;
    }

    std::vector<uint8_t> response(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    auto rc = encode_get_numeric_effecter_value_resp(
        0, completionCode, effecterDataSize, operState,
        reinterpret_cast<uint8_t*>(&pending),
        reinterpret_cast<uint8_t*>(&present), responseMsg, payloadLen);
    EXPECT_EQ(PLDM_SUCCESS, rc);
    return response;
}

static StateSetInfo makeBootRequestStateSetInfo()
{
    return std::make_tuple(
        EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
        std::vector<StateSetData>{
            {PLDM_STATESET_ID_BOOT_REQUEST,
             PossibleStates{PLDM_STATESET_BOOT_REQUEST_NORMAL,
                            PLDM_STATESET_BOOT_REQUEST_REQUESTED}}});
}

static std::vector<uint8_t> makeGetStateEffecterStatesResp(
    uint8_t compEffecterCount, uint8_t completionCode = PLDM_SUCCESS,
    pldm_effecter_oper_state effecterOpState =
        EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING,
    uint8_t pendingState = 0, uint8_t presentState = 1)
{
    pldm_get_state_effecter_states_resp respData{};
    respData.completion_code = completionCode;
    respData.comp_effecter_count = compEffecterCount;
    for (size_t i = 0; i < compEffecterCount; ++i)
    {
        respData.field[i].effecter_op_state = effecterOpState;
        respData.field[i].pending_state = pendingState;
        respData.field[i].present_state = presentState;
    }

    size_t payloadLen =
        1 + 1 + (compEffecterCount * sizeof(get_effecter_state_field));
    std::vector<uint8_t> response(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    auto rc = encode_get_state_effecter_states_resp(0, &respData, responseMsg,
                                                    payloadLen);
    EXPECT_EQ(PLDM_SUCCESS, rc);
    return response;
}

#ifdef OEM_NVIDIA
static std::shared_ptr<pldm_oem_energycount_numeric_sensor_value_pdr>
    makeOemEnergyCountSensorPdr(uint16_t sensorId, uint8_t sensorDataSize,
                                uint8_t baseUnit = PLDM_SENSOR_UNIT_WATTS)
{
    auto pdr =
        std::make_shared<pldm_oem_energycount_numeric_sensor_value_pdr>();
    pdr->terminus_handle = 1;
    pdr->nvidia_oem_pdr_type = 3;
    pdr->sensor_id = sensorId;
    pdr->entity_type = PLDM_ENTITY_SYS_BOARD;
    pdr->entity_instance_num = 1;
    pdr->container_id = 1;
    pdr->sensor_auxiliary_names_pdr = false;
    pdr->base_unit = baseUnit;
    pdr->sensor_data_size = sensorDataSize;
    pdr->update_interval = 1.0f;

    switch (sensorDataSize)
    {
        case PLDM_SENSOR_DATA_SIZE_UINT8:
            pdr->max_readable.value_u8 = 100;
            pdr->min_readable.value_u8 = 1;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT8:
            pdr->max_readable.value_s8 = 100;
            pdr->min_readable.value_s8 = -1;
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT16:
            pdr->max_readable.value_u16 = 1000;
            pdr->min_readable.value_u16 = 10;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT16:
            pdr->max_readable.value_s16 = 1000;
            pdr->min_readable.value_s16 = -10;
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT32:
            pdr->max_readable.value_u32 = 100000;
            pdr->min_readable.value_u32 = 100;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT32:
            pdr->max_readable.value_s32 = 100000;
            pdr->min_readable.value_s32 = -100;
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT64:
            pdr->max_readable.value_u64 = 1000000;
            pdr->min_readable.value_u64 = 1000;
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT64:
        default:
            pdr->max_readable.value_s64 = 1000000;
            pdr->min_readable.value_s64 = -1000;
            break;
    }

    return pdr;
}

static std::vector<uint8_t> makeGetOemEnergyCountSensorReadingResp(
    uint8_t sensorDataSize, int64_t reading, uint8_t sensorOperState,
    uint8_t completionCode = PLDM_SUCCESS)
{
    union_sensor_oem_data_size presentReading{};
    size_t payloadLen = PLDM_GET_OEM_ENERGYCOUNT_SENSOR_READING_MIN_RESP_BYTES;

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
        default:
            presentReading.value_s64 = static_cast<int64_t>(reading);
            payloadLen += 7;
            break;
    }

    std::vector<uint8_t> response(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    responseMsg->hdr.type = PLDM_PLATFORM;
    responseMsg->hdr.command = PLDM_OEM_GET_ENERGYCOUNT_SENSOR_READING;
    responseMsg->payload[0] = completionCode;
    responseMsg->payload[1] = sensorDataSize;
    responseMsg->payload[2] = sensorOperState;
    memcpy(responseMsg->payload + 3, &presentReading, payloadLen - 3);
    return response;
}
#endif

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

enum class InterruptAction
{
    none,
    eraseTerminus,
    stopPolling,
};

class InterruptingTerminusManager :
    public pldm::platform_mc::MockTerminusManager
{
  public:
    InterruptingTerminusManager(
        sdeventplus::Event& event,
        pldm::requester::Handler<pldm::requester::Request>& handler,
        TestInstanceIdDb& instanceIdDb,
        std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>>&
            termini,
        mctp_eid_t localEid) :
        pldm::platform_mc::MockTerminusManager(event, handler, instanceIdDb,
                                               termini, localEid, nullptr),
        trackedTermini(termini)
    {}

    exec::task<int> SendRecvPldmMsgOverMctp(
        mctp_eid_t /*eid*/, pldm::Request& /*request*/,
        const pldm_msg** responseMsg, size_t* responseLen) override
    {
        ++callCount;
        if (responseMsgs.empty() || responseMsg == nullptr ||
            responseLen == nullptr)
        {
            co_return PLDM_ERROR;
        }

        *responseMsg = reinterpret_cast<pldm_msg*>(responseMsgs.front());
        *responseLen = responseLens.front() - sizeof(pldm_msg_hdr);

        responseMsgs.pop();
        responseLens.pop();

        if (interruptCall == callCount)
        {
            if (action == InterruptAction::eraseTerminus)
            {
                trackedTermini.erase(targetTid);
            }
            else if (action == InterruptAction::stopPolling)
            {
                auto it = trackedTermini.find(targetTid);
                if (it != trackedTermini.end() && it->second)
                {
                    it->second->stopPolling = true;
                }
            }
        }

        co_return PLDM_SUCCESS;
    }

    void resetInterruptState()
    {
        action = InterruptAction::none;
        targetTid = 0;
        interruptCall = -1;
        callCount = 0;
    }

    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>>&
        trackedTermini;
    InterruptAction action = InterruptAction::none;
    pldm::tid_t targetTid = 0;
    int interruptCall = -1;
    int callCount = 0;
};

class SensorManagerInterruptTest : public testing::Test
{
  protected:
    SensorManagerInterruptTest() :
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, instanceIdDb, termini, 0x8),
        sensorManager(event, terminusManager, termini, nullptr)
    {}

    void mapTerminus(pldm::tid_t tid)
    {
        pldm::MctpInfo mctpInfo(
            tid, "f72d6f90-5675-11ed-9b6a-0242ac1201ff",
            "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1,
            std::nullopt, "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe",
            std::nullopt);
        ASSERT_TRUE(terminusManager.mapTid(mctpInfo, tid).has_value());
    }

    std::shared_ptr<pldm::platform_mc::Terminus> createPlatformTerminus(
        pldm::tid_t tid)
    {
        std::string uuid("00000000-0000-0000-0000-0000000001ff");
        auto terminus = std::make_shared<pldm::platform_mc::Terminus>(
            tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
        termini[tid] = terminus;
        mapTerminus(tid);
        return terminus;
    }

    void prepareInterrupt(pldm::tid_t tid, InterruptAction nextAction)
    {
        ASSERT_EQ(PLDM_SUCCESS, terminusManager.clearQueuedResponses());
        terminusManager.resetInterruptState();
        terminusManager.targetTid = tid;
        terminusManager.action = nextAction;
        terminusManager.interruptCall = 1;
    }

    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    InterruptingTerminusManager terminusManager;
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
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
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

    const auto invalidConfig =
        std::filesystem::path("pldm_sensor_manager_invalid_config.json");
    const auto validConfig =
        std::filesystem::path("pldm_sensor_manager_valid_config.json");

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

TEST(SensorManagerStandaloneTest, sensorManagerDefaultNamespaceConfigCoverage)
{
    auto event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler(
        nullptr, event, instanceIdDb, false, seconds(1), 2, milliseconds(100));
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
    pldm::platform_mc::MockTerminusManager terminusManager(
        event, reqHandler, instanceIdDb, termini, 0x8, nullptr);

    const auto emptyObjectConfig =
        std::filesystem::path("pldm_sensor_manager_empty_object.json");
    const auto emptyArrayConfig =
        std::filesystem::path("pldm_sensor_manager_empty_array.json");

    {
        std::ofstream out(emptyObjectConfig);
        out << "{}";
    }
    {
        std::ofstream out(emptyArrayConfig);
        out << R"({"PrioritySensorNameSpaces":[]})";
    }

    {
        pldm::platform_mc::SensorManager emptyObjectManager(
            event, terminusManager, termini, nullptr, false, emptyObjectConfig);
        EXPECT_EQ(3u, emptyObjectManager.prioritySensorNameSpaces.size());
    }

    {
        pldm::platform_mc::SensorManager emptyArrayManager(
            event, terminusManager, termini, nullptr, false, emptyArrayConfig);
        EXPECT_EQ(3u, emptyArrayManager.prioritySensorNameSpaces.size());
    }

    std::filesystem::remove(emptyObjectConfig);
    std::filesystem::remove(emptyArrayConfig);
}

TEST(SensorManagerStandaloneTest, sensorManagerCustomPriorityNamespaceCoverage)
{
    auto event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler(
        nullptr, event, instanceIdDb, false, seconds(1), 2, milliseconds(100));
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
    pldm::platform_mc::MockTerminusManager terminusManager(
        event, reqHandler, instanceIdDb, termini, 0x8, nullptr);

    const auto customConfig =
        std::filesystem::path("pldm_sensor_manager_voltage_config.json");
    {
        std::ofstream out(customConfig);
        out << R"({"PrioritySensorNameSpaces":["/xyz/openbmc_project/sensors/voltage/"]})";
    }

    pldm::platform_mc::SensorManager sensorManager(
        event, terminusManager, termini, nullptr, false, customConfig);

    auto voltagePdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    auto voltagePdrData = makeNumericSensorPdr(0x56);
    ASSERT_EQ(PLDM_SUCCESS, decode_numeric_sensor_pdr_data(
                                voltagePdrData.data(), voltagePdrData.size(),
                                voltagePdr.get()));
    voltagePdr->base_unit = PLDM_SENSOR_UNIT_VOLTS;

    std::string voltageName{"priority_voltage_sensor"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis56"};
    auto voltageSensor = std::make_shared<pldm::platform_mc::NumericSensor>(
        0x56, false, voltagePdr, voltageName, inventoryPath, nullptr);
    EXPECT_TRUE(sensorManager.isPriority(voltageSensor));

    auto temperaturePdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    auto temperaturePdrData = makeNumericSensorPdr(0x57);
    ASSERT_EQ(PLDM_SUCCESS,
              decode_numeric_sensor_pdr_data(temperaturePdrData.data(),
                                             temperaturePdrData.size(),
                                             temperaturePdr.get()));

    std::string temperatureName{"non_priority_temperature_sensor"};
    auto temperatureSensor = std::make_shared<pldm::platform_mc::NumericSensor>(
        0x57, false, temperaturePdr, temperatureName, inventoryPath, nullptr);
    EXPECT_FALSE(sensorManager.isPriority(temperatureSensor));

    std::filesystem::remove(customConfig);
}

TEST(SensorManagerStandaloneTest, isPriorityFalseCoverage)
{
    auto event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler(
        nullptr, event, instanceIdDb, false, seconds(1), 2, milliseconds(100));
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
    pldm::platform_mc::MockTerminusManager terminusManager(
        event, reqHandler, instanceIdDb, termini, 0x8, nullptr);
    pldm::platform_mc::SensorManager sensorManager(event, terminusManager,
                                                   termini, nullptr);

    auto sensorPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    auto pdrData = makeNumericSensorPdr(0x52);
    ASSERT_EQ(PLDM_SUCCESS,
              decode_numeric_sensor_pdr_data(pdrData.data(), pdrData.size(),
                                             sensorPdr.get()));
    sensorPdr->base_unit = PLDM_SENSOR_UNIT_HERTZ;

    std::string sensorName{"non_priority_frequency_sensor"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis52"};
    auto sensor = std::make_shared<pldm::platform_mc::NumericSensor>(
        0x52, false, sensorPdr, sensorName, inventoryPath, nullptr);

    EXPECT_FALSE(sensorManager.isPriority(sensor));
}

TEST(SensorManagerStandaloneTest, isPriorityTrueCoverage)
{
    auto event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler(
        nullptr, event, instanceIdDb, false, seconds(1), 2, milliseconds(100));
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
    pldm::platform_mc::MockTerminusManager terminusManager(
        event, reqHandler, instanceIdDb, termini, 0x8, nullptr);
    pldm::platform_mc::SensorManager sensorManager(event, terminusManager,
                                                   termini, nullptr);

    auto sensorPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    auto pdrData = makeNumericSensorPdr(0x53);
    ASSERT_EQ(PLDM_SUCCESS,
              decode_numeric_sensor_pdr_data(pdrData.data(), pdrData.size(),
                                             sensorPdr.get()));

    std::string sensorName{"priority_temperature_sensor"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis53"};
    auto sensor = std::make_shared<pldm::platform_mc::NumericSensor>(
        0x53, false, sensorPdr, sensorName, inventoryPath, nullptr);

    EXPECT_TRUE(sensorManager.isPriority(sensor));
}

TEST(SensorManagerStandaloneTest, isPriorityPowerCoverage)
{
    auto event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler(
        nullptr, event, instanceIdDb, false, seconds(1), 2, milliseconds(100));
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
    pldm::platform_mc::MockTerminusManager terminusManager(
        event, reqHandler, instanceIdDb, termini, 0x8, nullptr);
    pldm::platform_mc::SensorManager sensorManager(event, terminusManager,
                                                   termini, nullptr);

    auto sensorPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    auto pdrData = makeNumericSensorPdr(0x54);
    ASSERT_EQ(PLDM_SUCCESS,
              decode_numeric_sensor_pdr_data(pdrData.data(), pdrData.size(),
                                             sensorPdr.get()));
    sensorPdr->base_unit = PLDM_SENSOR_UNIT_WATTS;

    std::string sensorName{"priority_power_sensor"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis54"};
    auto sensor = std::make_shared<pldm::platform_mc::NumericSensor>(
        0x54, false, sensorPdr, sensorName, inventoryPath, nullptr);

    EXPECT_TRUE(sensorManager.isPriority(sensor));
}

TEST(SensorManagerStandaloneTest, isPriorityEnergyCoverage)
{
    auto event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler(
        nullptr, event, instanceIdDb, false, seconds(1), 2, milliseconds(100));
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
    pldm::platform_mc::MockTerminusManager terminusManager(
        event, reqHandler, instanceIdDb, termini, 0x8, nullptr);
    pldm::platform_mc::SensorManager sensorManager(event, terminusManager,
                                                   termini, nullptr);

    auto sensorPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    auto pdrData = makeNumericSensorPdr(0x55);
    ASSERT_EQ(PLDM_SUCCESS,
              decode_numeric_sensor_pdr_data(pdrData.data(), pdrData.size(),
                                             sensorPdr.get()));
    sensorPdr->base_unit = PLDM_SENSOR_UNIT_JOULES;

    std::string sensorName{"priority_energy_sensor"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis55"};
    auto sensor = std::make_shared<pldm::platform_mc::NumericSensor>(
        0x55, false, sensorPdr, sensorName, inventoryPath, nullptr);

    EXPECT_TRUE(sensorManager.isPriority(sensor));
}

TEST(SensorManagerStandaloneTest, isPriorityEmptyNamespacesCoverage)
{
    auto event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler(
        nullptr, event, instanceIdDb, false, seconds(1), 2, milliseconds(100));
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
    pldm::platform_mc::MockTerminusManager terminusManager(
        event, reqHandler, instanceIdDb, termini, 0x8, nullptr);
    pldm::platform_mc::SensorManager sensorManager(event, terminusManager,
                                                   termini, nullptr);

    auto sensorPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    auto pdrData = makeNumericSensorPdr(0x58);
    ASSERT_EQ(PLDM_SUCCESS,
              decode_numeric_sensor_pdr_data(pdrData.data(), pdrData.size(),
                                             sensorPdr.get()));

    std::string sensorName{"priority_temperature_sensor"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis58"};
    auto sensor = std::make_shared<pldm::platform_mc::NumericSensor>(
        0x58, false, sensorPdr, sensorName, inventoryPath, nullptr);

    sensorManager.prioritySensorNameSpaces.clear();

    EXPECT_FALSE(sensorManager.isPriority(sensor));
}

TEST_F(SensorManagerDataPathTest, doSensorPollingMissingTidCoverage)
{
    EXPECT_NO_THROW(sensorManager.doSensorPolling(0x91));
}

TEST_F(SensorManagerDataPathTest, doSensorPollingCompletionCoverage)
{
    constexpr pldm::tid_t tid = 0x92;
    std::string uuid("00000000-0000-0000-0000-000000000092");
    termini[tid] = std::make_shared<pldm::platform_mc::Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid]->stopPolling = true;

    sensorManager.doSensorPolling(tid);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < deadline &&
           !termini[tid]->sensorPollingTaskRc.has_value())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ASSERT_TRUE(termini[tid]->sensorPollingTaskRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, termini[tid]->sensorPollingTaskRc.value());
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
    stateSensor->needUpdate = false;

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

TEST_F(SensorManagerDataPathTest, initSensorListPriorityAndAsyncCoverage)
{
    constexpr pldm::tid_t tid = 0x43;
    std::string uuid("00000000-0000-0000-0000-000000000043");
    termini[tid] = std::make_shared<pldm::platform_mc::Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid]->initalized = true;

    auto priorityPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    auto priorityPdrData = makeNumericSensorPdr(0x44);
    ASSERT_EQ(PLDM_SUCCESS, decode_numeric_sensor_pdr_data(
                                priorityPdrData.data(), priorityPdrData.size(),
                                priorityPdr.get()));
    std::string priorityName{"priority_sensor"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis43"};
    auto prioritySensor = std::make_shared<pldm::platform_mc::NumericSensor>(
        tid, false, priorityPdr, priorityName, inventoryPath, nullptr);

    auto nonPriorityPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    auto nonPriorityPdrData = makeNumericSensorPdr(0x45);
    ASSERT_EQ(PLDM_SUCCESS,
              decode_numeric_sensor_pdr_data(nonPriorityPdrData.data(),
                                             nonPriorityPdrData.size(),
                                             nonPriorityPdr.get()));
    nonPriorityPdr->base_unit = PLDM_SENSOR_UNIT_HERTZ;
    std::string nonPriorityName{"round_robin_sensor"};
    auto nonPrioritySensor = std::make_shared<pldm::platform_mc::NumericSensor>(
        tid, false, nonPriorityPdr, nonPriorityName, inventoryPath, nullptr);

    StateSetData healthStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_HEALTHSTATE),
                        PossibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                       PLDM_STATESET_HEALTH_STATE_CRITICAL});
    StateSetInfo stateInfo =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
                        std::vector<StateSetData>{healthStateData});
    auto asyncStateSensor = std::make_shared<pldm::platform_mc::StateSensor>(
        tid, false, 0x46, stateInfo, nullptr, inventoryPath, nullptr);
    asyncStateSensor->async = true;
    auto polledStateSensor = std::make_shared<pldm::platform_mc::StateSensor>(
        tid, false, 0x47, stateInfo, nullptr, inventoryPath, nullptr);

    termini[tid]->numericSensors.emplace_back(prioritySensor);
    termini[tid]->numericSensors.emplace_back(nonPrioritySensor);
    termini[tid]->stateSensors.emplace_back(asyncStateSensor);
    termini[tid]->stateSensors.emplace_back(polledStateSensor);

    sensorManager.initSensorList(tid);
    EXPECT_FALSE(termini[tid]->initSensorList);
    ASSERT_EQ(1u, termini[tid]->prioritySensors.size());
    EXPECT_EQ(prioritySensor.get(),
              termini[tid]->prioritySensors.front().get());
    EXPECT_TRUE(prioritySensor->isPriority);
    EXPECT_FALSE(nonPrioritySensor->isPriority);
    EXPECT_EQ(2u, termini[tid]->roundRobinSensors.size());
}

TEST_F(SensorManagerDataPathTest, initSensorListEmptyCoverage)
{
    constexpr pldm::tid_t tid = 0x7B;
    std::string uuid("00000000-0000-0000-0000-00000000007B");
    termini[tid] = std::make_shared<pldm::platform_mc::Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid]->initalized = true;

    sensorManager.initSensorList(tid);

    EXPECT_FALSE(termini[tid]->initSensorList);
    EXPECT_TRUE(termini[tid]->prioritySensors.empty());
    EXPECT_TRUE(termini[tid]->roundRobinSensors.empty());
}

TEST_F(SensorManagerDataPathTest,
       initSensorListClearsExistingQueuesAndUsesEmptyNamespacesCoverage)
{
    constexpr pldm::tid_t tid = 0x7C;
    std::string uuid("00000000-0000-0000-0000-00000000007C");
    termini[tid] = std::make_shared<pldm::platform_mc::Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid]->initalized = true;
    sensorManager.prioritySensorNameSpaces.clear();

    auto numericSensorPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    auto numericPdrData = makeNumericSensorPdr(0x7C);
    ASSERT_EQ(PLDM_SUCCESS, decode_numeric_sensor_pdr_data(
                                numericPdrData.data(), numericPdrData.size(),
                                numericSensorPdr.get()));
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis7C"};
    std::string numericName{"priority_sensor_without_namespace_match"};
    auto numericSensor = std::make_shared<pldm::platform_mc::NumericSensor>(
        tid, false, numericSensorPdr, numericName, inventoryPath, nullptr);

    StateSetData healthStateData =
        std::make_tuple(static_cast<uint16_t>(PLDM_STATESET_ID_HEALTHSTATE),
                        PossibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                       PLDM_STATESET_HEALTH_STATE_CRITICAL});
    StateSetInfo stateInfo =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
                        std::vector<StateSetData>{healthStateData});
    auto syncStateSensor = std::make_shared<pldm::platform_mc::StateSensor>(
        tid, false, 0x7D, stateInfo, nullptr, inventoryPath, nullptr);
    auto asyncStateSensor = std::make_shared<pldm::platform_mc::StateSensor>(
        tid, false, 0x7E, stateInfo, nullptr, inventoryPath, nullptr);
    asyncStateSensor->async = true;

    termini[tid]->numericSensors.emplace_back(numericSensor);
    termini[tid]->stateSensors.emplace_back(syncStateSensor);
    termini[tid]->stateSensors.emplace_back(asyncStateSensor);
    termini[tid]->prioritySensors.emplace_back(numericSensor);
    termini[tid]->roundRobinSensors.push(asyncStateSensor);

    sensorManager.initSensorList(tid);

    EXPECT_FALSE(termini[tid]->initSensorList);
    EXPECT_TRUE(termini[tid]->prioritySensors.empty());
    EXPECT_FALSE(numericSensor->isPriority);
    ASSERT_EQ(2u, termini[tid]->roundRobinSensors.size());

    auto sensors = termini[tid]->roundRobinSensors;
    ASSERT_TRUE(std::holds_alternative<
                std::shared_ptr<pldm::platform_mc::NumericSensor>>(
        sensors.front()));
    EXPECT_EQ(numericSensor,
              std::get<std::shared_ptr<pldm::platform_mc::NumericSensor>>(
                  sensors.front()));
    sensors.pop();
    ASSERT_TRUE(
        std::holds_alternative<std::shared_ptr<pldm::platform_mc::StateSensor>>(
            sensors.front()));
    EXPECT_EQ(syncStateSensor,
              std::get<std::shared_ptr<pldm::platform_mc::StateSensor>>(
                  sensors.front()));
}

TEST_F(SensorManagerDataPathTest,
       sensorManagerRemovedTerminusAndStateReadingCoverage)
{
    constexpr pldm::tid_t tid = 0x44;
    constexpr uint16_t numericSensorId = 0x48;
    constexpr uint16_t stateSensorId = 0x49;

    std::string uuid("00000000-0000-0000-0000-000000000044");
    termini[tid] = std::make_shared<pldm::platform_mc::Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);

    pldm::MctpInfo mctpInfo(
        68, "f72d6f90-5675-11ed-9b6a-0242ac120144",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    ASSERT_TRUE(terminusManager.mapTid(mctpInfo, tid).has_value());

    auto numericSensorPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    auto numericPdr = makeNumericSensorPdr(numericSensorId);
    ASSERT_EQ(PLDM_SUCCESS, decode_numeric_sensor_pdr_data(
                                numericPdr.data(), numericPdr.size(),
                                numericSensorPdr.get()));
    std::string numericName{"removed_terminus_sensor"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis44"};
    auto numericSensor = std::make_shared<pldm::platform_mc::NumericSensor>(
        tid, false, numericSensorPdr, numericName, inventoryPath, nullptr);
    termini[tid]->numericSensors.emplace_back(numericSensor);

    termini[tid]->pdrs.emplace_back(makeStateSensorPdr(stateSensorId));
    ASSERT_TRUE(termini[tid]->parsePDRs());
    auto stateSensor = termini[tid]->stateSensors.front();

    auto numericReadingResp = makeGetSensorReadingResp(
        PLDM_SENSOR_DATA_SIZE_UINT8, 33, PLDM_SENSOR_ENABLED);
    ASSERT_EQ(PLDM_SUCCESS,
              terminusManager.enqueueResponse(numericReadingResp));
    termini.erase(tid);
    auto numericRc =
        stdexec::sync_wait(sensorManager.getSensorReading(numericSensor));
    ASSERT_TRUE(numericRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*numericRc));

    termini[tid] = std::make_shared<pldm::platform_mc::Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid]->stateSensors.emplace_back(stateSensor);
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
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(stateReadingResp));
    termini.erase(tid);
    auto stateRc =
        stdexec::sync_wait(sensorManager.getStateSensorReadings(stateSensor));
    ASSERT_TRUE(stateRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*stateRc));
}

TEST_F(SensorManagerDataPathTest, sensorManagerAdditionalReadCoverage)
{
    constexpr pldm::tid_t tid = 0x46;
    constexpr uint16_t numericSensorId = 0x4B;
    constexpr uint16_t stateSensorId = 0x4C;

    std::string uuid("00000000-0000-0000-0000-000000000046");
    termini[tid] = std::make_shared<pldm::platform_mc::Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    ASSERT_TRUE(
        terminusManager
            .mapTid(pldm::MctpInfo(
                        70, "f72d6f90-5675-11ed-9b6a-0242ac120146",
                        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1,
                        std::nullopt,
                        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe",
                        std::nullopt),
                    tid)
            .has_value());

    auto numericSensorPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    auto numericPdr = makeNumericSensorPdr(numericSensorId);
    ASSERT_EQ(PLDM_SUCCESS, decode_numeric_sensor_pdr_data(
                                numericPdr.data(), numericPdr.size(),
                                numericSensorPdr.get()));
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis46"};
    std::string numericName{"additional_sensor_manager_numeric"};
    auto numericSensor = std::make_shared<pldm::platform_mc::NumericSensor>(
        tid, false, numericSensorPdr, numericName, inventoryPath, nullptr);
    termini[tid]->numericSensors.emplace_back(numericSensor);

    termini[tid]->pdrs.emplace_back(makeStateSensorPdr(stateSensorId));
    ASSERT_TRUE(termini[tid]->parsePDRs());
    auto stateSensor = termini[tid]->stateSensors.front();

    auto noResponseRc =
        stdexec::sync_wait(sensorManager.getSensorReading(numericSensor));
    ASSERT_TRUE(noResponseRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*noResponseRc));
    auto noResponseRcAgain =
        stdexec::sync_wait(sensorManager.getSensorReading(numericSensor));
    ASSERT_TRUE(noResponseRcAgain.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*noResponseRcAgain));

    std::vector<get_sensor_state_field> multiStateFields{
        {PLDM_SENSOR_ENABLED, PLDM_STATESET_HEALTH_STATE_NORMAL,
         PLDM_STATESET_HEALTH_STATE_NORMAL, PLDM_STATESET_HEALTH_STATE_NORMAL},
        {PLDM_SENSOR_ENABLED, PLDM_STATESET_HEALTH_STATE_CRITICAL,
         PLDM_STATESET_HEALTH_STATE_NORMAL,
         PLDM_STATESET_HEALTH_STATE_CRITICAL}};
    auto multiStateResp = makeGetStateSensorReadingsResp(multiStateFields);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(multiStateResp));
    auto multiStateRc =
        stdexec::sync_wait(sensorManager.getStateSensorReadings(stateSensor));
    ASSERT_TRUE(multiStateRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*multiStateRc));
}

TEST_F(SensorManagerInterruptTest, doSensorPollingTaskRemovalAfterAwaitCoverage)
{
    {
        constexpr pldm::tid_t tid = 0x70;
        auto terminus = createPlatformTerminus(tid);
        std::string inventoryPath{
            "/xyz/openbmc_project/inventory/system/chassis/chassis70"};
        std::string effecterName{"interrupt_numeric_effecter"};
        auto effecter = std::make_shared<pldm::platform_mc::NumericEffecter>(
            tid, false, makeNumericEffecterValuePdrForPolling(0x170),
            effecterName, inventoryPath, terminusManager);
        terminus->numericEffecters.emplace_back(effecter);

        prepareInterrupt(tid, InterruptAction::eraseTerminus);
        auto response = makeGetNumericEffecterValueResp(
            PLDM_EFFECTER_DATA_SIZE_UINT8,
            EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING);
        ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));

        auto rc = stdexec::sync_wait(sensorManager.doSensorPollingTask(tid));
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
        EXPECT_EQ(termini.end(), termini.find(tid));
    }

    termini.clear();

    {
        constexpr pldm::tid_t tid = 0x71;
        auto terminus = createPlatformTerminus(tid);
        std::string inventoryPath{
            "/xyz/openbmc_project/inventory/system/chassis/chassis71"};
        auto stateEffecter = std::make_shared<pldm::platform_mc::StateEffecter>(
            tid, false, 0x171, makeBootRequestStateSetInfo(), nullptr,
            inventoryPath, terminusManager);
        terminus->stateEffecters.emplace_back(stateEffecter);

        prepareInterrupt(tid, InterruptAction::eraseTerminus);
        auto response = makeGetStateEffecterStatesResp(1);
        ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));

        auto rc = stdexec::sync_wait(sensorManager.doSensorPollingTask(tid));
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
        EXPECT_EQ(termini.end(), termini.find(tid));
    }

    termini.clear();

    {
        constexpr pldm::tid_t tid = 0x72;
        auto terminus = createPlatformTerminus(tid);
        terminus->pdrs.emplace_back(makeStateSensorPdr(0x172));
        ASSERT_TRUE(terminus->parsePDRs());
        ASSERT_EQ(1u, terminus->stateSensors.size());

        prepareInterrupt(tid, InterruptAction::eraseTerminus);
        std::vector<get_sensor_state_field> stateFields{
            {PLDM_SENSOR_ENABLED, PLDM_STATESET_HEALTH_STATE_NORMAL,
             PLDM_STATESET_HEALTH_STATE_NORMAL,
             PLDM_STATESET_HEALTH_STATE_NORMAL}};
        auto response = makeGetStateSensorReadingsResp(stateFields);
        ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));

        auto rc = stdexec::sync_wait(sensorManager.doSensorPollingTask(tid));
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
        EXPECT_EQ(termini.end(), termini.find(tid));
    }

    termini.clear();

    {
        constexpr pldm::tid_t tid = 0x73;
        auto terminus = createPlatformTerminus(tid);
        auto numericSensorPdr =
            std::make_shared<pldm_numeric_sensor_value_pdr>();
        auto numericPdr = makeNumericSensorPdr(0x173);
        ASSERT_EQ(PLDM_SUCCESS, decode_numeric_sensor_pdr_data(
                                    numericPdr.data(), numericPdr.size(),
                                    numericSensorPdr.get()));
        std::string sensorName{"interrupt_priority_sensor"};
        std::string inventoryPath{
            "/xyz/openbmc_project/inventory/system/chassis/chassis73"};
        auto numericSensor = std::make_shared<pldm::platform_mc::NumericSensor>(
            tid, false, numericSensorPdr, sensorName, inventoryPath, nullptr);
        numericSensor->isPriority = true;
        numericSensor->updateTime = 0;
        terminus->prioritySensors.emplace_back(numericSensor);
        terminus->initSensorList = false;

        prepareInterrupt(tid, InterruptAction::eraseTerminus);
        auto response = makeGetSensorReadingResp(PLDM_SENSOR_DATA_SIZE_UINT8,
                                                 55, PLDM_SENSOR_ENABLED);
        ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));

        auto rc = stdexec::sync_wait(sensorManager.doSensorPollingTask(tid));
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
        EXPECT_EQ(termini.end(), termini.find(tid));
    }

    termini.clear();

    {
        constexpr pldm::tid_t tid = 0x74;
        auto terminus = createPlatformTerminus(tid);
        terminus->pdrs.emplace_back(makeStateSensorPdr(0x174));
        ASSERT_TRUE(terminus->parsePDRs());
        ASSERT_EQ(1u, terminus->stateSensors.size());
        auto stateSensor = terminus->stateSensors.front();
        stateSensor->needUpdate = false;
        stateSensor->setLastUpdatedTimeStamp(0);
        terminus->roundRobinSensors.push(stateSensor);
        terminus->initSensorList = false;

        prepareInterrupt(tid, InterruptAction::eraseTerminus);
        std::vector<get_sensor_state_field> stateFields{
            {PLDM_SENSOR_ENABLED, PLDM_STATESET_HEALTH_STATE_NORMAL,
             PLDM_STATESET_HEALTH_STATE_NORMAL,
             PLDM_STATESET_HEALTH_STATE_NORMAL}};
        auto response = makeGetStateSensorReadingsResp(stateFields);
        ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));

        auto rc = stdexec::sync_wait(sensorManager.doSensorPollingTask(tid));
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
        EXPECT_EQ(termini.end(), termini.find(tid));
    }
}

TEST_F(SensorManagerInterruptTest, doSensorPollingTaskStopAfterAwaitCoverage)
{
    {
        constexpr pldm::tid_t tid = 0x75;
        auto terminus = createPlatformTerminus(tid);
        std::string inventoryPath{
            "/xyz/openbmc_project/inventory/system/chassis/chassis75"};
        std::string effecterName{"interrupt_numeric_effecter_stop"};
        auto effecter = std::make_shared<pldm::platform_mc::NumericEffecter>(
            tid, false, makeNumericEffecterValuePdrForPolling(0x175),
            effecterName, inventoryPath, terminusManager);
        terminus->numericEffecters.emplace_back(effecter);

        prepareInterrupt(tid, InterruptAction::stopPolling);
        auto response = makeGetNumericEffecterValueResp(
            PLDM_EFFECTER_DATA_SIZE_UINT8,
            EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING);
        ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));

        auto rc = stdexec::sync_wait(sensorManager.doSensorPollingTask(tid));
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(PLDM_ERROR, std::get<0>(*rc));
    }

    termini.clear();

    {
        constexpr pldm::tid_t tid = 0x76;
        auto terminus = createPlatformTerminus(tid);
        std::string inventoryPath{
            "/xyz/openbmc_project/inventory/system/chassis/chassis76"};
        auto stateEffecter = std::make_shared<pldm::platform_mc::StateEffecter>(
            tid, false, 0x176, makeBootRequestStateSetInfo(), nullptr,
            inventoryPath, terminusManager);
        terminus->stateEffecters.emplace_back(stateEffecter);

        prepareInterrupt(tid, InterruptAction::stopPolling);
        auto response = makeGetStateEffecterStatesResp(1);
        ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));

        auto rc = stdexec::sync_wait(sensorManager.doSensorPollingTask(tid));
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(PLDM_ERROR, std::get<0>(*rc));
    }

    termini.clear();

    {
        constexpr pldm::tid_t tid = 0x77;
        auto terminus = createPlatformTerminus(tid);
        terminus->pdrs.emplace_back(makeStateSensorPdr(0x177));
        ASSERT_TRUE(terminus->parsePDRs());
        ASSERT_EQ(1u, terminus->stateSensors.size());

        prepareInterrupt(tid, InterruptAction::stopPolling);
        std::vector<get_sensor_state_field> stateFields{
            {PLDM_SENSOR_ENABLED, PLDM_STATESET_HEALTH_STATE_NORMAL,
             PLDM_STATESET_HEALTH_STATE_NORMAL,
             PLDM_STATESET_HEALTH_STATE_NORMAL}};
        auto response = makeGetStateSensorReadingsResp(stateFields);
        ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));

        auto rc = stdexec::sync_wait(sensorManager.doSensorPollingTask(tid));
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(PLDM_ERROR, std::get<0>(*rc));
    }

    termini.clear();

    {
        constexpr pldm::tid_t tid = 0x78;
        auto terminus = createPlatformTerminus(tid);
        auto numericSensorPdr =
            std::make_shared<pldm_numeric_sensor_value_pdr>();
        auto numericPdr = makeNumericSensorPdr(0x178);
        ASSERT_EQ(PLDM_SUCCESS, decode_numeric_sensor_pdr_data(
                                    numericPdr.data(), numericPdr.size(),
                                    numericSensorPdr.get()));
        std::string sensorName{"interrupt_round_robin_sensor"};
        std::string inventoryPath{
            "/xyz/openbmc_project/inventory/system/chassis/chassis78"};
        auto numericSensor = std::make_shared<pldm::platform_mc::NumericSensor>(
            tid, false, numericSensorPdr, sensorName, inventoryPath, nullptr);
        numericSensor->updateTime = 0;
        numericSensor->setLastUpdatedTimeStamp(0);
        terminus->roundRobinSensors.push(numericSensor);
        terminus->initSensorList = false;

        prepareInterrupt(tid, InterruptAction::stopPolling);
        auto response = makeGetSensorReadingResp(PLDM_SENSOR_DATA_SIZE_UINT8,
                                                 88, PLDM_SENSOR_ENABLED);
        ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));

        auto rc = stdexec::sync_wait(sensorManager.doSensorPollingTask(tid));
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(PLDM_ERROR, std::get<0>(*rc));
    }
}

TEST_F(SensorManagerInterruptTest, numericEffecterStateTrackingPollCoverage)
{
    constexpr pldm::tid_t tid = 0x7B;
    auto terminus = createPlatformTerminus(tid);
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis7b"};

    std::string trackedName{"tracked_numeric_effecter"};
    auto trackedEffecter = std::make_shared<pldm::platform_mc::NumericEffecter>(
        tid, false, makeNumericEffecterValuePdrForPolling(0x17B), trackedName,
        inventoryPath, terminusManager);
    trackedEffecter->trackOperationalState = true;
    terminus->numericEffecters.emplace_back(trackedEffecter);

    std::string untrackedName{"untracked_numeric_effecter"};
    auto untrackedEffecter =
        std::make_shared<pldm::platform_mc::NumericEffecter>(
            tid, false, makeNumericEffecterValuePdrForPolling(0x17C),
            untrackedName, inventoryPath, terminusManager);
    terminus->numericEffecters.emplace_back(untrackedEffecter);

    // Interrupt anchor polled after the numeric effecters each round.
    auto stateEffecter = std::make_shared<pldm::platform_mc::StateEffecter>(
        tid, false, 0x17D, makeBootRequestStateSetInfo(), nullptr,
        inventoryPath, terminusManager);
    terminus->stateEffecters.emplace_back(stateEffecter);

    // Round 1: both numeric effecters are read; the loop clears needUpdate
    // only for the untracked one.
    ASSERT_TRUE(trackedEffecter->needUpdate);
    ASSERT_TRUE(untrackedEffecter->needUpdate);
    prepareInterrupt(tid, InterruptAction::stopPolling);
    terminusManager.interruptCall = 3;
    auto response = makeGetNumericEffecterValueResp(
        PLDM_EFFECTER_DATA_SIZE_UINT8,
        EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    auto stateResponse = makeGetStateEffecterStatesResp(1);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(stateResponse));
    auto rc = stdexec::sync_wait(sensorManager.doSensorPollingTask(tid));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*rc));
    EXPECT_TRUE(trackedEffecter->needUpdate);
    EXPECT_FALSE(untrackedEffecter->needUpdate);

    // Round 2: the tracked effecter reads a terminal state and disarms.
    terminus->stopPolling = false;
    prepareInterrupt(tid, InterruptAction::stopPolling);
    terminusManager.interruptCall = 2;
    response = makeGetNumericEffecterValueResp(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                               EFFECTER_OPER_STATE_UNAVAILABLE);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    stateResponse = makeGetStateEffecterStatesResp(1);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(stateResponse));
    rc = stdexec::sync_wait(sensorManager.doSensorPollingTask(tid));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*rc));
    EXPECT_FALSE(trackedEffecter->needUpdate);

    // Round 3: with both numeric effecters idle, the single queued response
    // is decoded by the state effecter — had a numeric effecter still been
    // polled, it would have consumed the response first and the update-
    // pending flag below would not be set.
    terminus->stopPolling = false;
    prepareInterrupt(tid, InterruptAction::stopPolling);
    stateResponse = makeGetStateEffecterStatesResp(
        1, PLDM_SUCCESS, EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(stateResponse));
    rc = stdexec::sync_wait(sensorManager.doSensorPollingTask(tid));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*rc));
    EXPECT_TRUE(stateEffecter->isUpdatePending());
    EXPECT_FALSE(trackedEffecter->needUpdate);

    // Round 4: a PDR repository refresh re-arms the tracked effecter only, and
    // the next round genuinely reads it again. Queuing a terminal state is the
    // discriminator: needUpdate can only fall back to false if the effecter
    // actually issued a GetNumericEffecterValue this round.
    EXPECT_EQ(1u, terminus->rearmTrackedEffecters());
    EXPECT_TRUE(trackedEffecter->needUpdate);
    EXPECT_FALSE(untrackedEffecter->needUpdate);

    terminus->stopPolling = false;
    prepareInterrupt(tid, InterruptAction::stopPolling);
    terminusManager.interruptCall = 2;
    response = makeGetNumericEffecterValueResp(PLDM_EFFECTER_DATA_SIZE_UINT8,
                                               EFFECTER_OPER_STATE_UNAVAILABLE);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(response));
    stateResponse = makeGetStateEffecterStatesResp(1);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(stateResponse));
    rc = stdexec::sync_wait(sensorManager.doSensorPollingTask(tid));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*rc));
    EXPECT_FALSE(trackedEffecter->needUpdate);
    EXPECT_FALSE(untrackedEffecter->needUpdate);
}

TEST_F(SensorManagerDataPathTest, initSensorListSkipsAsyncStateSensorsCoverage)
{
    constexpr pldm::tid_t tid = 0x79;
    std::string uuid("00000000-0000-0000-0000-000000000079");
    auto terminus = std::make_shared<pldm::platform_mc::Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid] = terminus;
    terminus->initalized = true;
    terminus->pdrs.emplace_back(makeStateSensorPdr(0x179));
    terminus->pdrs.emplace_back(makeStateSensorPdr(0x17A));
    ASSERT_TRUE(terminus->parsePDRs());
    ASSERT_EQ(2u, terminus->stateSensors.size());

    auto syncStateSensor = terminus->stateSensors[0];
    auto asyncStateSensor = terminus->stateSensors[1];
    asyncStateSensor->async = true;

    sensorManager.initSensorList(tid);

    EXPECT_FALSE(terminus->initSensorList);
    ASSERT_EQ(1u, terminus->roundRobinSensors.size());
    EXPECT_TRUE(
        std::holds_alternative<std::shared_ptr<pldm::platform_mc::StateSensor>>(
            terminus->roundRobinSensors.front()));
    EXPECT_EQ(std::get<std::shared_ptr<pldm::platform_mc::StateSensor>>(
                  terminus->roundRobinSensors.front()),
              syncStateSensor);
    EXPECT_NE(std::get<std::shared_ptr<pldm::platform_mc::StateSensor>>(
                  terminus->roundRobinSensors.front()),
              asyncStateSensor);
}

TEST_F(SensorManagerTest, sensorManagerGlobalPollingWithMixedSupportCoverage)
{
    constexpr pldm::tid_t supportedTid = 0x21;
    constexpr pldm::tid_t unsupportedTid = 0x22;
    std::string supportedUuid("00000000-0000-0000-0000-000000000021");
    std::string unsupportedUuid("00000000-0000-0000-0000-000000000022");

    termini[supportedTid] = std::make_shared<pldm::platform_mc::Terminus>(
        supportedTid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, supportedUuid,
        terminusManager);
    termini[unsupportedTid] = std::make_shared<pldm::platform_mc::Terminus>(
        unsupportedTid, 1 << PLDM_BASE, unsupportedUuid, terminusManager);

    EXPECT_CALL(sensorManager, doSensorPolling(supportedTid))
        .Times(1)
        .WillOnce(Return());
    EXPECT_CALL(sensorManager, doSensorPolling(unsupportedTid)).Times(0);

    sensorManager.startPolling();
    EXPECT_FALSE(termini[supportedTid]->stopPolling);
    EXPECT_FALSE(termini[unsupportedTid]->stopPolling);

    sensorManager.stopPolling();
    EXPECT_TRUE(termini[supportedTid]->stopPolling);
    EXPECT_TRUE(termini[unsupportedTid]->stopPolling);
}

#ifdef OEM_NVIDIA
TEST_F(SensorManagerDataPathTest, sensorManagerOemNumericCoverage)
{
    constexpr pldm::tid_t tid = 0x45;
    constexpr uint16_t sensorId = 0x4A;
    std::string uuid("00000000-0000-0000-0000-000000000045");
    termini[tid] = std::make_shared<pldm::platform_mc::Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);

    pldm::MctpInfo mctpInfo(
        69, "f72d6f90-5675-11ed-9b6a-0242ac120145",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    ASSERT_TRUE(terminusManager.mapTid(mctpInfo, tid).has_value());

    auto pdr = makeOemEnergyCountSensorPdr(
        sensorId, PLDM_SENSOR_DATA_SIZE_UINT64, PLDM_SENSOR_UNIT_WATTS);
    std::string sensorName{"oem_energy_sensor"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis45"};
    auto sensor = std::make_shared<pldm::platform_mc::NumericSensor>(
        tid, false, pdr, sensorName, inventoryPath,
        pldm::platform_mc::POLLING_METHOD_INDICATOR_PLDM_TYPE_OEM);
    termini[tid]->numericSensors.emplace_back(sensor);

    auto successResp = makeGetOemEnergyCountSensorReadingResp(
        PLDM_SENSOR_DATA_SIZE_UINT64, 123456, PLDM_SENSOR_ENABLED);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(successResp));
    auto successRc = stdexec::sync_wait(sensorManager.getSensorReading(sensor));
    ASSERT_TRUE(successRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*successRc));
    EXPECT_TRUE(std::isfinite(sensor->getReading()));

    auto ccResp = makeGetOemEnergyCountSensorReadingResp(
        PLDM_SENSOR_DATA_SIZE_UINT64, 0, PLDM_SENSOR_ENABLED, PLDM_ERROR);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(ccResp));
    auto ccRc = stdexec::sync_wait(sensorManager.getSensorReading(sensor));
    ASSERT_TRUE(ccRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*ccRc));
}

TEST_F(SensorManagerDataPathTest, sensorManagerOemAdditionalCoverage)
{
    constexpr pldm::tid_t tid = 0x47;
    constexpr uint16_t sensorId = 0x4D;
    std::string uuid("00000000-0000-0000-0000-000000000047");
    termini[tid] = std::make_shared<pldm::platform_mc::Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);

    pldm::MctpInfo mctpInfo(
        71, "f72d6f90-5675-11ed-9b6a-0242ac120147",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    ASSERT_TRUE(terminusManager.mapTid(mctpInfo, tid).has_value());

    auto pdr = makeOemEnergyCountSensorPdr(
        sensorId, PLDM_SENSOR_DATA_SIZE_UINT64, PLDM_SENSOR_UNIT_WATTS);
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis47"};

    std::string invalidIndicatorName{"invalid_oem_indicator_sensor"};
    auto invalidIndicatorSensor =
        std::make_shared<pldm::platform_mc::NumericSensor>(
            tid, false, pdr, invalidIndicatorName, inventoryPath, 0x7F);
    auto invalidIndicatorRc = stdexec::sync_wait(
        sensorManager.getSensorReading(invalidIndicatorSensor));
    ASSERT_TRUE(invalidIndicatorRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*invalidIndicatorRc));

    std::string sensorName{"oem_energy_sensor_extra"};
    auto sensor = std::make_shared<pldm::platform_mc::NumericSensor>(
        tid, false, pdr, sensorName, inventoryPath,
        pldm::platform_mc::POLLING_METHOD_INDICATOR_PLDM_TYPE_OEM);

    std::vector<uint8_t> shortResp{0x00, PLDM_PLATFORM,
                                   PLDM_OEM_GET_ENERGYCOUNT_SENSOR_READING,
                                   PLDM_SUCCESS};
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(shortResp));
    auto decodeRc = stdexec::sync_wait(sensorManager.getSensorReading(sensor));
    ASSERT_TRUE(decodeRc.has_value());
    EXPECT_NE(PLDM_SUCCESS, std::get<0>(*decodeRc));

    auto disabledResp = makeGetOemEnergyCountSensorReadingResp(
        PLDM_SENSOR_DATA_SIZE_UINT64, 0, PLDM_SENSOR_DISABLED);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(disabledResp));
    auto disabledRc =
        stdexec::sync_wait(sensorManager.getSensorReading(sensor));
    ASSERT_TRUE(disabledRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*disabledRc));
    EXPECT_FALSE(sensor->operationalStatusIntf->functional());

    auto unavailableResp = makeGetOemEnergyCountSensorReadingResp(
        PLDM_SENSOR_DATA_SIZE_UINT64, 0, PLDM_SENSOR_UNAVAILABLE);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(unavailableResp));
    auto unavailableRc =
        stdexec::sync_wait(sensorManager.getSensorReading(sensor));
    ASSERT_TRUE(unavailableRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*unavailableRc));
    EXPECT_FALSE(sensor->availabilityIntf->available());
}

TEST_F(SensorManagerTest, sensorHeaderInlineCoverageFromSensorManagerTu)
{
    constexpr pldm::tid_t tid = 0x5A;
    std::string uuid("00000000-0000-0000-0000-00000000005A");
    termini[tid] = std::make_shared<pldm::platform_mc::Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);

    auto numericSensorPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    auto numericPdr = makeNumericSensorPdr(0x5A01);
    ASSERT_EQ(PLDM_SUCCESS, decode_numeric_sensor_pdr_data(
                                numericPdr.data(), numericPdr.size(),
                                numericSensorPdr.get()));

    std::string numericName{"sensor_manager_inline_numeric"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/sensor_manager_inline"};
    auto numericEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    numericEventInfo->impactedComponent = "GPU0";
    auto numericSensor = std::make_shared<pldm::platform_mc::NumericSensor>(
        tid, false, numericSensorPdr, numericName, inventoryPath,
        numericEventInfo);

    EXPECT_FALSE(numericSensor->getSensorName().empty());
    EXPECT_FALSE(numericSensor->getSensorNameSpace().empty());
    EXPECT_EQ(numericSensor->getSensorEventInfo(), numericEventInfo);
    auto replacementNumericEventInfo =
        std::make_shared<pldm::utils::SensorEventInfo>();
    replacementNumericEventInfo->impactedComponent = "GPU1";
    numericSensor->updateSensorEventInfo(replacementNumericEventInfo);
    EXPECT_EQ(numericSensor->getSensorEventInfo(), replacementNumericEventInfo);
    numericSensor->setRefreshed(false);
    EXPECT_FALSE(numericSensor->isRefreshed());
    numericSensor->setRefreshed(true);
    EXPECT_TRUE(numericSensor->isRefreshed());
    numericSensor->setLastUpdatedTimeStamp(100);
    EXPECT_FALSE(numericSensor->needsUpdate(100));
    EXPECT_TRUE(numericSensor->needsUpdate(
        100 + numericSensor->refreshLimitInUsec + 1));

    auto stateEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    stateEventInfo->impactedComponent = "CPU0";
    std::string stateInventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/sensor_manager_state"};
    auto stateSensor = std::make_shared<pldm::platform_mc::StateSensor>(
        tid, false, 0x5A02, makeBootRequestStateSetInfo(), nullptr,
        stateInventoryPath, stateEventInfo);

    EXPECT_TRUE(stateSensor->isDefaultInventoryAssociated());
    stateSensor->setInventoryPaths(
        {stateInventoryPath + "/board0", stateInventoryPath + "/cpu0"}, false);
    EXPECT_FALSE(stateSensor->isDefaultInventoryAssociated());
    EXPECT_EQ(stateSensor->getAssociationEntityId(), "cpu0");
    EXPECT_EQ(stateSensor->getSensorEventInfo(), stateEventInfo);
    auto replacementStateEventInfo =
        std::make_shared<pldm::utils::SensorEventInfo>();
    replacementStateEventInfo->impactedComponent = "CPU1";
    stateSensor->updateSensorEventInfo(replacementStateEventInfo);
    EXPECT_EQ(stateSensor->getSensorEventInfo(), replacementStateEventInfo);
    stateSensor->setRefreshed(false);
    EXPECT_FALSE(stateSensor->isRefreshed());
    stateSensor->setRefreshed(true);
    EXPECT_TRUE(stateSensor->isRefreshed());
    stateSensor->setLastUpdatedTimeStamp(50);
    EXPECT_FALSE(stateSensor->needsUpdate(50));
    EXPECT_TRUE(
        stateSensor->needsUpdate(50 + stateSensor->refreshLimitInUsec + 1));
}
#endif
