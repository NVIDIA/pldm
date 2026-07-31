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
#include "libpldm/base.h"
#include "libpldm/entity.h"
#include "libpldm/platform.h"
#include "libpldm/utils.h"

#include "common/instance_id.hpp"
#include "common/types.hpp"
#include "fw-update/component_updater.hpp"
#include "fw-update/config.hpp"
#include "fw-update/device_inventory.hpp"
#include "fw-update/device_updater.hpp"
#include "fw-update/firmware_inventory.hpp"
#include "fw-update/inventory_manager.hpp"
#include "fw-update/manager.hpp"
#include "fw-update/other_device_update_manager.hpp"
#include "fw-update/package_parser.hpp"
#include "fw-update/package_signature.hpp"
#include "fw-update/update_manager.hpp"
#include "mock_event_manager.hpp"
#include "mock_sensor_manager.hpp"
#include "mock_terminus_manager.hpp"
#include "platform-mc/manager.hpp"
#include "platform-mc/oem_events.hpp"
#include "platform-mc/platform_manager.hpp"
#include "platform-mc/pldmServiceReadyInterface.hpp"
#include "platform-mc/sensor_manager.hpp"
#include "platform-mc/terminus_manager.hpp"
#include "test/test_instance_id.hpp"

#include <array>
#include <filesystem>

#include <gtest/gtest.h>

using ::testing::_;
using ::testing::AtLeast;
using ::testing::Return;

using namespace std::chrono;
using namespace pldm::platform_mc;

constexpr uint8_t mockTerminusManagerLocalEid = 0x08;

static std::vector<uint8_t> makeStateSensorPdr(
    uint16_t sensorId, uint16_t entityType, uint16_t entityInstance,
    uint16_t stateSetId, uint8_t possibleStates = 0x3)
{
    return {0x0,
            0x0,
            0x0,
            0x1, // record handle
            0x1, // PDRHeaderVersion
            PLDM_STATE_SENSOR_PDR,
            0x0,
            0x0,  // recordChangeNumber
            0x0,
            0x11, // dataLength
            0,
            0,    // PLDMTerminusHandle
            static_cast<uint8_t>(sensorId & 0xFF),
            static_cast<uint8_t>((sensorId >> 8) & 0xFF),
            static_cast<uint8_t>(entityType & 0xFF),
            static_cast<uint8_t>((entityType >> 8) & 0xFF),
            static_cast<uint8_t>(entityInstance & 0xFF),
            static_cast<uint8_t>((entityInstance >> 8) & 0xFF),
            0x1,
            0x0,   // containerID=1
            PLDM_NO_INIT,
            false, // sensorAuxiliaryNamesPDR
            1,     // compositeSensorCount
            static_cast<uint8_t>(stateSetId & 0xFF),
            static_cast<uint8_t>((stateSetId >> 8) & 0xFF),
            0x1, // possibleStatesSize
            possibleStates};
}

static std::vector<uint8_t> makeNumericSensorEventStateData(
    uint8_t eventState, uint8_t previousEventState, uint8_t sensorDataSize,
    int64_t reading)
{
    std::vector<uint8_t> data{eventState, previousEventState, sensorDataSize};
    auto append16 = [&data](uint16_t value) {
        data.push_back(static_cast<uint8_t>(value & 0xFF));
        data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    };
    auto append32 = [&data](uint32_t value) {
        data.push_back(static_cast<uint8_t>(value & 0xFF));
        data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        data.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        data.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    };
    auto append64 = [&data](uint64_t value) {
        for (int i = 0; i < 8; ++i)
        {
            data.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
        }
    };

    switch (sensorDataSize)
    {
        case PLDM_SENSOR_DATA_SIZE_UINT8:
            data.push_back(static_cast<uint8_t>(reading));
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT8:
            data.push_back(static_cast<uint8_t>(static_cast<int8_t>(reading)));
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT16:
            append16(static_cast<uint16_t>(reading));
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT16:
            append16(static_cast<uint16_t>(static_cast<int16_t>(reading)));
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT32:
            append32(static_cast<uint32_t>(reading));
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT32:
            append32(static_cast<uint32_t>(static_cast<int32_t>(reading)));
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT64:
            append64(static_cast<uint64_t>(reading));
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT64:
            append64(static_cast<uint64_t>(static_cast<int64_t>(reading)));
            break;
        default:
            data.push_back(0);
            break;
    }
    return data;
}

static std::vector<uint8_t> makeStateSensorEventData(
    uint16_t sensorId, uint8_t sensorEventClass, uint8_t sensorOffset,
    uint8_t eventState, uint8_t previousEventState)
{
    return {static_cast<uint8_t>(sensorId & 0xFF),
            static_cast<uint8_t>((sensorId >> 8) & 0xFF),
            sensorEventClass,
            sensorOffset,
            eventState,
            previousEventState};
}

static std::vector<uint8_t> makeSensorOpEventData(
    uint16_t sensorId, uint8_t presentOpState, uint8_t previousOpState)
{
    return {static_cast<uint8_t>(sensorId & 0xFF),
            static_cast<uint8_t>((sensorId >> 8) & 0xFF), PLDM_SENSOR_OP_STATE,
            presentOpState, previousOpState};
}

static std::vector<uint8_t> makePollForPlatformEventResponse(
    uint8_t tid, uint16_t eventId, uint8_t transferFlag, uint8_t eventClass,
    const std::vector<uint8_t>& eventData = {}, uint32_t checksum = 0,
    uint8_t completionCode = PLDM_SUCCESS)
{
    size_t payloadLength = PLDM_POLL_FOR_PLATFORM_EVENT_MESSAGE_MIN_RESP_BYTES;
    uint32_t eventDataSize = 0;
    uint8_t* eventDataPtr = nullptr;

    if (eventId != 0x0000 && eventId != 0xFFFF)
    {
        eventDataSize = eventData.size();
        eventDataPtr = eventData.empty()
                           ? nullptr
                           : const_cast<uint8_t*>(eventData.data());
        payloadLength = PLDM_POLL_FOR_PLATFORM_EVENT_MESSAGE_RESP_BYTES +
                        eventDataSize;
        if (transferFlag == PLDM_PLATFORM_TRANSFER_END ||
            transferFlag == PLDM_PLATFORM_TRANSFER_START_AND_END)
        {
            payloadLength +=
                PLDM_POLL_FOR_PLATFORM_EVENT_MESSAGE_CHECKSUM_BYTES;
        }
    }

    std::vector<uint8_t> response(sizeof(pldm_msg_hdr) + payloadLength, 0);
    auto* responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    auto rc = encode_poll_for_platform_event_message_resp(
        0, completionCode, tid, eventId, 0x1234, transferFlag, eventClass,
        eventDataSize, eventDataPtr, checksum, responseMsg, payloadLength);
    EXPECT_EQ(PLDM_SUCCESS, rc);
    return response;
}

class EventManagerCoverage : public EventManager
{
  public:
    using EventManager::createSensorThresholdLogEntry;
    using EventManager::EventManager;
    using EventManager::notifyCPERLogger;
    using EventManager::pollForPlatformEventTask;
    using EventManager::processNumericSensorEvent;
    using EventManager::processStateSensorEvent;
    using EventManager::processTelemetryPauseEvent;
    using EventManager::processTelemetryRediscoveryEvent;
    using EventManager::processTelemetryResumeEvent;
};

class EventManagerTest : public testing::Test
{
  protected:
    EventManagerTest() :
        bus(pldm::utils::DBusHandler::getBus()),
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, instanceIdDb, termini,
                        mockTerminusManagerLocalEid, nullptr),
        fwUpdateManager(event, reqHandler, instanceIdDb, "", false),
        platformManager(terminusManager, termini),
        sensorManager(event, terminusManager, termini, nullptr),
        eventManager(terminusManager, termini, fwUpdateManager, platformManager,
                     sensorManager, false)
    {}

    sdbusplus::bus::bus& bus;
    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    pldm::platform_mc::TerminusManager terminusManager;
    pldm::fw_update::Manager fwUpdateManager;
    pldm::platform_mc::PlatformManager platformManager;
    pldm::platform_mc::SensorManager sensorManager;
    MockEventManager eventManager;
    std::map<pldm::tid_t, std::shared_ptr<Terminus>> termini{};
};

class PlatformMcManagerTest : public testing::Test
{
  protected:
    PlatformMcManagerTest() :
        bus(pldm::utils::DBusHandler::getBus()),
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        fwUpdateManager(event, reqHandler, instanceIdDb, "", false)
    {}

    sdbusplus::bus::bus& bus;
    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    pldm::fw_update::Manager fwUpdateManager;
};

class EventManagerProtectedTest : public testing::Test
{
  protected:
    EventManagerProtectedTest() :
        bus(pldm::utils::DBusHandler::getBus()),
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, instanceIdDb, termini,
                        mockTerminusManagerLocalEid, nullptr),
        fwUpdateManager(event, reqHandler, instanceIdDb, "", false),
        platformManager(terminusManager, termini),
        sensorManager(event, terminusManager, termini, nullptr),
        eventManager(terminusManager, termini, fwUpdateManager, platformManager,
                     sensorManager, false)
    {}

    sdbusplus::bus::bus& bus;
    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    pldm::platform_mc::TerminusManager terminusManager;
    pldm::fw_update::Manager fwUpdateManager;
    pldm::platform_mc::PlatformManager platformManager;
    pldm::platform_mc::MockSensorManager sensorManager;
    EventManagerCoverage eventManager;
    std::map<pldm::tid_t, std::shared_ptr<Terminus>> termini{};
};

class EventManagerPollingTest : public testing::Test
{
  protected:
    EventManagerPollingTest() :
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, instanceIdDb, termini,
                        mockTerminusManagerLocalEid, nullptr),
        fwUpdateManager(event, reqHandler, instanceIdDb, "", false),
        platformManager(terminusManager, termini),
        sensorManager(event, terminusManager, termini, nullptr),
        eventManager(terminusManager, termini, fwUpdateManager, platformManager,
                     sensorManager, false)
    {}

    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    pldm::platform_mc::MockTerminusManager terminusManager;
    pldm::fw_update::Manager fwUpdateManager;
    pldm::platform_mc::PlatformManager platformManager;
    pldm::platform_mc::SensorManager sensorManager;
    EventManagerCoverage eventManager;
    std::map<pldm::tid_t, std::shared_ptr<Terminus>> termini{};
};

TEST_F(PlatformMcManagerTest, pldmServiceReadyInterfaceCoverage)
{
    constexpr auto objectPath =
        "/xyz/openbmc_project/state/service_ready/pldm_coverage";

    EXPECT_THROW((void)PldmServiceReadyIntf::getInstance(), std::runtime_error);
    EXPECT_NO_THROW(PldmServiceReadyIntf::initialize(bus, objectPath));

    auto& intf = PldmServiceReadyIntf::getInstance();
    EXPECT_NO_THROW(intf.setStateStarting());
    EXPECT_NO_THROW(intf.setStateEnabled());
    EXPECT_THROW(PldmServiceReadyIntf::initialize(bus, objectPath),
                 std::logic_error);
}

TEST_F(PlatformMcManagerTest, managerInterfaceCoverage)
{
    Manager manager(event, reqHandler, instanceIdDb, fwUpdateManager, false,
                    true);

    auto beforeRc = stdexec::sync_wait(manager.beforeDiscoverTerminus());
    ASSERT_TRUE(beforeRc.has_value());
    EXPECT_EQ(std::get<0>(*beforeRc), PLDM_SUCCESS);

    auto afterRc = stdexec::sync_wait(manager.afterDiscoverTerminus());
    ASSERT_TRUE(afterRc.has_value());
    EXPECT_EQ(std::get<0>(*afterRc), PLDM_SUCCESS);

    pldm::MctpInfos mctpInfos{};
    manager.handleMctpEndpoints(mctpInfos, {});
    manager.handleRemovedMctpEndpoints(mctpInfos);

    pldm::MctpInfo mctpInfo(
        9, "00000000-0000-0000-0000-00000000abcd",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    manager.updateMctpEndpointAvailability(mctpInfo, true);
    manager.onlineMctpEndpoint(std::get<1>(mctpInfo), std::get<0>(mctpInfo));
    manager.offlineMctpEndpoint(std::get<1>(mctpInfo), std::get<0>(mctpInfo));

    EXPECT_FALSE(manager.getActiveEidByName("non-existent").has_value());
    manager.startSensorPolling();
    manager.stopSensorPolling();
    auto getTerminusManager = &Manager::getTerminusManager;
    auto& terminusManagerRef = (manager.*getTerminusManager)();
    EXPECT_NE(terminusManagerRef.getLocalEid(), 0);

    auto pollRc = stdexec::sync_wait(manager.pollForPlatformEvent(7));
    ASSERT_TRUE(pollRc.has_value());
    EXPECT_EQ(std::get<0>(*pollRc), PLDM_SUCCESS);

    auto resumeRc = stdexec::sync_wait(manager.resumeTerminus(7));
    ASSERT_TRUE(resumeRc.has_value());
    EXPECT_EQ(std::get<0>(*resumeRc), PLDM_SUCCESS);
}

TEST_F(PlatformMcManagerTest, managerEventHandlersCoverage)
{
    Manager manager(event, reqHandler, instanceIdDb, fwUpdateManager, false,
                    true);

    std::vector<uint8_t> request(sizeof(pldm_msg_hdr) + 64, 0);
    auto* msg = reinterpret_cast<pldm_msg*>(request.data());
    uint8_t platformEventStatus = 0;
    constexpr uint8_t tid = 1;

    EXPECT_EQ(manager.handleCperEvent(msg, 64, 1, tid, 0, platformEventStatus,
                                      PLDM_OEM_EVENT_CLASS_ERROR_COUNTER),
              PLDM_SUCCESS);
    EXPECT_EQ(manager.handleActiveFWVersionChangeEvent(msg, 64, 1, tid, 0,
                                                       platformEventStatus),
              PLDM_SUCCESS);
    EXPECT_EQ(manager.handleSmbiosEvent(msg, 1, 1, tid, 0, platformEventStatus),
              PLDM_SUCCESS);
    EXPECT_EQ(manager.handleTelemetryManagementEvent(msg, 64, 1, tid, 0,
                                                     platformEventStatus),
              PLDM_SUCCESS);
    EXPECT_EQ(
        manager.handleSensorEvent(msg, 64, 1, tid, 0, platformEventStatus),
        PLDM_SUCCESS);

    EXPECT_EQ(manager.handleErrorCounterEvent(msg, 64, 1, tid, 65,
                                              platformEventStatus),
              PLDM_ERROR_INVALID_LENGTH);
    EXPECT_EQ(manager.handleErrorCounterEvent(msg, 64, 1, tid, 0,
                                              platformEventStatus),
              PLDM_SUCCESS);
    EXPECT_EQ(manager.handlePcieTelemetryEvent(msg, 64, 1, tid, 65,
                                               platformEventStatus),
              PLDM_ERROR_INVALID_LENGTH);
    EXPECT_EQ(manager.handlePcieTelemetryEvent(msg, 64, 1, tid, 0,
                                               platformEventStatus),
              PLDM_SUCCESS);
    EXPECT_EQ(
        manager.handlePCoreDumpEvent(msg, 64, 1, tid, 65, platformEventStatus),
        PLDM_ERROR_INVALID_LENGTH);
    EXPECT_EQ(
        manager.handlePCoreDumpEvent(msg, 64, 1, tid, 0, platformEventStatus),
        PLDM_SUCCESS);

    EXPECT_EQ(manager.handlePldmMessagePollEvent(msg, 64, 1, tid, 64,
                                                 platformEventStatus),
              PLDM_ERROR);

    std::vector<uint8_t> pollEventData(sizeof(pldm_message_poll_event_data), 0);
    pollEventData[0] = 0x00;
    std::copy(pollEventData.begin(), pollEventData.end(), msg->payload);
    EXPECT_NE(manager.handlePldmMessagePollEvent(msg, pollEventData.size(), 1,
                                                 tid, 0, platformEventStatus),
              PLDM_SUCCESS);

    pollEventData[0] = 0x01; // format_version
    pollEventData[1] = 0x01; // event_id (little-endian)
    pollEventData[2] = 0x00;
    pollEventData[3] = 0x01; // data_transfer_handle (little-endian)
    pollEventData[4] = 0x00;
    pollEventData[5] = 0x00;
    pollEventData[6] = 0x00;
    std::copy(pollEventData.begin(), pollEventData.end(), msg->payload);
    EXPECT_EQ(manager.handlePldmMessagePollEvent(msg, pollEventData.size(), 1,
                                                 tid, 0, platformEventStatus),
              PLDM_SUCCESS);
}

TEST_F(EventManagerTest, processNumericSensorEventTest)
{
#define SENSOR_READING 50
#define WARNING_HIGH 45
    pldm::tid_t tid = 1;
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1, terminusManager);
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
        0x80,
        0x3f, // resolution=1.0
        0,
        0,
        0,
        0,  // offset=1.0
        0,
        0,  // accuracy
        0,  // plusTolerance
        0,  // minusTolerance
        2,  // hysteresis
        63, // supportedThresholds
        0,  // thresholdAndHysteresisVolatility
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
        0x18,                          // rangeFieldsupport
        0,                             // nominalValue
        0,                             // normalMax
        0,                             // normalMin
        WARNING_HIGH,                  // warningHigh
        20,                            // warningLow
        60,                            // criticalHigh
        10,                            // criticalLow
        0,                             // fatalHigh
        0                              // fatalLow
    };

    // add dummy numeric sensor
    termini[tid]->pdrs.emplace_back(pdr1);
    auto rc = termini[1]->parsePDRs();
    uint8_t platformEventStatus = 0;
    EXPECT_EQ(true, rc);

    std::vector<uint8_t> eventData{
        0x1,
        0x0, // sensor id
        PLDM_NUMERIC_SENSOR_STATE,
        PLDM_SENSOR_UPPERWARNING,
        PLDM_SENSOR_NORMAL,
        PLDM_SENSOR_DATA_SIZE_UINT8,
        SENSOR_READING};
    rc = eventManager.handlePlatformEvent(
        tid, PLDM_SENSOR_EVENT, eventData.data(), eventData.size(),
        platformEventStatus);
    EXPECT_EQ(PLDM_SUCCESS, rc);
    EXPECT_EQ(PLDM_EVENT_NO_LOGGING, platformEventStatus);
}

TEST_F(EventManagerProtectedTest, processNumericSensorEventCoverageMatrix)
{
    constexpr pldm::tid_t tid = 2;
    constexpr uint16_t sensorId = 0x22;
    std::string uuid("00000000-0000-0000-0000-000000000022");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    auto sensorPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    sensorPdr->sensor_id = sensorId;
    sensorPdr->entity_type = PLDM_ENTITY_POWER_SUPPLY;
    sensorPdr->entity_instance_num = 1;
    sensorPdr->container_id = 1;
    sensorPdr->base_unit = PLDM_SENSOR_UNIT_DEGRESS_C;
    sensorPdr->sensor_data_size = PLDM_SENSOR_DATA_SIZE_UINT8;
    sensorPdr->max_readable.value_u8 = 100;
    sensorPdr->min_readable.value_u8 = 0;
    sensorPdr->hysteresis.value_u8 = 2;
    sensorPdr->supported_thresholds.byte = 63;
    sensorPdr->range_field_support.byte = 0x18;
    sensorPdr->range_field_format = PLDM_RANGE_FIELD_FORMAT_UINT8;
    sensorPdr->warning_high.value_u8 = 45;
    sensorPdr->warning_low.value_u8 = 20;
    sensorPdr->critical_high.value_u8 = 60;
    sensorPdr->critical_low.value_u8 = 10;
    sensorPdr->resolution = 1.0f;
    sensorPdr->offset = 0.0f;
    sensorPdr->update_interval = 1.0f;

    std::string sensorName{"event_matrix_sensor"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis0"};
    auto sensor = std::make_shared<NumericSensor>(
        tid, false, sensorPdr, sensorName, inventoryPath, nullptr);
    termini[tid]->numericSensors.emplace_back(sensor);
    ASSERT_EQ(1u, termini[tid]->numericSensors.size());

    std::array<uint8_t, 2> shortData{0, 0};
    eventManager.processNumericSensorEvent(tid, sensorId, shortData.data(),
                                           shortData.size());

    struct NumericEventCase
    {
        uint8_t previousState;
        uint8_t currentState;
        uint8_t dataSize;
        int64_t reading;
    };

    const std::array<NumericEventCase, 9> cases{{
        {PLDM_SENSOR_NORMAL, PLDM_SENSOR_UPPERWARNING,
         PLDM_SENSOR_DATA_SIZE_UINT8, 50},
        {PLDM_SENSOR_UPPERWARNING, PLDM_SENSOR_NORMAL,
         PLDM_SENSOR_DATA_SIZE_SINT8, -50},
        {PLDM_SENSOR_LOWERWARNING, PLDM_SENSOR_NORMAL,
         PLDM_SENSOR_DATA_SIZE_UINT16, 500},
        {PLDM_SENSOR_NORMAL, PLDM_SENSOR_LOWERWARNING,
         PLDM_SENSOR_DATA_SIZE_SINT16, -500},
        {PLDM_SENSOR_UPPERWARNING, PLDM_SENSOR_UPPERCRITICAL,
         PLDM_SENSOR_DATA_SIZE_UINT32, 50000},
        {PLDM_SENSOR_UPPERCRITICAL, PLDM_SENSOR_UPPERWARNING,
         PLDM_SENSOR_DATA_SIZE_SINT32, -50000},
        {PLDM_SENSOR_LOWERWARNING, PLDM_SENSOR_LOWERCRITICAL,
         PLDM_SENSOR_DATA_SIZE_UINT64, 500000},
        {PLDM_SENSOR_LOWERCRITICAL, PLDM_SENSOR_LOWERWARNING,
         PLDM_SENSOR_DATA_SIZE_SINT64, -500000},
        {PLDM_SENSOR_NORMAL, PLDM_SENSOR_NORMAL, PLDM_SENSOR_DATA_SIZE_UINT8,
         0},
    }};

    for (const auto& item : cases)
    {
        auto sensorData = makeNumericSensorEventStateData(
            item.currentState, item.previousState, item.dataSize, item.reading);
        eventManager.processNumericSensorEvent(tid, sensorId, sensorData.data(),
                                               sensorData.size());
    }

    auto sensorData = makeNumericSensorEventStateData(
        PLDM_SENSOR_UPPERWARNING, PLDM_SENSOR_NORMAL,
        PLDM_SENSOR_DATA_SIZE_UINT8, 1);
    eventManager.processNumericSensorEvent(tid + 1, sensorId, sensorData.data(),
                                           sensorData.size());
    eventManager.processNumericSensorEvent(tid, sensorId + 1, sensorData.data(),
                                           sensorData.size());
}

TEST_F(EventManagerTest, getSensorThresholdEventDataTest)
{
    std::string messageId;
    std::string eventId;
    std::string impactedComponent;
    std::tie(messageId, eventId, impactedComponent) =
        eventManager.getSensorThresholdEventData(PLDM_SENSOR_UNKNOWN,
                                                 PLDM_SENSOR_NORMAL, nullptr);
    EXPECT_EQ(messageId, std::string{});

    std::tie(messageId, eventId, impactedComponent) =
        eventManager.getSensorThresholdEventData(
            PLDM_SENSOR_UNKNOWN, PLDM_SENSOR_LOWERWARNING, nullptr);
    EXPECT_EQ(messageId, SensorThresholdWarningLowGoingLow);

    std::tie(messageId, eventId, impactedComponent) =
        eventManager.getSensorThresholdEventData(
            PLDM_SENSOR_UNKNOWN, PLDM_SENSOR_LOWERCRITICAL, nullptr);
    EXPECT_EQ(messageId, SensorThresholdCriticalLowGoingLow);

    std::tie(messageId, eventId, impactedComponent) =
        eventManager.getSensorThresholdEventData(
            PLDM_SENSOR_UNKNOWN, PLDM_SENSOR_UPPERWARNING, nullptr);
    EXPECT_EQ(messageId, SensorThresholdWarningHighGoingHigh);

    std::tie(messageId, eventId, impactedComponent) =
        eventManager.getSensorThresholdEventData(
            PLDM_SENSOR_UNKNOWN, PLDM_SENSOR_UPPERCRITICAL, nullptr);
    EXPECT_EQ(messageId, SensorThresholdCriticalHighGoingHigh);

    std::tie(messageId, eventId, impactedComponent) =
        eventManager.getSensorThresholdEventData(
            PLDM_SENSOR_NORMAL, PLDM_SENSOR_LOWERWARNING, nullptr);
    EXPECT_EQ(messageId, SensorThresholdWarningLowGoingLow);

    std::tie(messageId, eventId, impactedComponent) =
        eventManager.getSensorThresholdEventData(
            PLDM_SENSOR_NORMAL, PLDM_SENSOR_LOWERCRITICAL, nullptr);
    EXPECT_EQ(messageId, SensorThresholdCriticalLowGoingLow);

    std::tie(messageId, eventId, impactedComponent) =
        eventManager.getSensorThresholdEventData(
            PLDM_SENSOR_LOWERCRITICAL, PLDM_SENSOR_LOWERWARNING, nullptr);
    EXPECT_EQ(messageId, SensorThresholdCriticalLowGoingHigh);

    std::tie(messageId, eventId, impactedComponent) =
        eventManager.getSensorThresholdEventData(PLDM_SENSOR_LOWERWARNING,
                                                 PLDM_SENSOR_NORMAL, nullptr);
    EXPECT_EQ(messageId, SensorThresholdWarningLowGoingHigh);

    std::tie(messageId, eventId, impactedComponent) =
        eventManager.getSensorThresholdEventData(
            PLDM_SENSOR_NORMAL, PLDM_SENSOR_UPPERWARNING, nullptr);
    EXPECT_EQ(messageId, SensorThresholdWarningHighGoingHigh);

    std::tie(messageId, eventId, impactedComponent) =
        eventManager.getSensorThresholdEventData(
            PLDM_SENSOR_UPPERWARNING, PLDM_SENSOR_UPPERCRITICAL, nullptr);
    EXPECT_EQ(messageId, SensorThresholdCriticalHighGoingHigh);

    std::tie(messageId, eventId, impactedComponent) =
        eventManager.getSensorThresholdEventData(
            PLDM_SENSOR_UPPERCRITICAL, PLDM_SENSOR_UPPERWARNING, nullptr);
    EXPECT_EQ(messageId, SensorThresholdCriticalHighGoingLow);

    std::tie(messageId, eventId, impactedComponent) =
        eventManager.getSensorThresholdEventData(PLDM_SENSOR_UPPERWARNING,
                                                 PLDM_SENSOR_NORMAL, nullptr);
    EXPECT_EQ(messageId, SensorThresholdWarningHighGoingLow);
}

TEST_F(EventManagerProtectedTest, protectedPathCoverage)
{
    constexpr pldm::tid_t tid = 5;
    constexpr uint16_t sensorId = 0x31;

    std::string uuid("00000000-0000-0000-0000-000000000005");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    auto pdr = makeStateSensorPdr(sensorId, PLDM_ENTITY_SYS_BOARD, 1,
                                  PLDM_STATESET_ID_HEALTHSTATE, 0x3);
    termini[tid]->pdrs.emplace_back(pdr);
    ASSERT_TRUE(termini[tid]->parsePDRs());
    ASSERT_EQ(1u, termini[tid]->stateSensors.size());

    EXPECT_CALL(sensorManager, doSensorPolling(tid))
        .Times(AtLeast(1))
        .WillRepeatedly(Return());

    uint8_t stateData[3]{0, PLDM_STATESET_HEALTH_STATE_CRITICAL,
                         PLDM_STATESET_HEALTH_STATE_NORMAL};
    eventManager.processStateSensorEvent(tid, sensorId, stateData, 2);
    eventManager.processStateSensorEvent(tid + 1, sensorId, stateData,
                                         sizeof(stateData));
    eventManager.processStateSensorEvent(tid, sensorId + 1, stateData,
                                         sizeof(stateData));
    eventManager.processStateSensorEvent(tid, sensorId, stateData,
                                         sizeof(stateData));

    eventManager.createSensorThresholdLogEntry(
        SensorThresholdWarningHighGoingHigh, "Sensor1", 52.0, 45.0,
        "OpenBMC.0.2.Test", "CPU0");
    eventManager.createSensorThresholdLogEntry("OpenBMC.0.2.Invalid", "Sensor1",
                                               1.0, 2.0, "", "");

    std::array<unsigned char, 4> cperData{1, 2, 3, 4};
    eventManager.notifyCPERLogger(cperData);

    eventManager.processTelemetryPauseEvent(tid + 1);
    eventManager.processTelemetryPauseEvent(tid);
    eventManager.processTelemetryResumeEvent(tid + 1);
    eventManager.processTelemetryResumeEvent(tid);

    auto rediscoverRc = stdexec::sync_wait(
        eventManager.processTelemetryRediscoveryEvent(tid + 1));
    ASSERT_TRUE(rediscoverRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*rediscoverRc));

    auto pollRc = stdexec::sync_wait(eventManager.pollForPlatformEventTask(
        static_cast<pldm::tid_t>(0x7F), 32));
    ASSERT_TRUE(pollRc.has_value());
    EXPECT_NE(PLDM_SUCCESS, std::get<0>(*pollRc));

    uint8_t platformEventStatus = 0;
    std::array<uint8_t, 1> shortTelemetryData{0x1};
    EXPECT_EQ(PLDM_ERROR,
              eventManager.handlePlatformEvent(
                  tid, PLDM_OEM_EVENT_CLASS_0xFD, shortTelemetryData.data(),
                  shortTelemetryData.size(), platformEventStatus));

    std::array<uint8_t, 2> unknownTelemetryData{0x1, 0xFF};
    EXPECT_EQ(PLDM_ERROR,
              eventManager.handlePlatformEvent(
                  tid, PLDM_OEM_EVENT_CLASS_0xFD, unknownTelemetryData.data(),
                  unknownTelemetryData.size(), platformEventStatus));

    std::array<uint8_t, 2> pauseTelemetryData{
        0x1, pldm::platform::PLDM_TELEMETRY_PAUSE};
    EXPECT_EQ(PLDM_SUCCESS,
              eventManager.handlePlatformEvent(
                  tid, PLDM_OEM_EVENT_CLASS_0xFD, pauseTelemetryData.data(),
                  pauseTelemetryData.size(), platformEventStatus));

    std::array<uint8_t, 2> resumeTelemetryData{
        0x1, pldm::platform::PLDM_TELEMETRY_RESUME};
    EXPECT_EQ(PLDM_SUCCESS,
              eventManager.handlePlatformEvent(
                  tid, PLDM_OEM_EVENT_CLASS_0xFD, resumeTelemetryData.data(),
                  resumeTelemetryData.size(), platformEventStatus));

    std::array<uint8_t, 2> rediscoverTelemetryData{
        0x1, pldm::platform::PLDM_TELEMETRY_REDISCOVER};
    EXPECT_EQ(PLDM_SUCCESS,
              eventManager.handlePlatformEvent(
                  tid + 1, PLDM_OEM_EVENT_CLASS_0xFD,
                  rediscoverTelemetryData.data(),
                  rediscoverTelemetryData.size(), platformEventStatus));

    {
        EventManager baseEventManager(terminusManager, termini, fwUpdateManager,
                                      platformManager, sensorManager, false);
    }
}

TEST_F(EventManagerProtectedTest, handlePdrRepositoryChgEventDispatch)
{
    constexpr pldm::tid_t tid = 0x41;
    uint8_t platformEventStatus = 0;

    // 1. refreshAllRecords (eventDataFormat == REFRESH_ENTIRE_REPOSITORY).
    //    Decode succeeds and a detached rebuild is spawned. With no terminus
    //    registered for the tid the spawned coroutine returns inline without
    //    suspending, so the synchronous handler returns PLDM_SUCCESS.
    std::array<uint8_t, 2> refreshAll{
        static_cast<uint8_t>(REFRESH_ENTIRE_REPOSITORY), 0x0};
    EXPECT_EQ(PLDM_SUCCESS,
              eventManager.handlePlatformEvent(
                  tid, PLDM_PDR_REPOSITORY_CHG_EVENT, refreshAll.data(),
                  refreshAll.size(), platformEventStatus));

    // 2. recordsAdded (FORMAT_IS_PDR_HANDLES, one change record, one handle).
    //    Decode succeeds; the lightweight add path is spawned detached.
    std::array<uint8_t, 8> addedRecord{
        static_cast<uint8_t>(FORMAT_IS_PDR_HANDLES), // eventDataFormat
        0x1,                                         // numberOfChangeRecords
        static_cast<uint8_t>(PLDM_RECORDS_ADDED),    // eventDataOperation
        0x1,                                         // numberOfChangeEntries
        0x10,
        0x00,
        0x00,
        0x00}; // pdrRecordHandle = 0x10
    EXPECT_EQ(PLDM_SUCCESS,
              eventManager.handlePlatformEvent(
                  tid, PLDM_PDR_REPOSITORY_CHG_EVENT, addedRecord.data(),
                  addedRecord.size(), platformEventStatus));

    // 3. Unsupported eventDataFormat (FORMAT_IS_PDR_TYPES) is rejected.
    std::array<uint8_t, 2> typeFormat{static_cast<uint8_t>(FORMAT_IS_PDR_TYPES),
                                      0x0};
    EXPECT_EQ(PLDM_ERROR,
              eventManager.handlePlatformEvent(
                  tid, PLDM_PDR_REPOSITORY_CHG_EVENT, typeFormat.data(),
                  typeFormat.size(), platformEventStatus));

    // 4. Malformed (too short to decode the event header) is rejected.
    std::array<uint8_t, 1> tooShort{0x0};
    EXPECT_EQ(PLDM_ERROR,
              eventManager.handlePlatformEvent(
                  tid, PLDM_PDR_REPOSITORY_CHG_EVENT, tooShort.data(),
                  tooShort.size(), platformEventStatus));
}

TEST_F(EventManagerProtectedTest, handlePlatformEventAdditionalCoverage)
{
    constexpr pldm::tid_t tid = 0x31;
    constexpr uint16_t sensorId = 0x51;
    std::string uuid("00000000-0000-0000-0000-000000000131");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid]->setTerminusName("ProcessorModule_31");

    auto pdr = makeStateSensorPdr(sensorId, PLDM_ENTITY_SYS_BOARD, 1,
                                  PLDM_STATESET_ID_HEALTHSTATE, 0x3);
    termini[tid]->pdrs.emplace_back(pdr);
    ASSERT_TRUE(termini[tid]->parsePDRs());
    ASSERT_EQ(1u, termini[tid]->stateSensors.size());

    pldm::MctpInfo mctpInfo(
        14, "f72d6f90-5675-11ed-9b6a-0242ac120131",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    auto mappedTid = terminusManager.mapTid(mctpInfo, tid);
    ASSERT_TRUE(mappedTid.has_value());
    ASSERT_EQ(tid, mappedTid.value());

    uint8_t platformEventStatus = 0;
    auto stateEventData = makeStateSensorEventData(
        sensorId, PLDM_STATE_SENSOR_STATE, 0,
        PLDM_STATESET_HEALTH_STATE_CRITICAL, PLDM_STATESET_HEALTH_STATE_NORMAL);
    EXPECT_EQ(PLDM_SUCCESS, eventManager.handlePlatformEvent(
                                tid, PLDM_SENSOR_EVENT, stateEventData.data(),
                                stateEventData.size(), platformEventStatus));

    auto sensorOpData = makeSensorOpEventData(sensorId, PLDM_SENSOR_ENABLED,
                                              PLDM_SENSOR_DISABLED);
    EXPECT_EQ(PLDM_SUCCESS, eventManager.handlePlatformEvent(
                                tid, PLDM_SENSOR_EVENT, sensorOpData.data(),
                                sensorOpData.size(), platformEventStatus));
    EXPECT_EQ(PLDM_EVENT_LOGGING_REJECTED, platformEventStatus);

    uint8_t dummyData = 0;
    EXPECT_EQ(PLDM_SUCCESS, eventManager.handlePlatformEvent(
                                tid, PLDM_MESSAGE_POLL_EVENT, &dummyData, 0,
                                platformEventStatus));
    EXPECT_TRUE(termini[tid]->pollEvent);

    EXPECT_EQ(PLDM_SUCCESS, eventManager.handlePlatformEvent(
                                tid, PLDM_OEM_EVENT_CLASS_0xFB, &dummyData, 0,
                                platformEventStatus));

    std::vector<uint8_t> cperShortData{0x01};
    EXPECT_NE(PLDM_SUCCESS, eventManager.handlePlatformEvent(
                                tid, PLDM_CPER_EVENT, cperShortData.data(),
                                cperShortData.size(), platformEventStatus));

    std::vector<uint8_t> cperData{0x01, 0x00, 0x00, 0x00};
    EXPECT_EQ(PLDM_SUCCESS, eventManager.handlePlatformEvent(
                                tid, PLDM_CPER_EVENT, cperData.data(),
                                cperData.size(), platformEventStatus));

    std::vector<uint8_t> smbiosShortData{0x01};
    EXPECT_NE(PLDM_SUCCESS,
              eventManager.handlePlatformEvent(
                  tid, PLDM_OEM_EVENT_CLASS_0xFC, smbiosShortData.data(),
                  smbiosShortData.size(), platformEventStatus));

    std::vector<uint8_t> oemEventShortData{0x01, 0x00};
    EXPECT_EQ(PLDM_ERROR, eventManager.handlePlatformEvent(
                              tid, PLDM_OEM_EVENT_CLASS_ERROR_COUNTER,
                              oemEventShortData.data(),
                              oemEventShortData.size(), platformEventStatus));
}

/** A PCore dump is filed under the terminus it came from and the collector
 *  reads it back by exactly that path, so falling back to a default name does
 *  not merely mislabel the payload -- it stages one CPU's dump where the other
 *  CPU's is expected. Reject the event instead.
 */
TEST_F(EventManagerProtectedTest, pcoreDumpEventRejectedWhenTerminusUnresolved)
{
    uint8_t platformEventStatus = 0;
    const std::array<uint8_t, 5> payload{{0x02, 0x01, 0x01, 0x00, 0x5A}};

    // A terminus that is present but has not been named yet.
    constexpr pldm::tid_t unnamedTid = 0x81;
    std::string unnamedUuid("00000000-0000-0000-0000-000000000181");
    termini[unnamedTid] = std::make_shared<Terminus>(
        unnamedTid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, unnamedUuid,
        terminusManager);

    // A terminus entry that exists but holds nothing.
    constexpr pldm::tid_t nullTerminusTid = 0x82;
    termini[nullTerminusTid] = nullptr;

    // A TID that was never discovered at all.
    constexpr pldm::tid_t unknownTid = 0x83;

    // Whether the staging root is writable here depends on the environment,
    // so compare against what was on disk beforehand rather than assuming it
    // is empty.
    const auto defaultStagingFile =
        std::filesystem::path(pldm::oem_events::PLDM_EVENT_DIR) /
        "ProcessorModule_0" / pldm::oem_events::PCOREDUMP_FILE;
    const bool stagedBefore = std::filesystem::exists(defaultStagingFile);

    for (auto tid : {unnamedTid, nullTerminusTid, unknownTid})
    {
        platformEventStatus = 0;
        EXPECT_EQ(PLDM_ERROR,
                  eventManager.handlePlatformEvent(
                      tid, PLDM_OEM_EVENT_CLASS_PCOREDUMP, payload.data(),
                      payload.size(), platformEventStatus))
            << "tid " << static_cast<unsigned>(tid);
        EXPECT_EQ(PLDM_EVENT_LOGGING_REJECTED, platformEventStatus)
            << "tid " << static_cast<unsigned>(tid);
    }

    // None of those may have staged a dump under the default terminus name on
    // the way out.
    EXPECT_EQ(stagedBefore, std::filesystem::exists(defaultStagingFile));
}

TEST_F(EventManagerTest, getSensorThresholdEventDataSensorEventInfoCoverage)
{
    auto sensorEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    sensorEventInfo->impactedComponent = "CPU0";
    sensorEventInfo->eventIdsMap["PLDM_SENSOR_UPPERFATAL"] = "EID_UPPER_FATAL";
    sensorEventInfo->eventIdsMap["PLDM_SENSOR_UPPERCRITICAL"] =
        "EID_UPPER_CRITICAL";
    sensorEventInfo->eventIdsMap["PLDM_SENSOR_UPPERWARNING"] =
        "EID_UPPER_WARNING";
    sensorEventInfo->eventIdsMap["PLDM_SENSOR_LOWERWARNING"] =
        "EID_LOWER_WARNING";
    sensorEventInfo->eventIdsMap["PLDM_SENSOR_LOWERCRITICAL"] =
        "EID_LOWER_CRITICAL";
    sensorEventInfo->eventIdsMap["PLDM_SENSOR_LOWERFATAL"] = "EID_LOWER_FATAL";

    const std::array<uint8_t, 6> states{
        PLDM_SENSOR_UPPERFATAL,    PLDM_SENSOR_UPPERCRITICAL,
        PLDM_SENSOR_UPPERWARNING,  PLDM_SENSOR_LOWERWARNING,
        PLDM_SENSOR_LOWERCRITICAL, PLDM_SENSOR_LOWERFATAL};

    for (const auto previousState : states)
    {
        for (const auto currentState : states)
        {
            auto [messageId, eventId, impactedComponent] =
                eventManager.getSensorThresholdEventData(
                    previousState, currentState, sensorEventInfo);
            EXPECT_EQ("CPU0", impactedComponent);
            if (!eventId.empty())
            {
                EXPECT_NE(std::string::npos, eventId.find("EID_"));
            }
            if (!messageId.empty())
            {
                EXPECT_NE(std::string::npos, messageId.find("OpenBMC"));
            }
        }
    }
}

TEST_F(EventManagerPollingTest, pollForPlatformEventTaskMultipartCoverage)
{
    constexpr pldm::tid_t tid = 0x21;
    std::string uuid("00000000-0000-0000-0000-000000000121");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);

    pldm::MctpInfo mctpInfo(
        13, "f72d6f90-5675-11ed-9b6a-0242ac120121",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    auto mappedTid = terminusManager.mapTid(mctpInfo, tid);
    ASSERT_TRUE(mappedTid.has_value());
    ASSERT_EQ(tid, mappedTid.value());

    std::vector<uint8_t> eventChunk1{0xAB, 0xCD};
    std::vector<uint8_t> eventChunk2{0xEF};
    std::vector<uint8_t> fullEvent = eventChunk1;
    fullEvent.insert(fullEvent.end(), eventChunk2.begin(), eventChunk2.end());
    uint32_t checksum = pldm_edac_crc32(fullEvent.data(), fullEvent.size());

    auto first = makePollForPlatformEventResponse(
        tid, 1, PLDM_PLATFORM_TRANSFER_START, 0xFE, eventChunk1);
    auto second = makePollForPlatformEventResponse(
        tid, 1, PLDM_PLATFORM_TRANSFER_END, 0xFE, eventChunk2, checksum);
    auto third = makePollForPlatformEventResponse(tid, 0xFFFF, 0, 0,
                                                  std::vector<uint8_t>{});
    auto fourth = makePollForPlatformEventResponse(tid, 0x0000, 0, 0,
                                                   std::vector<uint8_t>{});

    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(first));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(second));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(third));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(fourth));

    auto rc =
        stdexec::sync_wait(eventManager.pollForPlatformEventTask(tid, 64));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
}

TEST(OemEventsCoverage, validationPaths)
{
    std::array<uint8_t, 2> tooSmallData{0x1, 0x0};
    EXPECT_FALSE(pldm::oem_events::handleCperErrorCountEvent(
        "ProcessorModule_0", tooSmallData.data(), tooSmallData.size()));

    std::vector<uint8_t> mismatchData{
        0x02, 0x01, 0x06, 0x00, // version, type, payload size (6)
        0x11, 0x22              // payload too short
    };
    EXPECT_FALSE(pldm::oem_events::handlePcieTelemetryEvent(
        "../unsafe\\name", mismatchData.data(), mismatchData.size()));

    std::vector<uint8_t> sizeMatchedData{
        0x01, 0x00, 0x01, 0x00, // version, type, payload size (1)
        0x5A                    // payload
    };
    (void)pldm::oem_events::handlePCoreDumpEvent(
        "ProcessorModule_0", sizeMatchedData.data(), sizeMatchedData.size());
}
