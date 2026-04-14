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
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "platform-mc/manager.hpp"
#undef private
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "platform-mc/oem_events.hpp"
#include "platform-mc/platform_manager.hpp"
#include "platform-mc/pldmServiceReadyInterface.hpp"
#include "platform-mc/sensor_manager.hpp"
#include "platform-mc/terminus_manager.hpp"
#include "test/test_instance_id.hpp"

#include <libpldm/edac.h>

#include <sdbusplus/test/sdbus_mock.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <future>
#include <optional>
#include <thread>

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
        0,   // entityType
        1,
        0,   // entityInstanceNumber
        0x1,
        0x0, // containerID
        PLDM_NO_INIT,
        false,
        PLDM_SENSOR_UNIT_DEGRESS_C, // baseUint
        1,                          // unitModifier
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        true,
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
        0,
        0,
        0,
        2,
        0,
        0,
        0,
        0,
        0x80,
        0x3f,
        0,
        0,
        0x80,
        0x3f,
        255,
        0,
        PLDM_RANGE_FIELD_FORMAT_UINT8,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
    };
}

static std::shared_ptr<NumericSensor> makeEventNumericSensor(
    pldm::tid_t tid, uint16_t sensorId, const std::string& sensorName,
    const std::string& inventoryPath,
    std::shared_ptr<pldm::utils::SensorEventInfo> sensorEventInfo = nullptr)
{
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

    std::string mutableName = sensorName;
    std::string mutableInventoryPath = inventoryPath;
    return std::make_shared<NumericSensor>(
        tid, false, sensorPdr, mutableName, mutableInventoryPath,
        sensorEventInfo);
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

static StateSetInfo makeBootRequestStateSetInfo()
{
    return std::make_tuple(
        EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
        std::vector<StateSetData>{
            {PLDM_STATESET_ID_BOOT_REQUEST,
             PossibleStates{PLDM_STATESET_BOOT_REQUEST_NORMAL,
                            PLDM_STATESET_BOOT_REQUEST_REQUESTED}}});
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
    using EventManager::pollForPlatformEventMessage;
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
        fwUpdateManager(nullptr, event, reqHandler, instanceIdDb, "", false),
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
        fwUpdateManager(nullptr, event, reqHandler, instanceIdDb, "", false)
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
        fwUpdateManager(nullptr, event, reqHandler, instanceIdDb, "", false),
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
        fwUpdateManager(nullptr, event, reqHandler, instanceIdDb, "", false),
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

class EventManagerDbusMockTest : public EventManagerProtectedTest
{
  protected:
    void SetUp() override
    {
        auto& busRef = pldm::utils::DBusHandler::getBus();
        auto mockBus = sdbusplus::get_mocked_new(&mock);
        savedBus.emplace(std::move(busRef));
        busRef = std::move(mockBus);
        busSwapped = true;
    }

    void TearDown() override
    {
        if (busSwapped)
        {
            pldm::utils::DBusHandler::getBus() = std::move(*savedBus);
            savedBus.reset();
        }
    }

    void expectNewMethodCall(const char* service, const char* path,
                             const char* interface, const char* method)
    {
        EXPECT_CALL(mock, sd_bus_message_new_method_call(
                              testing::_, testing::_, testing::StrEq(service),
                              testing::StrEq(path), testing::StrEq(interface),
                              testing::StrEq(method)))
            .WillOnce(testing::Return(0));
    }

    void expectAppendString(const char* value)
    {
        EXPECT_CALL(mock, sd_bus_message_append_basic(
                              nullptr, SD_BUS_TYPE_STRING,
                              testing::MatcherCast<const void*>(
                                  testing::SafeMatcherCast<const char*>(
                                      testing::StrEq(value)))))
            .WillOnce(testing::Return(0));
    }

    void expectOpenContainer(char type, const char* contents)
    {
        EXPECT_CALL(mock, sd_bus_message_open_container(
                              nullptr, type, testing::StrEq(contents)))
            .WillOnce(testing::Return(0));
    }

    void expectCloseContainer()
    {
        EXPECT_CALL(mock, sd_bus_message_close_container(nullptr))
            .WillOnce(testing::Return(0));
    }

    void expectBusCallWithReply(uint64_t timeout = dbusTimeout)
    {
        EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, timeout, testing::_,
                                      testing::_))
            .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                         sd_bus_message** reply) {
                *reply = nullptr;
                return 0;
            });
    }

    void expectBusCallNoReply(uint64_t timeout = 0)
    {
        EXPECT_CALL(mock,
                    sd_bus_call(nullptr, nullptr, timeout, testing::_, nullptr))
            .WillOnce(testing::Return(0));
    }

    void expectReadString(const char* value)
    {
        EXPECT_CALL(mock, sd_bus_message_read_basic(nullptr, SD_BUS_TYPE_STRING,
                                                    testing::_))
            .WillOnce([value](sd_bus_message*, char, void* output) {
                *static_cast<const char**>(output) = value;
                return 0;
            });
    }

    void expectReadOneServiceMapEntry(const char* service,
                                      const char* interface)
    {
        EXPECT_CALL(mock, sd_bus_message_enter_container(
                              nullptr, SD_BUS_TYPE_ARRAY, testing::_))
            .WillOnce(testing::Return(0));
        EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, false))
            .WillOnce(testing::Return(0));
        EXPECT_CALL(mock, sd_bus_message_enter_container(
                              nullptr, SD_BUS_TYPE_DICT_ENTRY, testing::_))
            .WillOnce(testing::Return(0));
        expectReadString(service);
        EXPECT_CALL(mock, sd_bus_message_enter_container(
                              nullptr, SD_BUS_TYPE_ARRAY, testing::_))
            .WillOnce(testing::Return(0));
        EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, false))
            .WillOnce(testing::Return(0));
        expectReadString(interface);
        EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, false))
            .WillOnce(testing::Return(1));
        EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
            .WillOnce(testing::Return(0));
        EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
            .WillOnce(testing::Return(0));
        EXPECT_CALL(mock, sd_bus_message_at_end(nullptr, false))
            .WillOnce(testing::Return(1));
        EXPECT_CALL(mock, sd_bus_message_exit_container(nullptr))
            .WillOnce(testing::Return(0));
    }

    void expectGetObjectCall(const char* objectPath, const char* interface,
                             const char* service)
    {
        expectNewMethodCall(pldm::utils::ObjectMapper::default_service,
                            pldm::utils::ObjectMapper::instance_path,
                            pldm::utils::ObjectMapper::interface, "GetObject");
        expectAppendString(objectPath);
        expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
        expectAppendString(interface);
        expectCloseContainer();
        expectBusCallWithReply();
        expectReadOneServiceMapEntry(service, interface);
    }

    void expectStringMap(
        const std::vector<std::pair<std::string, std::string>>& entries)
    {
        expectOpenContainer(SD_BUS_TYPE_ARRAY, "{ss}");
        for (const auto& [key, value] : entries)
        {
            expectOpenContainer(SD_BUS_TYPE_DICT_ENTRY, "ss");
            expectAppendString(key.c_str());
            expectAppendString(value.c_str());
            expectCloseContainer();
        }
        expectCloseContainer();
    }

    testing::StrictMock<sdbusplus::SdBusMock> mock;
    std::optional<sdbusplus::bus_t> savedBus;
    bool busSwapped = false;
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
        manager.handlePcieLtssmEvent(msg, 64, 1, tid, 65, platformEventStatus),
        PLDM_ERROR_INVALID_LENGTH);
    EXPECT_EQ(
        manager.handlePcieLtssmEvent(msg, 64, 1, tid, 0, platformEventStatus),
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

TEST_F(PlatformMcManagerTest, managerEventHandlerBoundaryCoverage)
{
    Manager manager(event, reqHandler, instanceIdDb, fwUpdateManager, false,
                    true);

    std::vector<uint8_t> request(sizeof(pldm_msg_hdr) + 64, 0);
    auto* msg = reinterpret_cast<pldm_msg*>(request.data());
    uint8_t platformEventStatus = 0;
    constexpr uint8_t tid = 1;
    const size_t payloadLength = request.size() - sizeof(pldm_msg_hdr);
    const std::array<size_t, 2> offsets{payloadLength, payloadLength + 1};
    const size_t boundaryOffset = offsets[0];
    const size_t invalidOffset = offsets[1];

    EXPECT_EQ(
        manager.handleErrorCounterEvent(msg, payloadLength, 1, tid,
                                        invalidOffset, platformEventStatus),
        PLDM_ERROR_INVALID_LENGTH);
    EXPECT_EQ(
        manager.handleErrorCounterEvent(msg, payloadLength, 1, tid,
                                        boundaryOffset, platformEventStatus),
        PLDM_SUCCESS);
    EXPECT_EQ(
        manager.handlePcieTelemetryEvent(msg, payloadLength, 1, tid,
                                         invalidOffset, platformEventStatus),
        PLDM_ERROR_INVALID_LENGTH);
    EXPECT_EQ(
        manager.handlePcieTelemetryEvent(msg, payloadLength, 1, tid,
                                         boundaryOffset, platformEventStatus),
        PLDM_SUCCESS);
    EXPECT_EQ(manager.handlePcieLtssmEvent(msg, payloadLength, 1, tid,
                                           invalidOffset, platformEventStatus),
              PLDM_ERROR_INVALID_LENGTH);
    EXPECT_EQ(manager.handlePcieLtssmEvent(msg, payloadLength, 1, tid,
                                           boundaryOffset, platformEventStatus),
              PLDM_SUCCESS);

    std::vector<uint8_t> pollEventData(sizeof(pldm_message_poll_event_data), 0);
    pollEventData[0] = 0x02; // format_version
    pollEventData[1] = 0x01; // event_id (little-endian)
    pollEventData[2] = 0x00;
    pollEventData[3] = 0x01; // data_transfer_handle (little-endian)
    pollEventData[4] = 0x00;
    pollEventData[5] = 0x00;
    pollEventData[6] = 0x00;
    std::copy(pollEventData.begin(), pollEventData.end(), msg->payload);
    EXPECT_EQ(manager.handlePldmMessagePollEvent(msg, pollEventData.size(), 1,
                                                 tid, 0, platformEventStatus),
              PLDM_ERROR_INVALID_DATA);
}

TEST_F(PlatformMcManagerTest, managerMappedTerminusLifecycleCoverage)
{
    Manager manager(event, reqHandler, instanceIdDb, fwUpdateManager, false,
                    true);

    constexpr pldm::tid_t platformTid = 0x24;
    constexpr pldm::tid_t nonPlatformTid = 0x25;
    const pldm::MctpInfo platformInfo(
        24, "00000000-0000-0000-0000-000000000024",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    const pldm::MctpInfo nonPlatformInfo(
        25, "00000000-0000-0000-0000-000000000025",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);

    ASSERT_TRUE(
        manager.terminusManager.mapTid(platformInfo, platformTid).has_value());
    ASSERT_TRUE(manager.terminusManager.mapTid(nonPlatformInfo, nonPlatformTid)
                    .has_value());

    std::string platformUuid = std::get<1>(platformInfo);
    auto platformTerminus = std::make_shared<Terminus>(
        platformTid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, platformUuid,
        manager.terminusManager);
    platformTerminus->sensorPollingTaskRc.reset();
    manager.termini[platformTid] = platformTerminus;

    std::string nonPlatformUuid = std::get<1>(nonPlatformInfo);
    auto nonPlatformTerminus =
        std::make_shared<Terminus>(nonPlatformTid, 1 << PLDM_BASE,
                                   nonPlatformUuid, manager.terminusManager);
    nonPlatformTerminus->resumed = false;
    manager.termini[nonPlatformTid] = nonPlatformTerminus;

    pldm::MctpInfos removedInfos{platformInfo, nonPlatformInfo};
    manager.handleRemovedMctpEndpoints(removedInfos);
    EXPECT_TRUE(platformTerminus->stopPolling);
    EXPECT_FALSE(nonPlatformTerminus->stopPolling);

    manager.updateMctpEndpointAvailability(platformInfo, true);
    EXPECT_FALSE(platformTerminus->stopPolling);
    manager.updateMctpEndpointAvailability(platformInfo, false);
    EXPECT_TRUE(platformTerminus->stopPolling);

    manager.onlineMctpEndpoint(std::get<1>(platformInfo),
                               std::get<0>(platformInfo));
    EXPECT_FALSE(platformTerminus->stopPolling);
    manager.offlineMctpEndpoint(std::get<1>(platformInfo),
                                std::get<0>(platformInfo));
    EXPECT_TRUE(platformTerminus->stopPolling);

    auto resumeRc = stdexec::sync_wait(manager.resumeTerminus(nonPlatformTid));
    ASSERT_TRUE(resumeRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*resumeRc));
    EXPECT_TRUE(nonPlatformTerminus->resumed);
}

TEST_F(PlatformMcManagerTest, managerEndpointNullAndNonPlatformCoverage)
{
    Manager manager(event, reqHandler, instanceIdDb, fwUpdateManager, false,
                    true);

    constexpr pldm::tid_t nullTid = 0x2A;
    constexpr pldm::tid_t nonPlatformTid = 0x2B;
    const pldm::MctpInfo nullInfo(
        42, "00000000-0000-0000-0000-00000000002a",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    const pldm::MctpInfo nonPlatformInfo(
        43, "00000000-0000-0000-0000-00000000002b",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);

    ASSERT_TRUE(manager.terminusManager.mapTid(nullInfo, nullTid).has_value());
    ASSERT_TRUE(manager.terminusManager.mapTid(nonPlatformInfo, nonPlatformTid)
                    .has_value());

    std::string nonPlatformUuid = std::get<1>(nonPlatformInfo);
    auto nonPlatformTerminus =
        std::make_shared<Terminus>(nonPlatformTid, 1 << PLDM_BASE,
                                   nonPlatformUuid, manager.terminusManager);
    manager.termini[nonPlatformTid] = nonPlatformTerminus;

    manager.handleRemovedMctpEndpoints({nullInfo});
    manager.updateMctpEndpointAvailability(nonPlatformInfo, true);
    manager.updateMctpEndpointAvailability(nonPlatformInfo, false);
    manager.onlineMctpEndpoint(std::get<1>(nonPlatformInfo),
                               std::get<0>(nonPlatformInfo));
    manager.offlineMctpEndpoint(std::get<1>(nonPlatformInfo),
                                std::get<0>(nonPlatformInfo));

    EXPECT_FALSE(nonPlatformTerminus->stopPolling);
}

TEST_F(PlatformMcManagerTest, managerMessagePollValidFormatCoverage)
{
    Manager manager(event, reqHandler, instanceIdDb, fwUpdateManager, false,
                    true);

    std::vector<uint8_t> request(
        sizeof(pldm_msg_hdr) + PLDM_MSG_POLL_EVENT_LENGTH, 0);
    auto* msg = reinterpret_cast<pldm_msg*>(request.data());

    pldm_message_poll_event pollEvent{};
    pollEvent.format_version = 0x01;
    pollEvent.event_id = 0x1234;
    pollEvent.data_transfer_handle = 0x11223344;

    std::array<uint8_t, PLDM_MSG_POLL_EVENT_LENGTH> pollEventData{
        pollEvent.format_version,
        static_cast<uint8_t>(pollEvent.event_id & 0xFF),
        static_cast<uint8_t>((pollEvent.event_id >> 8) & 0xFF),
        static_cast<uint8_t>(pollEvent.data_transfer_handle & 0xFF),
        static_cast<uint8_t>((pollEvent.data_transfer_handle >> 8) & 0xFF),
        static_cast<uint8_t>((pollEvent.data_transfer_handle >> 16) & 0xFF),
        static_cast<uint8_t>((pollEvent.data_transfer_handle >> 24) & 0xFF),
    };

    std::copy(pollEventData.begin(), pollEventData.end(), msg->payload);

    uint8_t platformEventStatus = 0;
    EXPECT_EQ(PLDM_SUCCESS,
              manager.handlePldmMessagePollEvent(msg, pollEventData.size(), 1,
                                                 0x32, 0, platformEventStatus));
}

TEST_F(PlatformMcManagerTest, managerSensorPollingManagerPathCoverage)
{
    Manager manager(event, reqHandler, instanceIdDb, fwUpdateManager, false,
                    true);

    constexpr pldm::tid_t tid = 0x26;
    std::string uuid("00000000-0000-0000-0000-000000000226");
    auto terminus = std::make_shared<Terminus>(tid, 1 << PLDM_BASE, uuid,
                                               manager.terminusManager);
    terminus->resumed = false;
    terminus->pollEvent = true;
    terminus->initSensorList = false;
    terminus->ready = false;
    manager.termini[tid] = terminus;
    manager.sensorManager.pollingTime = 0;

    auto pollingFuture = std::async(std::launch::async, [&]() {
        return stdexec::sync_wait(
            manager.sensorManager.doSensorPollingTask(tid));
    });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < deadline &&
           (!terminus->resumed || terminus->pollEvent || !terminus->ready))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    terminus->stopPolling = true;

    auto pollingRc = pollingFuture.get();
    ASSERT_TRUE(pollingRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*pollingRc));
    EXPECT_TRUE(terminus->resumed);
    EXPECT_FALSE(terminus->pollEvent);
    EXPECT_TRUE(terminus->ready);
}

TEST_F(PlatformMcManagerTest, managerSensorPollingMissingTidCoverage)
{
    Manager manager(event, reqHandler, instanceIdDb, fwUpdateManager, false,
                    true);

    auto pollingRc =
        stdexec::sync_wait(manager.sensorManager.doSensorPollingTask(
            static_cast<pldm::tid_t>(0x60)));
    ASSERT_TRUE(pollingRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*pollingRc));

    EXPECT_NO_THROW(
        manager.sensorManager.doSensorPolling(static_cast<pldm::tid_t>(0x60)));
    EXPECT_NO_THROW(
        manager.sensorManager.startPolling(static_cast<pldm::tid_t>(0x60)));
    EXPECT_NO_THROW(
        manager.sensorManager.stopPolling(static_cast<pldm::tid_t>(0x60)));
}

TEST_F(PlatformMcManagerTest, managerSensorPollingNonPlatformStartCoverage)
{
    Manager manager(event, reqHandler, instanceIdDb, fwUpdateManager, false,
                    true);

    constexpr pldm::tid_t tid = 0x61;
    std::string uuid("00000000-0000-0000-0000-000000000261");
    auto terminus = std::make_shared<Terminus>(tid, 1 << PLDM_BASE, uuid,
                                               manager.terminusManager);
    terminus->stopPolling = true;
    manager.termini[tid] = terminus;

    manager.sensorManager.startPolling(tid);

    EXPECT_TRUE(terminus->stopPolling);
    EXPECT_TRUE(terminus->sensorPollingTaskRc.has_value());
}

TEST_F(PlatformMcManagerTest, managerSensorPollingExistingTaskCoverage)
{
    Manager manager(event, reqHandler, instanceIdDb, fwUpdateManager, false,
                    true);

    constexpr pldm::tid_t tid = 0x62;
    std::string uuid("00000000-0000-0000-0000-000000000262");
    auto terminus = std::make_shared<Terminus>(tid, 1 << PLDM_BASE, uuid,
                                               manager.terminusManager);
    terminus->sensorPollingTaskRc.reset();
    manager.termini[tid] = terminus;

    manager.sensorManager.doSensorPolling(tid);

    EXPECT_FALSE(terminus->sensorPollingTaskRc.has_value());
}

TEST_F(PlatformMcManagerTest, managerSensorPollingDetachedCompletionCoverage)
{
    Manager manager(event, reqHandler, instanceIdDb, fwUpdateManager, false,
                    true);

    constexpr pldm::tid_t tid = 0x63;
    std::string uuid("00000000-0000-0000-0000-000000000263");
    auto terminus = std::make_shared<Terminus>(tid, 1 << PLDM_BASE, uuid,
                                               manager.terminusManager);
    terminus->stopPolling = true;
    terminus->initSensorList = false;
    manager.termini[tid] = terminus;

    manager.sensorManager.doSensorPolling(tid);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < deadline &&
           !terminus->sensorPollingTaskRc.has_value())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ASSERT_TRUE(terminus->sensorPollingTaskRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, terminus->sensorPollingTaskRc.value());
}

TEST_F(PlatformMcManagerTest, managerSensorPollingPollEventBreakCoverage)
{
    Manager manager(event, reqHandler, instanceIdDb, fwUpdateManager, false,
                    true);

    constexpr pldm::tid_t tid = 0x64;
    std::string uuid("00000000-0000-0000-0000-000000000264");
    auto terminus = std::make_shared<Terminus>(tid, 1 << PLDM_BASE, uuid,
                                               manager.terminusManager);
    terminus->resumed = true;
    terminus->pollEvent = true;
    terminus->initSensorList = false;
    manager.termini[tid] = terminus;
    manager.sensorManager.pollingTime = 0;

    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis64"};
    std::string numericEffecterName{"manager_poll_numeric_effecter"};
    auto numericEffecter = std::make_shared<NumericEffecter>(
        tid, false, makeNumericEffecterValuePdrForPolling(0x164),
        numericEffecterName, inventoryPath, manager.terminusManager);
    numericEffecter->needUpdate = false;
    terminus->numericEffecters.emplace_back(numericEffecter);

    auto stateEffecter = std::make_shared<StateEffecter>(
        tid, false, 0x264, makeBootRequestStateSetInfo(), nullptr,
        inventoryPath, manager.terminusManager);
    stateEffecter->needUpdate = false;
    terminus->stateEffecters.emplace_back(stateEffecter);

    terminus->pdrs.emplace_back(makeStateSensorPdr(
        0x364, PLDM_ENTITY_SYS_BOARD, 1, PLDM_STATESET_ID_HEALTHSTATE));
    ASSERT_TRUE(terminus->parsePDRs());
    auto stateSensor = terminus->stateSensors.front();
    stateSensor->needUpdate = false;

    auto numericSensorPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    auto numericPdr = makeNumericSensorPdr(0x464);
    ASSERT_EQ(PLDM_SUCCESS, decode_numeric_sensor_pdr_data(
                                numericPdr.data(), numericPdr.size(),
                                numericSensorPdr.get()));
    std::string numericSensorName{"manager_poll_priority_sensor"};
    auto prioritySensor = std::make_shared<NumericSensor>(
        tid, false, numericSensorPdr, numericSensorName, inventoryPath,
        nullptr);
    prioritySensor->updateTime = std::numeric_limits<uint64_t>::max();
    terminus->prioritySensors.emplace_back(prioritySensor);

    uint64_t now = 0;
    sd_event_now(event.get(), CLOCK_MONOTONIC, &now);
    stateSensor->setLastUpdatedTimeStamp(now);
    terminus->roundRobinSensors.push(stateSensor);

    std::atomic<bool> keepSettingPollEvent{true};
    auto toggleThread = std::thread([&]() {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (std::chrono::steady_clock::now() < deadline &&
               terminus->pollEvent)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        while (keepSettingPollEvent.load())
        {
            terminus->pollEvent = true;
            std::this_thread::yield();
        }
    });

    auto pollingFuture = std::async(std::launch::async, [&]() {
        return stdexec::sync_wait(
            manager.sensorManager.doSensorPollingTask(tid));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    keepSettingPollEvent = false;
    terminus->stopPolling = true;
    toggleThread.join();

    auto pollingRc = pollingFuture.get();
    ASSERT_TRUE(pollingRc.has_value());
    EXPECT_TRUE(std::get<0>(*pollingRc) == PLDM_SUCCESS ||
                std::get<0>(*pollingRc) == PLDM_ERROR);
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

TEST_F(EventManagerProtectedTest,
       handlePlatformEventMissingMctpAndNullInventoryCoverage)
{
    uint8_t platformEventStatus = 0;
    uint8_t dummy = 0;

    EXPECT_EQ(PLDM_ERROR,
              eventManager.handlePlatformEvent(0x90, PLDM_OEM_EVENT_CLASS_0xFB,
                                               &dummy, 0, platformEventStatus));

    constexpr pldm::tid_t nullInventoryTid = 0x91;
    termini[nullInventoryTid] = nullptr;
    std::array<uint8_t, 3> shortInventoryData{0x01, 0x00, 0x00};
    platformEventStatus = 0;
    EXPECT_EQ(PLDM_ERROR, eventManager.handlePlatformEvent(
                              nullInventoryTid, PLDM_OEM_EVENT_CLASS_0xF3,
                              shortInventoryData.data(),
                              shortInventoryData.size(), platformEventStatus));
    EXPECT_EQ(PLDM_EVENT_LOGGING_REJECTED, platformEventStatus);
}

TEST_F(EventManagerProtectedTest,
       handlePlatformEventInventoryHandlerFailureCoverage)
{
    constexpr pldm::tid_t tid = 0x92;
    std::string uuid("00000000-0000-0000-0000-000000000192");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid]->setTerminusName("InventoryTerminus");

    std::array<uint8_t, 4> emptyInventoryPayload{0x01, 0x00, 0x00, 0x00};
    uint8_t platformEventStatus = 0;
    EXPECT_EQ(PLDM_ERROR,
              eventManager.handlePlatformEvent(
                  tid, PLDM_OEM_EVENT_CLASS_0xF3, emptyInventoryPayload.data(),
                  emptyInventoryPayload.size(), platformEventStatus));
    EXPECT_EQ(PLDM_EVENT_LOGGING_REJECTED, platformEventStatus);
}

TEST_F(EventManagerProtectedTest,
       handlePlatformEventInventoryDefaultNameFailureCoverage)
{
    std::array<uint8_t, 4> emptyInventoryPayload{0x02, 0x01, 0x00, 0x00};
    uint8_t platformEventStatus = 0;
    EXPECT_EQ(PLDM_ERROR,
              eventManager.handlePlatformEvent(
                  0x93, PLDM_OEM_EVENT_CLASS_0xF3, emptyInventoryPayload.data(),
                  emptyInventoryPayload.size(), platformEventStatus));
    EXPECT_EQ(PLDM_EVENT_LOGGING_REJECTED, platformEventStatus);
}

TEST_F(EventManagerProtectedTest,
       processNumericSensorEventUnsupportedSizeCoverage)
{
    constexpr pldm::tid_t tid = 0x94;
    constexpr uint16_t sensorId = 0x294;
    std::string uuid("00000000-0000-0000-0000-000000000294");
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

    std::string sensorName{"unsupported_size_sensor"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis92"};
    auto sensor = std::make_shared<NumericSensor>(
        tid, false, sensorPdr, sensorName, inventoryPath, nullptr);
    auto sensorEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    sensorEventInfo->impactedComponent = "CPU92";
    sensor->updateSensorEventInfo(sensorEventInfo);
    termini[tid]->numericSensors.emplace_back(sensor);

    const std::array<uint8_t, 6> eventStates{
        PLDM_SENSOR_UPPERFATAL,    PLDM_SENSOR_UPPERCRITICAL,
        PLDM_SENSOR_UPPERWARNING,  PLDM_SENSOR_LOWERWARNING,
        PLDM_SENSOR_LOWERCRITICAL, PLDM_SENSOR_LOWERFATAL};

    for (const auto eventState : eventStates)
    {
        auto sensorData = makeNumericSensorEventStateData(
            eventState, PLDM_SENSOR_NORMAL, 0xFF, 0);
        eventManager.processNumericSensorEvent(tid, sensorId, sensorData.data(),
                                               sensorData.size());
    }
}

TEST_F(EventManagerProtectedTest, processStateSensorEventMissingSensorCoverage)
{
    constexpr pldm::tid_t tid = 0x93;
    constexpr uint16_t sensorId = 0x393;
    std::array<uint8_t, 3> sensorData{0x0, PLDM_STATESET_HEALTH_STATE_NORMAL,
                                      0x0};

    eventManager.processStateSensorEvent(tid, sensorId, sensorData.data(),
                                         sensorData.size());

    std::string uuid("00000000-0000-0000-0000-000000000393");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    eventManager.processStateSensorEvent(tid, sensorId, sensorData.data(),
                                         sensorData.size());
}

TEST_F(EventManagerProtectedTest,
       processStateSensorEventMultiSensorSearchCoverage)
{
    constexpr pldm::tid_t tid = 0x94;
    std::string uuid("00000000-0000-0000-0000-000000000394");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);

    constexpr std::array<uint16_t, 3> sensorIds{0x401, 0x402, 0x403};
    for (const auto sensorId : sensorIds)
    {
        termini[tid]->pdrs.emplace_back(makeStateSensorPdr(
            sensorId, PLDM_ENTITY_SYS_BOARD, 1, PLDM_STATESET_ID_HEALTHSTATE));
    }
    ASSERT_TRUE(termini[tid]->parsePDRs());
    ASSERT_EQ(sensorIds.size(), termini[tid]->stateSensors.size());

    const std::array<uint8_t, 3> sensorData{
        0x0, PLDM_STATESET_HEALTH_STATE_CRITICAL,
        PLDM_STATESET_HEALTH_STATE_NORMAL};
    eventManager.processStateSensorEvent(tid, sensorIds.back(),
                                         sensorData.data(), sensorData.size());

    ASSERT_EQ(1u, termini[tid]->stateSensors.back()->stateSets.size());
    auto [messageId, arg2, level, eventId, impactedComponent] =
        termini[tid]->stateSensors.back()->stateSets[0]->getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedCritical", messageId);
}

TEST_F(EventManagerProtectedTest,
       processStateSensorEventMultiSensorMissCoverage)
{
    constexpr pldm::tid_t tid = 0x95;
    std::string uuid("00000000-0000-0000-0000-000000000395");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);

    termini[tid]->pdrs.emplace_back(makeStateSensorPdr(
        0x411, PLDM_ENTITY_SYS_BOARD, 1, PLDM_STATESET_ID_HEALTHSTATE));
    termini[tid]->pdrs.emplace_back(makeStateSensorPdr(
        0x412, PLDM_ENTITY_SYS_BOARD, 1, PLDM_STATESET_ID_HEALTHSTATE));
    ASSERT_TRUE(termini[tid]->parsePDRs());
    ASSERT_EQ(2u, termini[tid]->stateSensors.size());

    const std::array<uint8_t, 3> sensorData{
        0x0, PLDM_STATESET_HEALTH_STATE_CRITICAL,
        PLDM_STATESET_HEALTH_STATE_NORMAL};
    eventManager.processStateSensorEvent(tid, 0x413, sensorData.data(),
                                         sensorData.size());

    for (const auto& sensor : termini[tid]->stateSensors)
    {
        ASSERT_EQ(1u, sensor->stateSets.size());
        auto [messageId, arg2, level, eventId,
              impactedComponent] = sensor->stateSets[0]->getEventData(nullptr);
        EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedOK", messageId);
    }
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

TEST_F(EventManagerTest, getSensorThresholdEventDataExpandedMatrixCoverage)
{
    auto sensorEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    sensorEventInfo->impactedComponent = "CPU_MATRIX";

    const std::array<uint8_t, 8> states{
        PLDM_SENSOR_UNKNOWN,       PLDM_SENSOR_NORMAL,
        PLDM_SENSOR_UPPERWARNING,  PLDM_SENSOR_UPPERCRITICAL,
        PLDM_SENSOR_UPPERFATAL,    PLDM_SENSOR_LOWERWARNING,
        PLDM_SENSOR_LOWERCRITICAL, PLDM_SENSOR_LOWERFATAL};

    for (const auto previousState : states)
    {
        for (const auto currentState : states)
        {
            auto [messageId, eventId, impactedComponent] =
                eventManager.getSensorThresholdEventData(
                    previousState, currentState, sensorEventInfo);
            EXPECT_TRUE(eventId.empty());
            if (currentState != PLDM_SENSOR_UNKNOWN &&
                currentState != PLDM_SENSOR_NORMAL)
            {
                EXPECT_EQ("CPU_MATRIX", impactedComponent);
            }
            if (!messageId.empty())
            {
                EXPECT_NE(std::string::npos, messageId.find("OpenBMC"));
            }
        }
    }
}

TEST_F(EventManagerTest, getSensorThresholdEventDataInvalidStateCoverage)
{
    auto sensorEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    sensorEventInfo->impactedComponent = "CPU_INVALID";
    sensorEventInfo->eventIdsMap["PLDM_SENSOR_UPPERWARNING"] =
        "EID_UPPER_WARNING";
    sensorEventInfo->eventIdsMap["PLDM_SENSOR_LOWERCRITICAL"] =
        "EID_LOWER_CRITICAL";

    auto [invalidPrevMessage, invalidPrevEvent, invalidPrevImpacted] =
        eventManager.getSensorThresholdEventData(0xFF, PLDM_SENSOR_UPPERWARNING,
                                                 sensorEventInfo);
    EXPECT_TRUE(invalidPrevMessage.empty());
    EXPECT_EQ("EID_UPPER_WARNING", invalidPrevEvent);
    EXPECT_EQ("CPU_INVALID", invalidPrevImpacted);

    auto [invalidCurrentMessage, invalidCurrentEvent, invalidCurrentImpacted] =
        eventManager.getSensorThresholdEventData(PLDM_SENSOR_LOWERCRITICAL,
                                                 0xFE, sensorEventInfo);
    EXPECT_TRUE(invalidCurrentMessage.empty());
    EXPECT_TRUE(invalidCurrentEvent.empty());
    EXPECT_TRUE(invalidCurrentImpacted.empty());

    auto [bothInvalidMessage, bothInvalidEvent, bothInvalidImpacted] =
        eventManager.getSensorThresholdEventData(0xFD, 0xFC, sensorEventInfo);
    EXPECT_TRUE(bothInvalidMessage.empty());
    EXPECT_TRUE(bothInvalidEvent.empty());
    EXPECT_TRUE(bothInvalidImpacted.empty());
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

TEST_F(EventManagerPollingTest, pollForPlatformEventTaskEdgeCoverage)
{
    constexpr pldm::tid_t tid = 0x22;
    std::string uuid("00000000-0000-0000-0000-000000000122");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);

    pldm::MctpInfo mctpInfo(
        22, "f72d6f90-5675-11ed-9b6a-0242ac120122",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    auto mappedTid = terminusManager.mapTid(mctpInfo, tid);
    ASSERT_TRUE(mappedTid.has_value());

    const std::vector<uint8_t> singleShotData{0x01, 0x02};
    const std::vector<uint8_t> chunk1{0x10, 0x20};
    const std::vector<uint8_t> chunk2{0x30};
    const uint32_t badChecksum =
        pldm_edac_crc32(chunk1.data(), chunk1.size()) + 1;

    auto startAndEnd = makePollForPlatformEventResponse(
        tid, 1, PLDM_PLATFORM_TRANSFER_START_AND_END, 0xEE, singleShotData);
    auto ackOnly = makePollForPlatformEventResponse(tid, 0xFFFF, 0, 0,
                                                    std::vector<uint8_t>{});
    auto start = makePollForPlatformEventResponse(
        tid, 2, PLDM_PLATFORM_TRANSFER_START, 0xEE, chunk1);
    auto end = makePollForPlatformEventResponse(
        tid, 2, PLDM_PLATFORM_TRANSFER_END, 0xEE, chunk2, badChecksum);
    auto done = makePollForPlatformEventResponse(tid, 0x0000, 0, 0,
                                                 std::vector<uint8_t>{});

    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(startAndEnd));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(ackOnly));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(start));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(end));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(done));

    auto rc =
        stdexec::sync_wait(eventManager.pollForPlatformEventTask(tid, 64));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
}

TEST_F(EventManagerPollingTest, pollForPlatformEventMessageDecodeCoverage)
{
    constexpr pldm::tid_t tid = 0x23;
    pldm::MctpInfo mctpInfo(
        23, "f72d6f90-5675-11ed-9b6a-0242ac120123",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    ASSERT_TRUE(terminusManager.mapTid(mctpInfo, tid).has_value());

    uint8_t completionCode = 0;
    uint8_t eventTid = 0;
    uint16_t eventId = 0xFFFF;
    uint32_t nextDataTransferHandle = 0;
    uint8_t transferFlag = 0;
    uint8_t eventClass = 0;
    uint32_t eventDataSize = 32;
    std::vector<uint8_t> eventData(eventDataSize);
    uint32_t eventDataIntegrityChecksum = 0;

    std::vector<uint8_t> shortResp{0x00, PLDM_PLATFORM,
                                   PLDM_POLL_FOR_PLATFORM_EVENT_MESSAGE,
                                   PLDM_SUCCESS};
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(shortResp));
    auto decodeRc = stdexec::sync_wait(eventManager.pollForPlatformEventMessage(
        tid, 0x01, PLDM_GET_FIRSTPART, 0, 0, completionCode, eventTid, eventId,
        nextDataTransferHandle, transferFlag, eventClass, eventDataSize,
        eventData, eventDataIntegrityChecksum));
    ASSERT_TRUE(decodeRc.has_value());
    EXPECT_NE(PLDM_SUCCESS, std::get<0>(*decodeRc));

    auto ccResp = makePollForPlatformEventResponse(
        tid, 0x0000, 0, 0, std::vector<uint8_t>{}, 0, PLDM_ERROR);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(ccResp));
    auto ccRc = stdexec::sync_wait(eventManager.pollForPlatformEventMessage(
        tid, 0x01, PLDM_GET_FIRSTPART, 0, 0, completionCode, eventTid, eventId,
        nextDataTransferHandle, transferFlag, eventClass, eventDataSize,
        eventData, eventDataIntegrityChecksum));
    ASSERT_TRUE(ccRc.has_value());
    EXPECT_NE(PLDM_SUCCESS, std::get<0>(*ccRc));
    EXPECT_EQ(PLDM_ERROR, completionCode);
}

TEST_F(EventManagerPollingTest, pollForPlatformEventTaskReturnCodeCoverage)
{
    constexpr pldm::tid_t tid = 0x24;
    std::string uuid("00000000-0000-0000-0000-000000000124");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);

    pldm::MctpInfo mctpInfo(
        24, "f72d6f90-5675-11ed-9b6a-0242ac120124",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    ASSERT_TRUE(terminusManager.mapTid(mctpInfo, tid).has_value());

    std::vector<uint8_t> shortResp{0x00, PLDM_PLATFORM,
                                   PLDM_POLL_FOR_PLATFORM_EVENT_MESSAGE,
                                   PLDM_SUCCESS};
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(shortResp));
    auto rcError =
        stdexec::sync_wait(eventManager.pollForPlatformEventTask(tid, 64));
    ASSERT_TRUE(rcError.has_value());
    EXPECT_NE(PLDM_SUCCESS, std::get<0>(*rcError));

    auto ccResp = makePollForPlatformEventResponse(
        tid, 0x0001, PLDM_PLATFORM_TRANSFER_START_AND_END, PLDM_SENSOR_EVENT,
        std::vector<uint8_t>{0x00}, 0, PLDM_ERROR);
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(ccResp));
    auto ccError =
        stdexec::sync_wait(eventManager.pollForPlatformEventTask(tid, 64));
    ASSERT_TRUE(ccError.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*ccError));
}

TEST_F(EventManagerPollingTest,
       pollForPlatformEventTaskTransferFlagVariantsCoverage)
{
    constexpr pldm::tid_t tid = 0x25;
    std::string uuid("00000000-0000-0000-0000-000000000125");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);

    pldm::MctpInfo mctpInfo(
        25, "f72d6f90-5675-11ed-9b6a-0242ac120125",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    ASSERT_TRUE(terminusManager.mapTid(mctpInfo, tid).has_value());

    const std::vector<uint8_t> middleChunk{0x44, 0x55};
    const std::vector<uint8_t> endChunk{0x66};
    std::vector<uint8_t> fullEvent = middleChunk;
    fullEvent.insert(fullEvent.end(), endChunk.begin(), endChunk.end());
    const uint32_t checksum =
        pldm_edac_crc32(fullEvent.data(), fullEvent.size());

    auto middle = makePollForPlatformEventResponse(
        tid, 3, PLDM_PLATFORM_TRANSFER_MIDDLE, 0xEE, middleChunk);
    auto end = makePollForPlatformEventResponse(
        tid, 3, PLDM_PLATFORM_TRANSFER_END, 0xEE, endChunk, checksum);
    auto ackDone = makePollForPlatformEventResponse(tid, 0x0000, 0, 0,
                                                    std::vector<uint8_t>{});

    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(middle));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(end));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(ackDone));

    auto middleRc =
        stdexec::sync_wait(eventManager.pollForPlatformEventTask(tid, 64));
    ASSERT_TRUE(middleRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*middleRc));

    const std::vector<uint8_t> legacyChunk{0x77, 0x88};
    const std::vector<uint8_t> legacyEndChunk{0x99};
    std::vector<uint8_t> legacyFullEvent = legacyChunk;
    legacyFullEvent.insert(legacyFullEvent.end(), legacyEndChunk.begin(),
                           legacyEndChunk.end());
    const uint32_t legacyChecksum =
        pldm_edac_crc32(legacyFullEvent.data(), legacyFullEvent.size());

    auto legacyMiddle = makePollForPlatformEventResponse(tid, 4, PLDM_MIDDLE,
                                                         0xEE, legacyChunk);
    auto legacyEnd =
        makePollForPlatformEventResponse(tid, 4, PLDM_PLATFORM_TRANSFER_END,
                                         0xEE, legacyEndChunk, legacyChecksum);
    auto legacyAckDone = makePollForPlatformEventResponse(
        tid, 0x0000, 0, 0, std::vector<uint8_t>{});

    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(legacyMiddle));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(legacyEnd));
    ASSERT_EQ(PLDM_SUCCESS, terminusManager.enqueueResponse(legacyAckDone));

    auto legacyRc =
        stdexec::sync_wait(eventManager.pollForPlatformEventTask(tid, 64));
    ASSERT_TRUE(legacyRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*legacyRc));
}

TEST_F(EventManagerProtectedTest, inventoryAndVerbosePollCoverage)
{
    uint8_t platformEventStatus = 0;
    uint8_t dummyData = 0;

    EventManagerCoverage verboseEventManager(
        terminusManager, termini, fwUpdateManager, platformManager,
        sensorManager, true);
    EXPECT_EQ(PLDM_SUCCESS, verboseEventManager.handlePlatformEvent(
                                0x44, PLDM_MESSAGE_POLL_EVENT, &dummyData, 0,
                                platformEventStatus));

    std::array<uint8_t, 3> shortInventoryData{0x01, 0x00, 0x00};
    EXPECT_EQ(PLDM_ERROR,
              eventManager.handlePlatformEvent(
                  0x45, PLDM_OEM_EVENT_CLASS_0xF3, shortInventoryData.data(),
                  shortInventoryData.size(), platformEventStatus));

    constexpr pldm::tid_t tid = 0x46;
    std::string uuid("00000000-0000-0000-0000-000000000146");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);

    EXPECT_EQ(PLDM_ERROR,
              eventManager.handlePlatformEvent(
                  tid, PLDM_OEM_EVENT_CLASS_0xF3, shortInventoryData.data(),
                  shortInventoryData.size(), platformEventStatus));

    termini[tid]->setTerminusName("ProcessorModule_46");
    EXPECT_EQ(PLDM_ERROR,
              eventManager.handlePlatformEvent(
                  tid, PLDM_OEM_EVENT_CLASS_0xF3, shortInventoryData.data(),
                  shortInventoryData.size(), platformEventStatus));
}

TEST_F(EventManagerProtectedTest, oemFallbackAndUnhandledCoverage)
{
    uint8_t platformEventStatus = 0;
    std::array<uint8_t, 2> tooSmallData{0x01, 0x00};

    EXPECT_EQ(PLDM_ERROR,
              eventManager.handlePlatformEvent(
                  0x70, PLDM_OEM_EVENT_CLASS_ERROR_COUNTER, tooSmallData.data(),
                  tooSmallData.size(), platformEventStatus));
    EXPECT_EQ(PLDM_EVENT_LOGGING_REJECTED, platformEventStatus);

    EXPECT_EQ(PLDM_ERROR, eventManager.handlePlatformEvent(
                              0x71, PLDM_OEM_EVENT_CLASS_PCIE_TELEMETRY,
                              tooSmallData.data(), tooSmallData.size(),
                              platformEventStatus));
    EXPECT_EQ(PLDM_EVENT_LOGGING_REJECTED, platformEventStatus);

    EXPECT_EQ(PLDM_ERROR,
              eventManager.handlePlatformEvent(
                  0x72, PLDM_OEM_EVENT_CLASS_PCIE_LTSSM, tooSmallData.data(),
                  tooSmallData.size(), platformEventStatus));
    EXPECT_EQ(PLDM_EVENT_LOGGING_REJECTED, platformEventStatus);

    std::array<uint8_t, 4> cperData{0x01, 0x00, 0x00, 0x00};
    EXPECT_EQ(PLDM_SUCCESS,
              eventManager.handlePlatformEvent(0x73, PLDM_OEM_EVENT_CLASS_0xFA,
                                               cperData.data(), cperData.size(),
                                               platformEventStatus));

    uint8_t dummy = 0;
    platformEventStatus = 0;
    EXPECT_EQ(PLDM_SUCCESS,
              eventManager.handlePlatformEvent(
                  0x74, 0xEE, &dummy, sizeof(dummy), platformEventStatus));
    EXPECT_EQ(PLDM_EVENT_LOGGING_REJECTED, platformEventStatus);
}

TEST_F(EventManagerProtectedTest, oemTerminusNameFallbackCoverage)
{
    uint8_t platformEventStatus = 0;
    const std::array<uint8_t, 5> validOemPayload{
        {0x02, 0x01, 0x01, 0x00, 0x5A}};
    const std::array<uint8_t, 6> validInventoryPayload{
        {0x02, 0x01, 0x02, 0x00, '{', '}'}};

    constexpr pldm::tid_t unnamedTid = 0x75;
    std::string unnamedUuid("00000000-0000-0000-0000-000000000175");
    termini[unnamedTid] = std::make_shared<Terminus>(
        unnamedTid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, unnamedUuid,
        terminusManager);

    EXPECT_EQ(PLDM_ERROR, eventManager.handlePlatformEvent(
                              unnamedTid, PLDM_OEM_EVENT_CLASS_ERROR_COUNTER,
                              validOemPayload.data(), validOemPayload.size(),
                              platformEventStatus));
    EXPECT_EQ(PLDM_EVENT_LOGGING_REJECTED, platformEventStatus);

    EXPECT_EQ(PLDM_ERROR, eventManager.handlePlatformEvent(
                              unnamedTid, PLDM_OEM_EVENT_CLASS_PCIE_TELEMETRY,
                              validOemPayload.data(), validOemPayload.size(),
                              platformEventStatus));
    EXPECT_EQ(PLDM_EVENT_LOGGING_REJECTED, platformEventStatus);

    EXPECT_EQ(PLDM_ERROR, eventManager.handlePlatformEvent(
                              unnamedTid, PLDM_OEM_EVENT_CLASS_PCIE_LTSSM,
                              validOemPayload.data(), validOemPayload.size(),
                              platformEventStatus));
    EXPECT_EQ(PLDM_EVENT_LOGGING_REJECTED, platformEventStatus);

    EXPECT_EQ(PLDM_ERROR,
              eventManager.handlePlatformEvent(
                  unnamedTid, PLDM_OEM_EVENT_CLASS_0xF3,
                  validInventoryPayload.data(), validInventoryPayload.size(),
                  platformEventStatus));
    EXPECT_EQ(PLDM_EVENT_LOGGING_REJECTED, platformEventStatus);

    constexpr pldm::tid_t nullTerminusTid = 0x76;
    termini[nullTerminusTid] = nullptr;
    EXPECT_EQ(PLDM_ERROR,
              eventManager.handlePlatformEvent(
                  nullTerminusTid, PLDM_OEM_EVENT_CLASS_ERROR_COUNTER,
                  validOemPayload.data(), validOemPayload.size(),
                  platformEventStatus));
    EXPECT_EQ(PLDM_EVENT_LOGGING_REJECTED, platformEventStatus);
}

TEST_F(EventManagerProtectedTest, thresholdLogSeverityCoverage)
{
    const std::array<std::string, 8> messageIds{
        SensorThresholdWarningLowGoingHigh,
        SensorThresholdWarningHighGoingLow,
        SensorThresholdWarningLowGoingLow,
        SensorThresholdWarningHighGoingHigh,
        SensorThresholdCriticalLowGoingHigh,
        SensorThresholdCriticalHighGoingLow,
        SensorThresholdCriticalLowGoingLow,
        SensorThresholdCriticalHighGoingHigh,
    };

    for (const auto& messageId : messageIds)
    {
        eventManager.createSensorThresholdLogEntry(
            messageId, "SensorCov", 42.0, 43.0, "OpenBMC.0.2.Coverage",
            "DeviceCov");
    }

    eventManager.createSensorThresholdLogEntry("OpenBMC.0.2.InvalidCoverage",
                                               "SensorCov", 1.0, 2.0, "", "");
}

TEST_F(EventManagerDbusMockTest, thresholdLogCreateSuccessCoverage)
{
    constexpr auto* logObjPath = "/xyz/openbmc_project/logging";
    constexpr auto* logInterface = "xyz.openbmc_project.Logging.Create";
    constexpr auto* logService = "xyz.openbmc_project.Logging";

    testing::InSequence seq;
    expectGetObjectCall(logObjPath, logInterface, logService);
    expectNewMethodCall(logService, logObjPath, logInterface, "Create");
    expectAppendString(SensorThresholdCriticalHighGoingHigh.c_str());
    expectAppendString("xyz.openbmc_project.Logging.Entry.Level.Critical");
    expectStringMap({
        {"REDFISH_MESSAGE_ARGS", "SensorCritical,52.000000,45.000000"},
        {"REDFISH_MESSAGE_ID", SensorThresholdCriticalHighGoingHigh},
    });
    expectBusCallNoReply();

    eventManager.createSensorThresholdLogEntry(
        SensorThresholdCriticalHighGoingHigh, "SensorCritical", 52.0, 45.0, "",
        "");
}

TEST_F(EventManagerDbusMockTest, thresholdLogCreateWithOemArgsCoverage)
{
    constexpr auto* logObjPath = "/xyz/openbmc_project/logging";
    constexpr auto* logInterface = "xyz.openbmc_project.Logging.Create";
    constexpr auto* logService = "xyz.openbmc_project.Logging";

    testing::InSequence seq;
    expectGetObjectCall(logObjPath, logInterface, logService);
    expectNewMethodCall(logService, logObjPath, logInterface, "Create");
    expectAppendString(SensorThresholdWarningHighGoingHigh.c_str());
    expectAppendString("xyz.openbmc_project.Logging.Entry.Level.Warning");
    expectStringMap({
        {"DEVICE_NAME", "CPU0"},
        {"REDFISH_MESSAGE_ARGS", "SensorWarning,50.500000,40.000000"},
        {"REDFISH_MESSAGE_ID", SensorThresholdWarningHighGoingHigh},
        {"xyz.openbmc_project.Logging.Entry.EventId", "OpenBMC.0.2.Warning"},
    });
    expectBusCallNoReply();

    eventManager.createSensorThresholdLogEntry(
        SensorThresholdWarningHighGoingHigh, "SensorWarning", 50.5, 40.0,
        "OpenBMC.0.2.Warning", "CPU0");
}

TEST_F(EventManagerDbusMockTest, thresholdLogCreateGetServiceFailureCoverage)
{
    constexpr auto* logObjPath = "/xyz/openbmc_project/logging";
    constexpr auto* logInterface = "xyz.openbmc_project.Logging.Create";

    testing::InSequence seq;
    expectNewMethodCall(pldm::utils::ObjectMapper::default_service,
                        pldm::utils::ObjectMapper::instance_path,
                        pldm::utils::ObjectMapper::interface, "GetObject");
    expectAppendString(logObjPath);
    expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
    expectAppendString(logInterface);
    expectCloseContainer();
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, testing::_, testing::_,
                                  testing::_))
        .WillOnce(testing::Return(-ENOENT));

    EXPECT_NO_THROW(eventManager.createSensorThresholdLogEntry(
        SensorThresholdWarningLowGoingHigh, "SensorGetServiceFail", 10.0, 9.0,
        "", ""));
}

TEST_F(EventManagerDbusMockTest, thresholdLogCreateBusCallFailureCoverage)
{
    constexpr auto* logObjPath = "/xyz/openbmc_project/logging";
    constexpr auto* logInterface = "xyz.openbmc_project.Logging.Create";
    constexpr auto* logService = "xyz.openbmc_project.Logging";

    testing::InSequence seq;
    expectGetObjectCall(logObjPath, logInterface, logService);
    expectNewMethodCall(logService, logObjPath, logInterface, "Create");
    expectAppendString(SensorThresholdWarningLowGoingHigh.c_str());
    expectAppendString("xyz.openbmc_project.Logging.Entry.Level.Informational");
    expectStringMap({
        {"REDFISH_MESSAGE_ARGS", "SensorBusFail,10.000000,9.000000"},
        {"REDFISH_MESSAGE_ID", SensorThresholdWarningLowGoingHigh},
    });
    EXPECT_CALL(mock,
                sd_bus_call(nullptr, nullptr, testing::_, testing::_, nullptr))
        .WillOnce(testing::Return(-EINVAL));

    EXPECT_NO_THROW(eventManager.createSensorThresholdLogEntry(
        SensorThresholdWarningLowGoingHigh, "SensorBusFail", 10.0, 9.0, "",
        ""));
}

TEST_F(EventManagerTest, processNumericSensorEventOemInfoForwardingCoverage)
{
    constexpr pldm::tid_t tid = 0x96;
    constexpr uint16_t sensorId = 0x496;
    std::string uuid("00000000-0000-0000-0000-000000000496");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);

    auto sensorEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    sensorEventInfo->impactedComponent = "CPU96";
    sensorEventInfo->eventIdsMap["PLDM_SENSOR_UPPERCRITICAL"] = "EID96";

    constexpr auto sensorName = "oem_forward_sensor";
    auto sensor = makeEventNumericSensor(
        tid, sensorId, sensorName,
        "/xyz/openbmc_project/inventory/system/chassis/chassis96",
        sensorEventInfo);
    termini[tid]->numericSensors.emplace_back(sensor);

    auto sensorData = makeNumericSensorEventStateData(
        PLDM_SENSOR_UPPERCRITICAL, PLDM_SENSOR_UPPERWARNING,
        PLDM_SENSOR_DATA_SIZE_UINT16, 50);

    EXPECT_CALL(eventManager, createSensorThresholdLogEntry(
                                  SensorThresholdCriticalHighGoingHigh,
                                  sensorName, testing::DoubleEq(50.0),
                                  testing::DoubleEq(60.0), "EID96", "CPU96"));
    eventManager.processNumericSensorEvent(tid, sensorId, sensorData.data(),
                                           sensorData.size());
}

TEST_F(EventManagerTest,
       processNumericSensorEventNullSensorEventInfoForwardingCoverage)
{
    constexpr pldm::tid_t tid = 0x97;
    constexpr uint16_t sensorId = 0x497;
    std::string uuid("00000000-0000-0000-0000-000000000497");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);

    constexpr auto sensorName = "null_event_info_sensor";
    auto sensor = makeEventNumericSensor(
        tid, sensorId, sensorName,
        "/xyz/openbmc_project/inventory/system/chassis/chassis97");
    termini[tid]->numericSensors.emplace_back(sensor);

    auto sensorData = makeNumericSensorEventStateData(
        PLDM_SENSOR_NORMAL, PLDM_SENSOR_LOWERWARNING,
        PLDM_SENSOR_DATA_SIZE_UINT8, 18);

    EXPECT_CALL(eventManager,
                createSensorThresholdLogEntry(
                    SensorThresholdWarningLowGoingHigh, sensorName,
                    testing::DoubleEq(18.0), testing::DoubleEq(20.0), "", ""));
    eventManager.processNumericSensorEvent(tid, sensorId, sensorData.data(),
                                           sensorData.size());
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
    (void)pldm::oem_events::handlePcieLtssmEvent(
        "ProcessorModule_0", sizeMatchedData.data(), sizeMatchedData.size());
}
