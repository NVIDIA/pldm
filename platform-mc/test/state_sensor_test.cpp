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
#include "libpldm/oem/nvidia/state_set_oem_nvidia.h"

#include "../../test/test_valgrind_utils.hpp"
#include "common/instance_id.hpp"
#include "oem/nvidia/platform-mc/remoteDebug.hpp"
#include "oem/nvidia/platform-mc/state_set/memoryPerformance.hpp"
#include "oem/nvidia/platform-mc/state_set/memorySpareChannel.hpp"
#include "oem/nvidia/platform-mc/state_set/nvlink.hpp"
#include "oem/nvidia/platform-mc/state_set/processorPowerBreak.hpp"
#include "platform-mc/numeric_sensor.hpp"
#include "platform-mc/state_set.hpp"
#include "platform-mc/state_set/ethIBPortLinkState.hpp"
#include "platform-mc/state_set/healthState.hpp"
#include "platform-mc/state_set/pciePortLinkState.hpp"
#include "platform-mc/state_set/performance.hpp"
#include "platform-mc/state_set/powerSupplyInput.hpp"
#include "platform-mc/terminus.hpp"
#include "platform-mc/terminus_manager.hpp"
#include "test/test_instance_id.hpp"

#include <sdbusplus/test/sdbus_mock.hpp>
#include <sdeventplus/event.hpp>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <optional>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace state_sensor_test_alloc
{

thread_local bool failAllocations = false;
thread_local std::size_t failAtAllocation = 0;
thread_local std::size_t allocationCount = 0;

bool shouldFailAllocation()
{
    return failAllocations && failAtAllocation != 0 &&
           ++allocationCount == failAtAllocation;
}

void* allocate(std::size_t size,
               std::size_t alignment = alignof(std::max_align_t))
{
    if (shouldFailAllocation())
    {
        throw std::bad_alloc();
    }

    if (size == 0)
    {
        size = 1;
    }

    void* ptr = nullptr;
    if (alignment <= alignof(std::max_align_t))
    {
        ptr = std::malloc(size);
    }
    else if (posix_memalign(&ptr, alignment, size) != 0)
    {
        ptr = nullptr;
    }

    if (ptr == nullptr)
    {
        throw std::bad_alloc();
    }

    return ptr;
}

struct ScopedAllocationFailure
{
    explicit ScopedAllocationFailure(std::size_t failIndex) :
        previousFailAllocations(failAllocations),
        previousFailAtAllocation(failAtAllocation),
        previousAllocationCount(allocationCount)
    {
        failAllocations = true;
        failAtAllocation = failIndex;
        allocationCount = 0;
    }

    ~ScopedAllocationFailure()
    {
        failAllocations = previousFailAllocations;
        failAtAllocation = previousFailAtAllocation;
        allocationCount = previousAllocationCount;
    }

  private:
    bool previousFailAllocations;
    std::size_t previousFailAtAllocation;
    std::size_t previousAllocationCount;
};

template <typename Operation>
bool exerciseBadAlloc(Operation&& operation, std::size_t maxFailAt = 128)
{
    if (pldm::test::runningOnValgrind())
    {
        return true;
    }

    for (std::size_t failIndex = 1; failIndex <= maxFailAt; ++failIndex)
    {
        try
        {
            ScopedAllocationFailure failure(failIndex);
            operation();
        }
        catch (const std::bad_alloc&)
        {
            return true;
        }
    }

    return false;
}

template <typename Operation>
bool exerciseAllBadAlloc(Operation&& operation, std::size_t maxFailAt = 256)
{
    if (pldm::test::runningOnValgrind())
    {
        return true;
    }

    bool sawBadAlloc = false;

    for (std::size_t failIndex = 1; failIndex <= maxFailAt; ++failIndex)
    {
        try
        {
            ScopedAllocationFailure failure(failIndex);
            operation();
        }
        catch (const std::bad_alloc&)
        {
            sawBadAlloc = true;
        }
        catch (...)
        {}
    }

    return sawBadAlloc;
}

} // namespace state_sensor_test_alloc

void* operator new(std::size_t size)
{
    return state_sensor_test_alloc::allocate(size);
}

void* operator new[](std::size_t size)
{
    return state_sensor_test_alloc::allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    return state_sensor_test_alloc::allocate(
        size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return state_sensor_test_alloc::allocate(
        size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return state_sensor_test_alloc::allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return state_sensor_test_alloc::allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void operator delete(void* ptr) noexcept
{
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept
{
    std::free(ptr);
}

void operator delete(void* ptr, std::align_val_t) noexcept
{
    std::free(ptr);
}

void operator delete[](void* ptr, std::align_val_t) noexcept
{
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept
{
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept
{
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept
{
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept
{
    std::free(ptr);
}

using namespace pldm::platform_mc;
using namespace pldm;
using namespace std::chrono;

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

class StateSensorCoverage : public ::testing::Test
{
  protected:
    StateSensorCoverage() :
        bus(pldm::utils::DBusHandler::getBus()),
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, instanceIdDb, termini, 0x8, nullptr)
    {}

    sdbusplus::bus_t& bus;
    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    requester::Handler<requester::Request> reqHandler;
    TerminusManager terminusManager;
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
};

class StateSensorDbusMockTest : public StateSensorCoverage
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
        termini.clear();

        if (busSwapped)
        {
            pldm::utils::DBusHandler::getBus() = std::move(*savedBus);
            savedBus.reset();
            busSwapped = false;
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

    void expectNewMethodCallFailure(const char* service, const char* path,
                                    const char* interface, const char* method)
    {
        EXPECT_CALL(mock, sd_bus_message_new_method_call(
                              testing::_, testing::_, testing::StrEq(service),
                              testing::StrEq(path), testing::StrEq(interface),
                              testing::StrEq(method)))
            .WillOnce(testing::Return(-1));
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
        // GMock allocates matcher state here and the analyzer reports a
        // false leak through EXPECT_CALL matcher construction.
        // NOLINTNEXTLINE(clang-analyzer-unix.Malloc)
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

    void expectReadEmptyServiceMap()
    {
        EXPECT_CALL(mock, sd_bus_message_enter_container(
                              nullptr, SD_BUS_TYPE_ARRAY, testing::_))
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

    void expectGetObjectCallWithEmptyReply(const char* objectPath,
                                           const char* interface)
    {
        expectNewMethodCall(pldm::utils::ObjectMapper::default_service,
                            pldm::utils::ObjectMapper::instance_path,
                            pldm::utils::ObjectMapper::interface, "GetObject");
        expectAppendString(objectPath);
        expectOpenContainer(SD_BUS_TYPE_ARRAY, "s");
        expectAppendString(interface);
        expectCloseContainer();
        expectBusCallWithReply();
        expectReadEmptyServiceMap();
    }

    void expectStringMap(
        const std::vector<std::pair<std::string, std::string>>& entries)
    {
        // GMock allocates matcher state here and the analyzer reports a
        // false leak through EXPECT_CALL matcher construction.
        // NOLINTNEXTLINE(clang-analyzer-unix.Malloc)
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

    testing::NiceMock<sdbusplus::SdBusMock> mock;
    std::optional<sdbusplus::bus_t> savedBus;
    bool busSwapped = false;
};

class TrackingStateSet : public StateSet
{
  public:
    using EventTuple =
        std::tuple<std::string, std::string, Level, std::string, std::string>;

    TrackingStateSet(std::string stateType, EventTuple eventData,
                     uint16_t stateSetId = PLDM_STATESET_ID_HEALTHSTATE) :
        StateSet(stateSetId), stateType(std::move(stateType)),
        eventData(std::move(eventData))
    {}

    void setValue(uint8_t value) override
    {
        lastValue = value;
    }

    void setDefaultValue() override
    {
        defaultValueCalls++;
    }

    void setAssociation(
        std::vector<pldm::dbus::PathAssociation>& stateAssociations) override
    {
        lastAssociations = stateAssociations;
    }

    std::string getStringStateType() const override
    {
        return stateType;
    }

    EventTuple getEventData(
        utils::SensorEventInfo* sensorEventInfo) const override
    {
        lastSensorEventInfo = sensorEventInfo;
        return eventData;
    }

    void updateSensorName(std::string name) override
    {
        updatedNames.emplace_back(std::move(name));
    }

    mutable const utils::SensorEventInfo* lastSensorEventInfo = nullptr;
    std::vector<pldm::dbus::PathAssociation> lastAssociations{};
    std::vector<std::string> updatedNames{};
    uint8_t lastValue = 0;
    size_t defaultValueCalls = 0;

  private:
    std::string stateType;
    EventTuple eventData;
};

class AssociatingTrackingStateSet : public TrackingStateSet
{
  public:
    using TrackingStateSet::TrackingStateSet;

    void associateNumericSensor(
        const EntityInfo& entityInfo,
        std::vector<std::shared_ptr<NumericSensor>>& numericSensors) override
    {
        lastEntityInfo = entityInfo;
        associatedSensorCount = numericSensors.size();
        associateCalls++;
    }

    EntityInfo lastEntityInfo{};
    size_t associatedSensorCount = 0;
    size_t associateCalls = 0;
};

std::unique_ptr<StateSensor> makeTrackingStateSensor(
    uint8_t tid, uint16_t sensorId,
    std::shared_ptr<utils::SensorEventInfo> sensorEventInfo = nullptr)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_state_sensor"};

    return std::make_unique<StateSensor>(
        tid, false, sensorId, std::move(info), nullptr, associationPath,
        std::move(sensorEventInfo));
}

TEST(TestOemStateSensor, memorySpareChannelPresence)
{
    uint16_t sensorId = 1;
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    sdeventplus::Event event(sdeventplus::Event::get_default());
    TestInstanceIdDb instanceIdDb;
    requester::Handler<requester::Request> reqHandler(
        nullptr, event, instanceIdDb, false, seconds(1), 2, milliseconds(100));
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
    TerminusManager terminusManager(event, reqHandler, instanceIdDb, termini,
                                    0x8, nullptr);
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                   // record handle
        0x1,                   // PDRHeaderVersion
        PLDM_STATE_SENSOR_PDR, // PDRType
        0x0,
        0x0,                   // recordChangeNumber
        0x0,
        0x11,                  // dataLength
        0,
        0,                     // PLDMTerminusHandle
        static_cast<uint8_t>(sensorId & 0xFF),
        static_cast<uint8_t>((sensorId >> 8) & 0xFF),
        PLDM_ENTITY_MEMORY_CONTROLLER,
        0,            // entityType=Memory controller (143)
        1,
        0,            // entityInstanceNumber
        0x1,
        0x0,          // containerID=1
        PLDM_NO_INIT, // sensorInit
        false,        // sensorAuxiliaryNamesPDR
        1,            // compositeSensorCount
        static_cast<uint8_t>(PLDM_STATESET_ID_PRESENCE & 0xFF),
        static_cast<uint8_t>(
            (PLDM_STATESET_ID_PRESENCE >> 8) & 0xFF), // stateSetID (13)
        0x1,                                          // possibleStatesSize
        0x3                                           // possibleStates
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    auto stateSensorPdr = t1.stateSensorPdrs[0];
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.stateSensorPdrs.size());

    auto stateSensor = t1.stateSensors[0];
    EXPECT_EQ(sensorId, stateSensor->sensorId);
    EXPECT_EQ(1, stateSensor->stateSets.size());

    auto stateSetMemorySpareChannel =
        dynamic_pointer_cast<StateSetMemorySpareChannel>(
            stateSensor->stateSets[0]);

    // Should be true for PLDM_STATESET_PRESENCE_PRESENT
    stateSensor->updateReading(true, true, 0, PLDM_STATESET_PRESENCE_PRESENT);
    EXPECT_EQ(
        sdbusplus::common::com::nvidia::MemorySpareChannel::Presence::Present,
        stateSetMemorySpareChannel->ValueIntf->memorySpareChannelPresence());

    // Should be false for PLDM_STATESET_PRESENCE_NOT_PRESENT
    stateSensor->updateReading(true, true, 0,
                               PLDM_STATESET_PRESENCE_NOT_PRESENT);
    EXPECT_EQ(
        sdbusplus::common::com::nvidia::MemorySpareChannel::Presence::
            NotPresent,
        stateSetMemorySpareChannel->ValueIntf->memorySpareChannelPresence());

    // Should be false for invalid state set
    stateSensor->updateReading(true, true, 0, 0);
    EXPECT_EQ(
        sdbusplus::common::com::nvidia::MemorySpareChannel::Presence::
            Unavailable,
        stateSetMemorySpareChannel->ValueIntf->memorySpareChannelPresence());
}

#ifdef OEM_NVIDIA
TEST(StateSensorEventId, synthesizeEventId)
{
    EXPECT_EQ(
        "CPU_0_PERFORMANCE_THROTTLED",
        StateSensor::synthesizeEventId("CPU_0", "Performance", "Throttled"));
    EXPECT_EQ("HGX_CPU_0_EDP_VIOLATION_STATE_CURRENT_INPUT_OUT_OF_RANGE",
              StateSensor::synthesizeEventId("HGX_CPU_0", "EDP Violation State",
                                             "Current Input out of Range"));
    EXPECT_EQ("DEV_1_PCIE_LINKDOWN",
              StateSensor::synthesizeEventId("Dev-1", "PCIe", "LinkDown"));
}
#endif

TEST_F(StateSensorCoverage, healthStateCoverage)
{
    const uint16_t sensorId = 2;
    std::string uuid("00000000-0000-0000-0000-000000000002");
    auto terminus =
        Terminus(2, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    auto pdr = makeStateSensorPdr(sensorId, PLDM_ENTITY_SYS_BOARD, 1,
                                  PLDM_STATESET_ID_HEALTHSTATE, 0x3F);
    terminus.pdrs.emplace_back(pdr);
    ASSERT_TRUE(terminus.parsePDRs());
    ASSERT_EQ(1u, terminus.stateSensors.size());

    auto stateSensor = terminus.stateSensors[0];
    ASSERT_EQ(1u, stateSensor->stateSets.size());
    auto health = std::dynamic_pointer_cast<StateSetHealthState>(
        stateSensor->stateSets[0]);
    ASSERT_NE(nullptr, health);

    std::vector<std::string> inventoryPaths{
        "/xyz/openbmc_project/inventory/system/chassis/chassis0"};
    stateSensor->setInventoryPaths(inventoryPaths, false);
    std::vector<std::shared_ptr<NumericSensor>> numericSensors{};
    stateSensor->associateNumericSensor(numericSensors);

    stateSensor->updateReading(true, true, 0,
                               PLDM_STATESET_HEALTH_STATE_NORMAL);
    auto [msgOk, argOk, levelOk, eventOk,
          impactedOk] = health->getEventData(nullptr);
    EXPECT_EQ("OK", argOk);
    EXPECT_TRUE(eventOk.empty());
    EXPECT_TRUE(impactedOk.empty());

    stateSensor->updateReading(true, true, 0,
                               PLDM_STATESET_HEALTH_STATE_NON_CRITICAL);
    auto [msgWarn, argWarn, levelWarn, eventWarn,
          impactedWarn] = health->getEventData(nullptr);
    EXPECT_EQ("Warning", argWarn);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedWarning", msgWarn);
    EXPECT_EQ(Level::Warning, levelWarn);
    EXPECT_TRUE(eventWarn.empty());
    EXPECT_TRUE(impactedWarn.empty());

    stateSensor->handleSensorEvent(0, PLDM_STATESET_HEALTH_STATE_NON_CRITICAL,
                                   0);
    stateSensor->handleSensorEvent(0, PLDM_STATESET_HEALTH_STATE_CRITICAL, 1);
    stateSensor->handleSensorEvent(4, PLDM_STATESET_HEALTH_STATE_CRITICAL, 1);
    stateSensor->handleErrGetSensorReading();

    EXPECT_EQ(0, health->getValue());
    health->setOpState(EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING);
    EXPECT_EQ(EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING, health->getOpState());

    pldm::platform_mc::AuxiliaryNames auxNames{{{"en", "Id_0"}}};
    stateSensor->updateSensorNames(auxNames);
    auxNames[0][0].second = "Health-Renamed";
    stateSensor->updateSensorNames(auxNames);
    EXPECT_EQ("Health", health->getStringStateType());
}

TEST_F(StateSensorCoverage, stateSetCoverageMatrix)
{
    std::vector<std::shared_ptr<NumericSensor>> emptyNumericSensors{};
    std::vector<std::string> inventoryPaths{
        "/xyz/openbmc_project/inventory/system/chassis/chassis1"};

    {
        std::string uuid("00000000-0000-0000-0000-000000000003");
        auto terminus = Terminus(3, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                                 terminusManager);
        auto pdr = makeStateSensorPdr(3, PLDM_ENTITY_PROC, 1,
                                      PLDM_STATESET_ID_PERFORMANCE, 0x3);
        terminus.pdrs.emplace_back(pdr);
        ASSERT_TRUE(terminus.parsePDRs());
        auto sensor = terminus.stateSensors[0];
        auto stateSet = std::dynamic_pointer_cast<StateSetPerformance>(
            sensor->stateSets[0]);
        ASSERT_NE(nullptr, stateSet);
        sensor->setInventoryPaths(inventoryPaths, false);
        sensor->associateNumericSensor(emptyNumericSensors);
        sensor->updateReading(true, true, 0, PLDM_STATESET_PERFORMANCE_NORMAL);
        auto [msgOk, argOk, levelOk, eventOk,
              impactedOk] = stateSet->getEventData(nullptr);
        EXPECT_EQ("Normal", argOk);
        sensor->updateReading(true, true, 0,
                              PLDM_STATESET_PERFORMANCE_THROTTLED);
        auto [msgWarn, argWarn, levelWarn, eventWarn,
              impactedWarn] = stateSet->getEventData(nullptr);
        EXPECT_EQ("Throttled", argWarn);
        EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedWarning", msgWarn);
        EXPECT_EQ(Level::Warning, levelWarn);
        sensor->updateReading(true, true, 0, 0xFF);
        stateSet->updateSensorName("unused");
        EXPECT_EQ("Performance", stateSet->getStringStateType());
    }

    {
        std::string uuid("00000000-0000-0000-0000-000000000004");
        auto terminus = Terminus(4, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                                 terminusManager);
        auto pdr = makeStateSensorPdr(4, PLDM_ENTITY_POWER_SUPPLY, 1,
                                      PLDM_STATESET_ID_POWERSUPPLY, 0x3);
        terminus.pdrs.emplace_back(pdr);
        ASSERT_TRUE(terminus.parsePDRs());
        auto sensor = terminus.stateSensors[0];
        auto stateSet = std::dynamic_pointer_cast<StateSetPowerSupplyInput>(
            sensor->stateSets[0]);
        ASSERT_NE(nullptr, stateSet);
        sensor->setInventoryPaths(inventoryPaths, false);
        sensor->associateNumericSensor(emptyNumericSensors);
        sensor->updateReading(true, true, 0, PLDM_STATESET_POWERSUPPLY_NORMAL);
        auto [msgOk, argOk, levelOk, eventOk,
              impactedOk] = stateSet->getEventData(nullptr);
        EXPECT_EQ("Normal", argOk);
        sensor->updateReading(true, true, 0,
                              PLDM_STATESET_POWERSUPPLY_OUTOFRANGE);
        auto [msgWarn, argWarn, levelWarn, eventWarn,
              impactedWarn] = stateSet->getEventData(nullptr);
        EXPECT_EQ("Current Input out of Range", argWarn);
        EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedWarning", msgWarn);
        EXPECT_EQ(Level::Warning, levelWarn);
        sensor->updateReading(true, true, 0, 0xFF);
        stateSet->updateSensorName("unused");
        EXPECT_EQ("EDP Violation State", stateSet->getStringStateType());
    }

    {
        std::string uuid("00000000-0000-0000-0000-000000000005");
        auto terminus = Terminus(5, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                                 terminusManager);
        auto pdr = makeStateSensorPdr(5, PLDM_ENTITY_PCI_EXPRESS_BUS, 1,
                                      PLDM_STATESET_ID_LINKSTATE, 0x3);
        terminus.pdrs.emplace_back(pdr);
        ASSERT_TRUE(terminus.parsePDRs());
        auto sensor = terminus.stateSensors[0];
        auto stateSet = std::dynamic_pointer_cast<StateSetPciePortLinkState>(
            sensor->stateSets[0]);
        ASSERT_NE(nullptr, stateSet);
        sensor->setInventoryPaths(inventoryPaths, false);
        sensor->associateNumericSensor(emptyNumericSensors);
        sensor->updateReading(true, true, 0,
                              PLDM_STATESET_LINK_STATE_CONNECTED);
        auto [msgUp, argUp, levelUp, eventUp,
              impactedUp] = stateSet->getEventData(nullptr);
        EXPECT_EQ("Active", argUp);
        sensor->updateReading(true, true, 0,
                              PLDM_STATESET_LINK_STATE_DISCONNECTED);
        auto [msgDown, argDown, levelDown, eventDown,
              impactedDown] = stateSet->getEventData(nullptr);
        EXPECT_EQ("Inactive", argDown);
        EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedOK", msgDown);
        EXPECT_EQ(Level::Informational, levelDown);
        sensor->updateReading(true, true, 0, 0xFF);
        auto [msgUnknown, argUnknown, levelUnknown, eventUnknown,
              impactedUnknown] = stateSet->getEventData(nullptr);
        EXPECT_EQ("Unknown", argUnknown);
        EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedWarning", msgUnknown);
        EXPECT_EQ(Level::Warning, levelUnknown);
        stateSet->updateSensorName("unused");
        EXPECT_EQ("PCIe", stateSet->getStringStateType());
    }

    {
        std::string uuid("00000000-0000-0000-0000-000000000006");
        auto terminus = Terminus(6, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                                 terminusManager);
        auto pdr = makeStateSensorPdr(6, PLDM_ENTITY_ETHERNET, 7,
                                      PLDM_STATESET_ID_LINKSTATE, 0x3);
        terminus.pdrs.emplace_back(pdr);
        ASSERT_TRUE(terminus.parsePDRs());
        auto sensor = terminus.stateSensors[0];
        auto stateSet = std::dynamic_pointer_cast<StateSetEthIBPortLinkState>(
            sensor->stateSets[0]);
        ASSERT_NE(nullptr, stateSet);
        sensor->setInventoryPaths(inventoryPaths, false);
        sensor->associateNumericSensor(emptyNumericSensors);

        sensor->updateReading(true, true, 0,
                              PLDM_STATESET_LINK_STATE_CONNECTED);
        auto [msgUp, argUp, levelUp, eventUp,
              impactedUp] = stateSet->getEventData(nullptr);
        EXPECT_EQ("LinkUp", argUp);

        sensor->updateReading(true, true, 0,
                              PLDM_STATESET_LINK_STATE_DISCONNECTED);
        auto sensorEventInfo = std::make_shared<utils::SensorEventInfo>();
        sensorEventInfo->eventIdsMap["LinkDown"] = "ResourceEvent.1.0.LinkDown";
        sensorEventInfo->impactedComponent = "Switch0";
        auto [msgDown, argDown, levelDown, eventDown,
              impactedDown] = stateSet->getEventData(sensorEventInfo.get());
        EXPECT_EQ("LinkDown", argDown);
        EXPECT_EQ("ResourceEvent.1.0.LinkDown", eventDown);
        EXPECT_EQ("Switch0", impactedDown);
        // LinkDown is Critical (ResourceErrorsDetected + Alert)
        EXPECT_EQ("ResourceEvent.1.0.ResourceErrorsDetected", msgDown);
        EXPECT_EQ(Level::Alert, levelDown);

        sensor->updateReading(true, true, 0, 0xFF);
        auto [msgUnknown, argUnknown, levelUnknown, eventUnknown,
              impactedUnknown] = stateSet->getEventData(nullptr);
        EXPECT_EQ("Unknown", argUnknown);
        EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedWarning", msgUnknown);
        EXPECT_EQ(Level::Warning, levelUnknown);

        stateSet->setPortTypeValue(PortType::UpstreamPort);
        stateSet->setPortProtocolValue(PortProtocol::Ethernet);
        stateSet->setMaxSpeedValue(100.0);
        stateSet->addAssociation(
            {{"chassis", "all_states", inventoryPaths[0]}});

        EXPECT_EQ("", stateSet->getStringStateType());
        stateSet->updateSensorName("Eth-ib-port0");
        EXPECT_EQ("Eth-ib-port0", stateSet->getStringStateType());
    }
}

TEST_F(StateSensorCoverage, stateSetCreatorErrorPaths)
{
    std::string path =
        "/xyz/openbmc_project/state/PLDM_Sensor_creator_coverage/Id_0";
    dbus::PathAssociation association = {"chassis", "all_states",
                                         "/xyz/openbmc_project/inventory/test"};

    EXPECT_EQ(nullptr, StateSetCreator::createSensor(0xFFFE, 0, path,
                                                     association, nullptr));

    std::string uuid("00000000-0000-0000-0000-000000000007");
    auto terminus =
        Terminus(7, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    auto pdr = makeStateSensorPdr(7, PLDM_ENTITY_SYS_BOARD, 1, 0xFFFE, 0x1);
    terminus.pdrs.emplace_back(pdr);
    ASSERT_TRUE(terminus.parsePDRs());
    ASSERT_EQ(1u, terminus.stateSensors.size());
    auto sensor = terminus.stateSensors[0];
    ASSERT_EQ(1u, sensor->stateSets.size());
    EXPECT_EQ(nullptr, sensor->stateSets[0]);

    std::vector<std::string> inventoryPaths{
        "/xyz/openbmc_project/inventory/system/chassis/chassis2"};
    std::vector<std::shared_ptr<NumericSensor>> numericSensors{};
    sensor->setInventoryPaths(inventoryPaths, true);
    sensor->associateNumericSensor(numericSensors);
    sensor->updateReading(true, true, 0, 1);
    sensor->handleSensorEvent(0, 1, 1);
    sensor->handleErrGetSensorReading();

    auto created = StateSetCreator::createSensor(0xFFFD, 0, path, association,
                                                 sensor.get());
    EXPECT_EQ(nullptr, created);
}

TEST_F(StateSensorCoverage, stateSensorLogEntryCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000008");
    auto terminus =
        Terminus(8, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    auto pdr = makeStateSensorPdr(8, PLDM_ENTITY_SYS_BOARD, 1,
                                  PLDM_STATESET_ID_HEALTHSTATE, 0x3F);
    terminus.pdrs.emplace_back(pdr);
    ASSERT_TRUE(terminus.parsePDRs());
    ASSERT_EQ(1u, terminus.stateSensors.size());

    auto sensor = terminus.stateSensors[0];
    ASSERT_NE(nullptr, sensor);

    std::string messageId{"OpenBMC.0.2.StateSensorCoverage"};
    std::string arg1{"Board0 Health"};
    std::string arg2{"Critical"};
    std::string resolution{"None"};
    sensor->createLogEntry(messageId, arg1, arg2, resolution, Level::Warning);

    std::string eventId{"OpenBMC.0.2.TestEvent"};
    std::string impactedComponent{"Board0"};
    sensor->createLogEntryAdditionalOEMArgs(
        messageId, arg1, arg2, resolution, eventId, impactedComponent,
        Level::Critical);

    eventId.clear();
    impactedComponent.clear();
    sensor->createLogEntryAdditionalOEMArgs(
        messageId, arg1, arg2, resolution, eventId, impactedComponent,
        Level::Informational);
}

TEST_F(StateSensorCoverage, stateSetCreatorOemSensorCoverage)
{
    dbus::PathAssociation association = {"chassis", "all_states",
                                         "/xyz/openbmc_project/inventory/test"};

    {
        std::string uuid("00000000-0000-0000-0000-000000000009");
        auto terminus = Terminus(9, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                                 terminusManager);
        auto pdr = makeStateSensorPdr(9, PLDM_ENTITY_MEMORY_CONTROLLER, 1,
                                      PLDM_STATESET_ID_PERFORMANCE, 0x3);
        terminus.pdrs.emplace_back(pdr);
        ASSERT_TRUE(terminus.parsePDRs());
        ASSERT_EQ(1u, terminus.stateSensors.size());

        std::string path =
            "/xyz/openbmc_project/state/PLDM_Sensor_creator_oem/memory";
        auto created = StateSetCreator::createSensor(
            PLDM_STATESET_ID_PERFORMANCE, 0, path, association,
            terminus.stateSensors[0].get());
        EXPECT_NE(nullptr, created);
    }

    {
        std::string uuid("00000000-0000-0000-0000-00000000000A");
        auto terminus = Terminus(10, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                                 terminusManager);
        auto pdr =
            makeStateSensorPdr(STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC,
                               1, PLDM_STATESET_ID_PERFORMANCE, 0x3);
        terminus.pdrs.emplace_back(pdr);
        ASSERT_TRUE(terminus.parsePDRs());
        ASSERT_EQ(1u, terminus.stateSensors.size());

        std::string path =
            "/xyz/openbmc_project/state/PLDM_Sensor_creator_oem/power_break";
        auto created = StateSetCreator::createSensor(
            PLDM_STATESET_ID_PERFORMANCE, 0, path, association,
            terminus.stateSensors[0].get());
        EXPECT_NE(nullptr, created);
    }

    {
        std::string uuid("00000000-0000-0000-0000-00000000000B");
        auto terminus = Terminus(11, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                                 terminusManager);
        auto pdr =
            makeStateSensorPdr(11, PLDM_ENTITY_SYS_BOARD, 1,
                               PLDM_NVIDIA_OEM_STATE_SET_DEBUG_STATE, 0x3);
        terminus.pdrs.emplace_back(pdr);
        ASSERT_TRUE(terminus.parsePDRs());
        ASSERT_EQ(1u, terminus.stateSensors.size());

        std::string path =
            "/xyz/openbmc_project/state/PLDM_Sensor_creator_oem/debug";
        auto created = StateSetCreator::createSensor(
            PLDM_NVIDIA_OEM_STATE_SET_DEBUG_STATE, 0, path, association,
            terminus.stateSensors[0].get());
        EXPECT_NE(nullptr, created);
    }

    {
        std::string uuid("00000000-0000-0000-0000-00000000000C");
        auto terminus = Terminus(12, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                                 terminusManager);
        auto pdr = makeStateSensorPdr(12, PLDM_ENTITY_SYS_BUS, 1,
                                      PLDM_NVIDIA_OEM_STATE_SET_NVLINK, 0x3);
        terminus.pdrs.emplace_back(pdr);
        ASSERT_TRUE(terminus.parsePDRs());
        ASSERT_EQ(1u, terminus.stateSensors.size());

        std::string path =
            "/xyz/openbmc_project/state/PLDM_Sensor_creator_oem/nvlink";
        auto created = StateSetCreator::createSensor(
            PLDM_NVIDIA_OEM_STATE_SET_NVLINK, 0, path, association,
            terminus.stateSensors[0].get());
        EXPECT_NE(nullptr, created);
    }
}

TEST_F(StateSensorCoverage, oemStateSetMethodCoverage)
{
    std::vector<std::shared_ptr<NumericSensor>> emptyNumericSensors{};
    std::vector<std::string> inventoryPaths{
        "/xyz/openbmc_project/inventory/system/chassis/chassis13"};

    {
        std::string uuid("00000000-0000-0000-0000-00000000000D");
        auto terminus = Terminus(13, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                                 terminusManager);
        auto pdr = makeStateSensorPdr(13, PLDM_ENTITY_MEMORY_CONTROLLER, 1,
                                      PLDM_STATESET_ID_PERFORMANCE, 0x3);
        terminus.pdrs.emplace_back(pdr);
        ASSERT_TRUE(terminus.parsePDRs());
        ASSERT_EQ(1u, terminus.stateSensors.size());

        auto sensor = terminus.stateSensors[0];
        sensor->setInventoryPaths(inventoryPaths, false);
        sensor->associateNumericSensor(emptyNumericSensors);
        auto stateSet = std::dynamic_pointer_cast<StateSetMemoryPerformance>(
            sensor->stateSets[0]);
        ASSERT_NE(nullptr, stateSet);

        stateSet->setDefaultValue();
        stateSet->setValue(PLDM_STATESET_PERFORMANCE_NORMAL);
        auto [msgNormal, argNormal, levelNormal, eventNormal,
              impactedNormal] = stateSet->getEventData(nullptr);
        EXPECT_EQ("Normal", argNormal);

        stateSet->setValue(PLDM_STATESET_PERFORMANCE_THROTTLED);
        auto [msgThrottled, argThrottled, levelThrottled, eventThrottled,
              impactedThrottled] = stateSet->getEventData(nullptr);
        EXPECT_EQ("PerformanceDegraded due to high temperature", argThrottled);

        stateSet->setValue(0xFF);
        stateSet->updateShmemReading("Value");
        EXPECT_EQ("Performance", stateSet->getStringStateType());
    }

    {
        std::string uuid("00000000-0000-0000-0000-00000000000E");
        auto terminus = Terminus(14, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                                 terminusManager);
        auto pdr =
            makeStateSensorPdr(STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC,
                               1, PLDM_STATESET_ID_PERFORMANCE, 0x3);
        terminus.pdrs.emplace_back(pdr);
        ASSERT_TRUE(terminus.parsePDRs());
        ASSERT_EQ(1u, terminus.stateSensors.size());

        auto sensor = terminus.stateSensors[0];
        sensor->setInventoryPaths(inventoryPaths, false);
        auto stateSet = std::dynamic_pointer_cast<StateSetProcessorPowerBreak>(
            sensor->stateSets[0]);
        ASSERT_NE(nullptr, stateSet);

        stateSet->setDefaultValue();
        stateSet->setValue(PLDM_STATESET_PERFORMANCE_NORMAL);
        auto [msgNormal, argNormal, levelNormal, eventNormal,
              impactedNormal] = stateSet->getEventData(nullptr);
        EXPECT_EQ("Normal", argNormal);

        stateSet->setValue(PLDM_STATESET_PERFORMANCE_THROTTLED);
        auto [msgThrottle, argThrottle, levelThrottle, eventThrottle,
              impactedThrottle] = stateSet->getEventData(nullptr);
        EXPECT_EQ("Throttled", argThrottle);

        stateSet->setValue(0xFF);
        stateSet->updateShmemReading("Value");
        EXPECT_EQ("PowerBreak", stateSet->getStringStateType());

        EXPECT_EQ("com.nvidia.ProcessorPowerBreak.PowerBreakStates.Normal",
                  ProcessorPowerBreakIntf::convertPowerBreakStatesToString(
                      PowerBreakStates::Normal));
        EXPECT_EQ("com.nvidia.ProcessorPowerBreak.PowerBreakStates.Throttled",
                  ProcessorPowerBreakIntf::convertPowerBreakStatesToString(
                      PowerBreakStates::Throttled));
        EXPECT_EQ("com.nvidia.ProcessorPowerBreak.PowerBreakStates.Unknown",
                  ProcessorPowerBreakIntf::convertPowerBreakStatesToString(
                      PowerBreakStates::Unknown));
    }

    {
        std::string uuid("00000000-0000-0000-0000-00000000000F");
        auto terminus = Terminus(15, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                                 terminusManager);
        auto pdr = makeStateSensorPdr(15, PLDM_ENTITY_MEMORY_CONTROLLER, 1,
                                      PLDM_STATESET_ID_PRESENCE, 0x3);
        terminus.pdrs.emplace_back(pdr);
        ASSERT_TRUE(terminus.parsePDRs());
        ASSERT_EQ(1u, terminus.stateSensors.size());

        auto sensor = terminus.stateSensors[0];
        sensor->setInventoryPaths(inventoryPaths, false);
        auto stateSet = std::dynamic_pointer_cast<StateSetMemorySpareChannel>(
            sensor->stateSets[0]);
        ASSERT_NE(nullptr, stateSet);

        stateSet->setDefaultValue();
        stateSet->setValue(PLDM_STATESET_PRESENCE_PRESENT);
        auto [msgPresent, argPresent, levelPresent, eventPresent,
              impactedPresent] = stateSet->getEventData(nullptr);
        EXPECT_EQ("True", argPresent);

        stateSet->setValue(PLDM_STATESET_PRESENCE_NOT_PRESENT);
        auto [msgNotPresent, argNotPresent, levelNotPresent, eventNotPresent,
              impactedNotPresent] = stateSet->getEventData(nullptr);
        EXPECT_EQ("False", argNotPresent);

        stateSet->setValue(0xFF);
        auto [msgUnknown, argUnknown, levelUnknown, eventUnknown,
              impactedUnknown] = stateSet->getEventData(nullptr);
        EXPECT_EQ("Unknown", argUnknown);
        EXPECT_EQ("MemorySpareChannelPresence", stateSet->getStringStateType());
    }

    {
        std::string uuid("00000000-0000-0000-0000-000000000010");
        auto terminus = Terminus(16, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                                 terminusManager);
        auto pdr = makeStateSensorPdr(16, PLDM_ENTITY_SYS_BUS, 1,
                                      PLDM_NVIDIA_OEM_STATE_SET_NVLINK, 0x3);
        terminus.pdrs.emplace_back(pdr);
        ASSERT_TRUE(terminus.parsePDRs());
        ASSERT_EQ(1u, terminus.stateSensors.size());

        auto sensor = terminus.stateSensors[0];
        sensor->setInventoryPaths(inventoryPaths, false);
        auto stateSet = std::dynamic_pointer_cast<oem_nvidia::StateSetNvlink>(
            sensor->stateSets[0]);
        ASSERT_NE(nullptr, stateSet);

        stateSet->setDefaultValue();
        stateSet->setValue(PLDM_STATE_SET_NVLINK_ACTIVE);
        auto [msgUp, argUp, levelUp, eventUp,
              impactedUp] = stateSet->getEventData(nullptr);
        EXPECT_EQ("LinkUp", argUp);

        auto sensorEventInfo = std::make_shared<utils::SensorEventInfo>();
        sensorEventInfo->eventIdsMap["LinkDown"] = "ResourceEvent.1.0.LinkDown";
        sensorEventInfo->impactedComponent = "CPU13";

        stateSet->setValue(PLDM_STATE_SET_NVLINK_INACTIVE);
        auto [msgDown, argDown, levelDown, eventDown,
              impactedDown] = stateSet->getEventData(sensorEventInfo.get());
        EXPECT_EQ("LinkDown", argDown);
        EXPECT_EQ("ResourceEvent.1.0.LinkDown", eventDown);
        EXPECT_EQ("CPU13", impactedDown);

        stateSet->setValue(PLDM_STATE_SET_NVLINK_ERROR);
        auto [msgError, argError, levelError, eventError,
              impactedError] = stateSet->getEventData(nullptr);
        EXPECT_EQ("Error", argError);

        stateSet->setValue(0xFF);
        auto [msgUnknown, argUnknown, levelUnknown, eventUnknown,
              impactedUnknown] = stateSet->getEventData(nullptr);
        EXPECT_EQ("Unknown", argUnknown);
        EXPECT_EQ("NVLink", stateSet->getStringStateType());

        std::vector<dbus::PathAssociation> emptyAssociations{};
        stateSet->setAssociation(emptyAssociations);
        std::vector<dbus::PathAssociation> associations{
            {"chassis", "all_states",
             "/xyz/openbmc_project/inventory/system/chassis/chassis13/ProcessorModule_0"},
            {"chassis", "all_states",
             "/xyz/openbmc_project/inventory/system/chassis/chassis13/cpu0"}};
        stateSet->setAssociation(associations);
        stateSet->updateShmemReading("LinkState");
        stateSet->updateShmemReading("LinkStatus");
    }

    {
        std::string uuid("00000000-0000-0000-0000-000000000011");
        auto terminus = Terminus(17, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                                 terminusManager);
        auto pdr =
            makeStateSensorPdr(17, PLDM_ENTITY_SYS_BOARD, 1,
                               PLDM_NVIDIA_OEM_STATE_SET_DEBUG_STATE, 0x7);
        terminus.pdrs.emplace_back(pdr);
        ASSERT_TRUE(terminus.parsePDRs());
        ASSERT_EQ(1u, terminus.stateSensors.size());

        auto stateSet =
            std::dynamic_pointer_cast<oem_nvidia::StateSetDebugState>(
                terminus.stateSensors[0]->stateSets[0]);
        ASSERT_NE(nullptr, stateSet);

        stateSet->setDefaultValue();
        EXPECT_EQ(PLDM_STATE_SET_DEBUG_STATE_OFFLINE, stateSet->getValue());

        stateSet->setValue(PLDM_STATE_SET_DEBUG_STATE_ENABLED);
        auto [msgEnabled, argEnabled, levelEnabled, eventEnabled,
              impactedEnabled] = stateSet->getEventData(nullptr);
        EXPECT_EQ("Enabled", argEnabled);

        stateSet->setValue(PLDM_STATE_SET_DEBUG_STATE_DISABLED);
        auto [msgDisabled, argDisabled, levelDisabled, eventDisabled,
              impactedDisabled] = stateSet->getEventData(nullptr);
        EXPECT_EQ("Disable", argDisabled);

        stateSet->setValue(PLDM_STATE_SET_DEBUG_STATE_OFFLINE);
        auto [msgOffline, argOffline, levelOffline, eventOffline,
              impactedOffline] = stateSet->getEventData(nullptr);
        EXPECT_EQ("Offline", argOffline);

        stateSet->setValue(0xFF);
        auto [msgUnknown, argUnknown, levelUnknown, eventUnknown,
              impactedUnknown] = stateSet->getEventData(nullptr);
        EXPECT_EQ("Unknown", argUnknown);
        EXPECT_EQ("DebugState", stateSet->getStringStateType());
    }
}

TEST_F(StateSensorCoverage, stateSensorHeaderAndInventoryCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000012");
    auto terminus = Terminus(18, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                             terminusManager);
    auto pdr = makeStateSensorPdr(18, PLDM_ENTITY_SYS_BOARD, 1,
                                  PLDM_STATESET_ID_HEALTHSTATE, 0x3);
    terminus.pdrs.emplace_back(pdr);
    ASSERT_TRUE(terminus.parsePDRs());
    ASSERT_EQ(1u, terminus.stateSensors.size());

    auto sensor = terminus.stateSensors[0];
    ASSERT_NE(nullptr, sensor);
    EXPECT_TRUE(sensor->isDefaultInventoryAssociated());
    EXPECT_TRUE(sensor->getAssociationEntityId().empty());
    EXPECT_EQ(nullptr, sensor->getSensorEventInfo());

    sensor->setRefreshed(true);
    EXPECT_TRUE(sensor->isRefreshed());

    auto sensorEventInfo = std::make_shared<utils::SensorEventInfo>();
    sensorEventInfo->impactedComponent = "CPU0";
    sensor->updateSensorEventInfo(sensorEventInfo);
    ASSERT_NE(nullptr, sensor->getSensorEventInfo());
    EXPECT_EQ("CPU0", sensor->getSensorEventInfo()->impactedComponent);

    std::vector<std::string> inventoryPaths{
        "/xyz/openbmc_project/inventory/system/chassis/chassis18/cpu0"};
    sensor->setInventoryPaths(inventoryPaths, false);
    EXPECT_FALSE(sensor->isDefaultInventoryAssociated());
    EXPECT_EQ("cpu0", sensor->getAssociationEntityId());
}

TEST_F(StateSensorCoverage, oemStateSetTelemetryUpdateCoverage)
{
    {
        std::string uuid("00000000-0000-0000-0000-000000000013");
        auto terminus = Terminus(19, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                                 terminusManager);
        auto pdr = makeStateSensorPdr(19, PLDM_ENTITY_MEMORY_CONTROLLER, 1,
                                      PLDM_STATESET_ID_PERFORMANCE, 0x3);
        terminus.pdrs.emplace_back(pdr);
        ASSERT_TRUE(terminus.parsePDRs());
        auto sensor = terminus.stateSensors[0];
        auto stateSet = std::dynamic_pointer_cast<StateSetMemoryPerformance>(
            sensor->stateSets[0]);
        ASSERT_NE(nullptr, stateSet);

        std::vector<std::string> inventoryPaths{
            "/xyz/openbmc_project/inventory/system/chassis/chassis19/CPU_0"};
        sensor->setInventoryPaths(inventoryPaths, false);
        std::vector<dbus::PathAssociation> associations{
            {"memory", "all_states", inventoryPaths[0]}};
        stateSet->setAssociation(associations);
        stateSet->setValue(PLDM_STATESET_PERFORMANCE_THROTTLED);
        stateSet->updateShmemReading("Value");
    }

    {
        std::string uuid("00000000-0000-0000-0000-000000000014");
        auto terminus = Terminus(20, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                                 terminusManager);
        auto pdr =
            makeStateSensorPdr(STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC,
                               1, PLDM_STATESET_ID_PERFORMANCE, 0x3);
        terminus.pdrs.emplace_back(pdr);
        ASSERT_TRUE(terminus.parsePDRs());
        auto sensor = terminus.stateSensors[0];
        auto stateSet = std::dynamic_pointer_cast<StateSetProcessorPowerBreak>(
            sensor->stateSets[0]);
        ASSERT_NE(nullptr, stateSet);

        std::vector<std::string> inventoryPaths{
            "/xyz/openbmc_project/inventory/system/chassis/chassis20/cpu0"};
        sensor->setInventoryPaths(inventoryPaths, false);
        std::vector<dbus::PathAssociation> associations{
            {"memory", "all_states", inventoryPaths[0]}};
        stateSet->setAssociation(associations);
        stateSet->setValue(PLDM_STATESET_PERFORMANCE_THROTTLED);
        stateSet->updateShmemReading("Value");
    }

    {
        std::string uuid("00000000-0000-0000-0000-000000000015");
        auto terminus = Terminus(21, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                                 terminusManager);
        auto pdr = makeStateSensorPdr(21, PLDM_ENTITY_MEMORY_CONTROLLER, 1,
                                      PLDM_STATESET_ID_PRESENCE, 0x3);
        terminus.pdrs.emplace_back(pdr);
        ASSERT_TRUE(terminus.parsePDRs());
        auto sensor = terminus.stateSensors[0];
        auto stateSet = std::dynamic_pointer_cast<StateSetMemorySpareChannel>(
            sensor->stateSets[0]);
        ASSERT_NE(nullptr, stateSet);

        std::vector<std::string> inventoryPaths{
            "/xyz/openbmc_project/inventory/system/chassis/chassis21/dimm0"};
        sensor->setInventoryPaths(inventoryPaths, false);
        std::vector<dbus::PathAssociation> associations{
            {"chassis", "all_states", inventoryPaths[0]}};
        stateSet->setAssociation(associations);
        stateSet->setValue(PLDM_STATESET_PRESENCE_PRESENT);
        stateSet->updateShmemReading("MemorySpareChannelPresence");
    }

    {
        std::string uuid("00000000-0000-0000-0000-000000000016");
        auto terminus = Terminus(22, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                                 terminusManager);
        auto pdr = makeStateSensorPdr(22, PLDM_ENTITY_SYS_BUS, 1,
                                      PLDM_NVIDIA_OEM_STATE_SET_NVLINK, 0x3);
        terminus.pdrs.emplace_back(pdr);
        ASSERT_TRUE(terminus.parsePDRs());
        auto sensor = terminus.stateSensors[0];
        auto stateSet = std::dynamic_pointer_cast<oem_nvidia::StateSetNvlink>(
            sensor->stateSets[0]);
        ASSERT_NE(nullptr, stateSet);

        std::vector<std::string> inventoryPaths{
            "/xyz/openbmc_project/inventory/system/chassis/chassis22/cpu0"};
        sensor->setInventoryPaths(inventoryPaths, false);
        std::vector<dbus::PathAssociation> associations{
            {"chassis", "all_states", inventoryPaths[0]}};
        stateSet->setAssociation(associations);

        stateSet->setValue(PLDM_STATE_SET_NVLINK_INACTIVE);
        auto [msgDown, argDown, levelDown, eventDown,
              impactedDown] = stateSet->getEventData(nullptr);
        EXPECT_EQ("LinkDown", argDown);

        stateSet->updateShmemReading("Unexpected");
        stateSet->setValue(PLDM_STATE_SET_NVLINK_ERROR);
        auto [msgError, argError, levelError, eventError,
              impactedError] = stateSet->getEventData(nullptr);
        EXPECT_EQ("Error", argError);
    }
}

TEST_F(StateSensorCoverage, stateSensorEventSuppressionCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000017");
    auto terminus = Terminus(23, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                             terminusManager);
    auto pdr = makeStateSensorPdr(23, PLDM_ENTITY_SYS_BOARD, 1,
                                  PLDM_STATESET_ID_HEALTHSTATE, 0x3F);
    terminus.pdrs.emplace_back(pdr);
    ASSERT_TRUE(terminus.parsePDRs());
    auto sensor = terminus.stateSensors[0];
    ASSERT_NE(nullptr, sensor);

    sensor->handleSensorEvent(0, PLDM_STATESET_HEALTH_STATE_CRITICAL,
                              PLDM_STATESET_HEALTH_STATE_NORMAL);

    std::string entityName{"CPU_23"};
#ifdef PLATFORM_PREFIX
    entityName = std::string(PLATFORM_PREFIX) + "_CPU_23";
#endif
    std::vector<std::string> inventoryPaths{
        "/xyz/openbmc_project/inventory/system/chassis/chassis23/" +
        entityName};
    sensor->setInventoryPaths(inventoryPaths, false);
    EXPECT_FALSE(sensor->getAssociationEntityId().empty());

    sensor->handleSensorEvent(0, PLDM_STATESET_HEALTH_STATE_CRITICAL, 0);
    sensor->handleSensorEvent(0, PLDM_STATESET_HEALTH_STATE_NON_CRITICAL,
                              PLDM_STATESET_HEALTH_STATE_CRITICAL);
}

TEST_F(StateSensorCoverage, stateSensorEmptyStateTypeCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000018");
    auto terminus = Terminus(24, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                             terminusManager);
    auto pdr = makeStateSensorPdr(24, PLDM_ENTITY_ETHERNET, 1,
                                  PLDM_STATESET_ID_LINKSTATE, 0x3);
    terminus.pdrs.emplace_back(pdr);
    ASSERT_TRUE(terminus.parsePDRs());
    auto sensor = terminus.stateSensors[0];
    ASSERT_NE(nullptr, sensor);

    std::vector<std::string> inventoryPaths{
        "/xyz/openbmc_project/inventory/system/chassis/chassis24/switch0"};
    sensor->setInventoryPaths(inventoryPaths, false);
    sensor->handleSensorEvent(0, PLDM_STATESET_LINK_STATE_CONNECTED,
                              PLDM_STATESET_LINK_STATE_DISCONNECTED);

    pldm::platform_mc::AuxiliaryNames auxNames{{{"en", "EthPort0"}}};
    sensor->updateSensorNames(auxNames);
    sensor->handleSensorEvent(0, PLDM_STATESET_LINK_STATE_CONNECTED,
                              PLDM_STATESET_LINK_STATE_DISCONNECTED);
}

TEST_F(StateSensorCoverage, stateSensorAuxNameFallbackCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_LINK_STATE_CONNECTED,
                                  PLDM_STATESET_LINK_STATE_DISCONNECTED};
    StateSetInfo info = std::make_tuple(
        EntityInfo{1, PLDM_ENTITY_ETHERNET, 1},
        std::vector<StateSetData>{
            {PLDM_STATESET_ID_LINKSTATE, possibleStates},
            {PLDM_STATESET_ID_LINKSTATE, possibleStates}});
    pldm::platform_mc::AuxiliaryNames auxNames{{{{"fr", "Port-Fr"}}}};
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis30"};

    StateSensor sensor(30, false, 0x3000, info, &auxNames, associationPath,
                       nullptr);
    ASSERT_EQ(2u, sensor.stateSets.size());

    pldm::platform_mc::AuxiliaryNames emptyAuxNames{};
    sensor.updateSensorNames(emptyAuxNames);

    pldm::platform_mc::AuxiliaryNames nonEnglishAuxNames{{{{"fr", "Lien-Fr"}}}};
    sensor.updateSensorNames(nonEnglishAuxNames);

    sensor.updateReading(true, true, 3, PLDM_STATESET_LINK_STATE_CONNECTED);
    sensor.handleErrGetSensorReading();
}

TEST_F(StateSensorCoverage, stateSensorGuardBranchCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000019");
    auto terminus = Terminus(25, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                             terminusManager);
    auto pdr = makeStateSensorPdr(25, PLDM_ENTITY_SYS_BOARD, 1,
                                  PLDM_STATESET_ID_HEALTHSTATE, 0x3F);
    terminus.pdrs.emplace_back(pdr);
    ASSERT_TRUE(terminus.parsePDRs());
    ASSERT_EQ(1u, terminus.stateSensors.size());

    auto sensor = terminus.stateSensors[0];
    ASSERT_NE(nullptr, sensor);
    ASSERT_EQ(1u, sensor->stateSets.size());

    std::vector<std::string> inventoryPaths{
        "/xyz/openbmc_project/inventory/system/chassis/chassis25/cpu0"};
    sensor->setInventoryPaths(inventoryPaths, false);
    EXPECT_FALSE(sensor->getAssociationEntityId().empty());

    pldm::platform_mc::AuxiliaryNames emptyAuxNames{};
    sensor->updateSensorNames(emptyAuxNames);

    pldm::platform_mc::AuxiliaryNames nonEnglishAuxNames{
        {{{"fr", "Sante-Fr"}}}};
    sensor->updateSensorNames(nonEnglishAuxNames);

    sensor->handleSensorEvent(0, PLDM_STATESET_HEALTH_STATE_NORMAL, 0);
    sensor->updateReading(true, true, 4, PLDM_STATESET_HEALTH_STATE_NORMAL);

    sensor->stateSets[0] = nullptr;
    sensor->updateReading(true, true, 0, PLDM_STATESET_HEALTH_STATE_CRITICAL);
    sensor->handleSensorEvent(0, PLDM_STATESET_HEALTH_STATE_CRITICAL, 1);
    sensor->handleErrGetSensorReading();
    sensor->updateSensorNames(emptyAuxNames);
}

TEST_F(StateSensorCoverage, stateSensorInlineAndAssociationFlagCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis31"};

    StateSensor sensor(31, false, 0x3100, info, nullptr, associationPath,
                       nullptr);
    EXPECT_TRUE(sensor.isDefaultInventoryAssociated());
    EXPECT_TRUE(sensor.getAssociationEntityId().empty());
    EXPECT_FALSE(sensor.isRefreshed());

    sensor.setRefreshed(true);
    EXPECT_TRUE(sensor.isRefreshed());

    sensor.refreshLimitInUsec = 50;
    sensor.setLastUpdatedTimeStamp(100);
    EXPECT_FALSE(sensor.needsUpdate(120));
    EXPECT_TRUE(sensor.needsUpdate(200));

    std::vector<std::string> inventoryPaths{
        "/xyz/openbmc_project/inventory/system/chassis/chassis31/cpu0"};
    sensor.setInventoryPaths(inventoryPaths, true);
    EXPECT_TRUE(sensor.isDefaultInventoryAssociated());
    EXPECT_TRUE(sensor.getAssociationEntityId().empty());

    auto sensorEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    sensorEventInfo->impactedComponent = "CPU31";
    sensor.updateSensorEventInfo(sensorEventInfo);
    ASSERT_NE(nullptr, sensor.getSensorEventInfo());
    EXPECT_EQ("CPU31", sensor.getSensorEventInfo()->impactedComponent);

    sensor.updateSensorEventInfo(nullptr);
    EXPECT_EQ(nullptr, sensor.getSensorEventInfo());

    sensor.setInventoryPaths(inventoryPaths, false);
    EXPECT_FALSE(sensor.isDefaultInventoryAssociated());
    EXPECT_FALSE(sensor.getAssociationEntityId().empty());

    sensor.handleSensorEvent(0, PLDM_STATESET_HEALTH_STATE_NORMAL, 0);
}

TEST_F(StateSensorCoverage, stateSensorConstructorAndSharedPtrCoverage)
{
    PossibleStates healthStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                PLDM_STATESET_HEALTH_STATE_CRITICAL};
    PossibleStates perfStates{PLDM_STATESET_PERFORMANCE_NORMAL,
                              PLDM_STATESET_PERFORMANCE_THROTTLED};
    StateSetInfo info = std::make_tuple(
        EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
        std::vector<StateSetData>{{PLDM_STATESET_ID_HEALTHSTATE, healthStates},
                                  {PLDM_STATESET_ID_PERFORMANCE, perfStates}});
    pldm::platform_mc::AuxiliaryNames auxNames{{{{"en", "HealthState"}}},
                                               {{{"fr", "Perf-Fr"}}}};
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis32"};
    auto initialSensorEventInfo =
        std::make_shared<pldm::utils::SensorEventInfo>();
    initialSensorEventInfo->impactedComponent = "CPU32";

    StateSensor sensor(32, true, 0x3200, info, &auxNames, associationPath,
                       initialSensorEventInfo);
    ASSERT_NE(nullptr, sensor.getSensorEventInfo());
    EXPECT_EQ("CPU32", sensor.getSensorEventInfo()->impactedComponent);
    EXPECT_FALSE(sensor.isRefreshed());

    auto firstCopy = sensor.getSensorEventInfo();
    auto secondCopy = sensor.getSensorEventInfo();
    ASSERT_NE(nullptr, firstCopy);
    ASSERT_NE(nullptr, secondCopy);
    EXPECT_EQ(firstCopy, secondCopy);

    sensor.setRefreshed(false);
    EXPECT_FALSE(sensor.isRefreshed());
    sensor.setRefreshed(true);
    EXPECT_TRUE(sensor.isRefreshed());

    sensor.refreshLimitInUsec = 100;
    sensor.setLastUpdatedTimeStamp(500);
    EXPECT_FALSE(sensor.needsUpdate(550));
    EXPECT_TRUE(sensor.needsUpdate(700));

    std::vector<std::string> inventoryPaths{
        "/xyz/openbmc_project/inventory/system/chassis/chassis32/cpu0",
        "/xyz/openbmc_project/inventory/system/chassis/chassis32/cpu1"};
    sensor.setInventoryPaths(inventoryPaths, false);
    EXPECT_FALSE(sensor.isDefaultInventoryAssociated());
    EXPECT_FALSE(sensor.getAssociationEntityId().empty());

    auto replacementSensorEventInfo =
        std::make_shared<pldm::utils::SensorEventInfo>();
    replacementSensorEventInfo->impactedComponent = "CPU32B";
    sensor.updateSensorEventInfo(replacementSensorEventInfo);
    ASSERT_NE(nullptr, sensor.getSensorEventInfo());
    EXPECT_EQ("CPU32B", sensor.getSensorEventInfo()->impactedComponent);

    firstCopy.reset();
    secondCopy.reset();
    sensor.updateSensorEventInfo(nullptr);
    EXPECT_EQ(nullptr, sensor.getSensorEventInfo());

    sensor.handleSensorEvent(1, PLDM_STATESET_PERFORMANCE_THROTTLED, 0);
}

TEST_F(StateSensorCoverage, stateSensorUnknownToCriticalCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    pldm::platform_mc::AuxiliaryNames auxNames{{{{"en", "HealthState"}}}};
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis33"};

    StateSensor sensor(33, false, 0x3300, info, &auxNames, associationPath,
                       nullptr);
    sensor.setInventoryPaths({associationPath + "/cpu0"}, false);
    sensor.handleSensorEvent(0, PLDM_STATESET_HEALTH_STATE_CRITICAL, 0);
    sensor.handleSensorEvent(0, PLDM_STATESET_HEALTH_STATE_CRITICAL, 1);
}

TEST_F(StateSensorCoverage, stateSensorSharedPtrBranchMatrix)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis34"};

    StateSensor sensor(34, false, 0x3400, info, nullptr, associationPath,
                       nullptr);
    EXPECT_TRUE(sensor.isDefaultInventoryAssociated());
    EXPECT_EQ(nullptr, sensor.getSensorEventInfo());
    EXPECT_FALSE(sensor.isRefreshed());

    sensor.setRefreshed(true);
    EXPECT_TRUE(sensor.isRefreshed());
    sensor.setRefreshed(false);
    EXPECT_FALSE(sensor.isRefreshed());

    sensor.refreshLimitInUsec = 0;
    sensor.setLastUpdatedTimeStamp(20);
    EXPECT_TRUE(sensor.needsUpdate(21));
    sensor.setLastUpdatedTimeStamp(50);
    EXPECT_TRUE(sensor.needsUpdate(10));

    auto firstInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    firstInfo->impactedComponent = "CPU34A";
    auto firstAlias = firstInfo;
    sensor.updateSensorEventInfo(firstInfo);

    auto copy1 = sensor.getSensorEventInfo();
    auto copy2 = sensor.getSensorEventInfo();
    ASSERT_EQ(firstInfo, copy1);
    ASSERT_EQ(copy1, copy2);
    sensor.updateSensorEventInfo(copy1);

    auto secondInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    secondInfo->impactedComponent = "CPU34B";
    sensor.updateSensorEventInfo(secondInfo);
    EXPECT_EQ(secondInfo, sensor.getSensorEventInfo());
    sensor.updateSensorEventInfo(secondInfo);

    copy1.reset();
    copy2.reset();
    firstAlias.reset();
    firstInfo.reset();

    sensor.updateSensorEventInfo(nullptr);
    EXPECT_EQ(nullptr, sensor.getSensorEventInfo());

    sensor.setInventoryPaths({associationPath + "/cpu0"}, false);
    EXPECT_FALSE(sensor.isDefaultInventoryAssociated());
    EXPECT_FALSE(sensor.getAssociationEntityId().empty());

    sensor.setInventoryPaths({}, true);
    EXPECT_TRUE(sensor.isDefaultInventoryAssociated());
}

TEST_F(StateSensorCoverage, stateSensorAssociationEntityIdSizeMatrixCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 10},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_assoc_matrix"};
    const std::array<std::size_t, 6> entityIdSizes{1, 15, 16, 31, 63, 127};

    for (std::size_t size : entityIdSizes)
    {
        StateSensor sensor(0x6D, false, 0x6D00, info, nullptr, associationPath,
                           nullptr);
        auto sensorEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
        sensorEventInfo->impactedComponent = std::string(size, 'E');
        sensor.updateSensorEventInfo(sensorEventInfo);

        auto firstInfoCopy = sensor.getSensorEventInfo();
        auto secondInfoCopy = sensor.getSensorEventInfo();
        ASSERT_EQ(sensorEventInfo, firstInfoCopy);
        ASSERT_EQ(firstInfoCopy, secondInfoCopy);

        const std::string entityId(size, 'a');
        auto inventoryPath = associationPath;
        inventoryPath.push_back('/');
        inventoryPath.append(entityId);
        sensor.setInventoryPaths({std::move(inventoryPath)}, false);
        auto firstAssocCopy = sensor.getAssociationEntityId();
        auto secondAssocCopy = sensor.getAssociationEntityId();
        EXPECT_EQ(entityId, firstAssocCopy);
        EXPECT_EQ(firstAssocCopy, secondAssocCopy);

        sensor.refreshLimitInUsec = size + 1;
        sensor.setLastUpdatedTimeStamp(size * 10);
        EXPECT_FALSE(sensor.needsUpdate(size * 10 + size));
        EXPECT_TRUE(sensor.needsUpdate(size * 10 + size + 2));

        sensor.updateSensorEventInfo(firstInfoCopy);
        EXPECT_EQ(firstInfoCopy, sensor.getSensorEventInfo());
        sensor.updateSensorEventInfo(nullptr);
        EXPECT_EQ(nullptr, sensor.getSensorEventInfo());

        sensor.setInventoryPaths({}, true);
        EXPECT_TRUE(sensor.isDefaultInventoryAssociated());
    }
}

TEST_F(StateSensorCoverage, stateSensorCustomUpdateSensorNamesCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info = std::make_tuple(
        EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
        std::vector<StateSetData>{
            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates},
            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates},
            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis38"};

    StateSensor sensor(38, false, 0x3800, info, nullptr, associationPath,
                       nullptr);
    auto firstStateSet = std::make_shared<TrackingStateSet>(
        "Health",
        TrackingStateSet::EventTuple{"msg0", "arg0", Level::Warning, "", ""});
    auto secondStateSet = std::make_shared<TrackingStateSet>(
        "Health",
        TrackingStateSet::EventTuple{"msg1", "arg1", Level::Warning, "", ""});
    sensor.stateSets = {firstStateSet, nullptr, secondStateSet};

    pldm::platform_mc::AuxiliaryNames auxNames{
        {{"fr", "Etat-Fr"}, {"en", "Health-Primary"}},
        {},
        {{"es", "Estado"}},
    };
    sensor.updateSensorNames(auxNames);

    ASSERT_EQ(1u, firstStateSet->updatedNames.size());
    EXPECT_EQ("Health-Primary", firstStateSet->updatedNames.back());
    ASSERT_EQ(1u, secondStateSet->updatedNames.size());
    EXPECT_EQ("Id_1", secondStateSet->updatedNames.back());
}

TEST_F(StateSensorCoverage, stateSensorCustomHandleErrGetSensorReadingCoverage)
{
    auto sensor = makeTrackingStateSensor(39, 0x3900);
    auto trackedStateSet = std::make_shared<TrackingStateSet>(
        "Health",
        TrackingStateSet::EventTuple{"msg", "arg", Level::Warning, "", ""});
    sensor->stateSets = {trackedStateSet, nullptr};

    sensor->handleErrGetSensorReading();

    EXPECT_EQ(1u, trackedStateSet->defaultValueCalls);
}

TEST_F(StateSensorCoverage,
       stateSensorCustomHandleSensorEventEmptyEntityCoverage)
{
    auto sensor = makeTrackingStateSensor(40, 0x4000);
    auto trackedStateSet = std::make_shared<TrackingStateSet>(
        "Health",
        TrackingStateSet::EventTuple{"msg", "arg", Level::Critical, "", ""});
    sensor->stateSets = {trackedStateSet};

    sensor->handleSensorEvent(0, PLDM_STATESET_HEALTH_STATE_CRITICAL, 1);

    EXPECT_EQ(PLDM_STATESET_HEALTH_STATE_CRITICAL, trackedStateSet->lastValue);
    EXPECT_EQ(nullptr, trackedStateSet->lastSensorEventInfo);
}

TEST_F(StateSensorCoverage,
       stateSensorCustomHandleSensorEventEmptyStateTypeCoverage)
{
    auto sensor = makeTrackingStateSensor(41, 0x4100);
    auto trackedStateSet = std::make_shared<TrackingStateSet>(
        "",
        TrackingStateSet::EventTuple{"msg", "arg", Level::Critical, "", ""});
    sensor->stateSets = {trackedStateSet};
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis41/cpu0"},
        false);

    sensor->handleSensorEvent(0, PLDM_STATESET_HEALTH_STATE_CRITICAL, 1);

    EXPECT_EQ(PLDM_STATESET_HEALTH_STATE_CRITICAL, trackedStateSet->lastValue);
    EXPECT_EQ(nullptr, trackedStateSet->lastSensorEventInfo);
}

TEST_F(StateSensorCoverage,
       stateSensorCustomHandleSensorEventPassesSensorEventInfoCoverage)
{
    auto sensorEventInfo = std::make_shared<pldm::utils::SensorEventInfo>(
        "CPU42", std::unordered_map<std::string, std::string>{});
    auto sensor = makeTrackingStateSensor(42, 0x4200, sensorEventInfo);
    auto trackedStateSet = std::make_shared<TrackingStateSet>(
        "Health",
        TrackingStateSet::EventTuple{"msg", "arg", Level::Critical, "", ""});
    sensor->stateSets = {trackedStateSet};
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis42/cpu0"},
        false);

    sensor->handleSensorEvent(0, PLDM_STATESET_HEALTH_STATE_CRITICAL, 0);

    EXPECT_EQ(sensorEventInfo.get(), trackedStateSet->lastSensorEventInfo);
}

TEST_F(StateSensorCoverage,
       stateSensorCustomHandleSensorEventNullSensorEventInfoCoverage)
{
    auto sensor = makeTrackingStateSensor(43, 0x4300);
    auto trackedStateSet = std::make_shared<TrackingStateSet>(
        "Health",
        TrackingStateSet::EventTuple{"msg", "arg", Level::Critical, "", ""});
    sensor->stateSets = {trackedStateSet};
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis43/cpu0"},
        false);

    sensor->handleSensorEvent(0, PLDM_STATESET_HEALTH_STATE_CRITICAL, 0);

    EXPECT_EQ(nullptr, trackedStateSet->lastSensorEventInfo);
}

TEST_F(StateSensorCoverage, updateReadingWithNoStateSetsCoverage)
{
    StateSetInfo info = std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
                                        std::vector<StateSetData>{});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis35"};
    StateSensor sensor(35, false, 0x3500, info, nullptr, associationPath,
                       nullptr);

    EXPECT_TRUE(sensor.stateSets.empty());
    sensor.updateReading(true, true, 0, PLDM_STATESET_HEALTH_STATE_CRITICAL);
    sensor.handleErrGetSensorReading();
    EXPECT_TRUE(sensor.stateSets.empty());
}

TEST_F(StateSensorCoverage, handleSensorEventWithNoStateSetsCoverage)
{
    StateSetInfo info = std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
                                        std::vector<StateSetData>{});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis36"};
    StateSensor sensor(36, false, 0x3600, info, nullptr, associationPath,
                       nullptr);

    EXPECT_TRUE(sensor.stateSets.empty());
    sensor.handleSensorEvent(0, PLDM_STATESET_HEALTH_STATE_NORMAL, 1);
    EXPECT_TRUE(sensor.stateSets.empty());
}

TEST_F(StateSensorCoverage, handleSensorEventOutOfRangeOffsetCoverage)
{
    auto sensor = makeTrackingStateSensor(46, 0x4600);
    auto trackedStateSet = std::make_shared<TrackingStateSet>(
        "Health",
        TrackingStateSet::EventTuple{"msg", "arg", Level::Warning, "", ""});
    sensor->stateSets = {trackedStateSet};

    sensor->handleSensorEvent(3, PLDM_STATESET_HEALTH_STATE_CRITICAL,
                              PLDM_STATESET_HEALTH_STATE_NORMAL);

    EXPECT_EQ(0u, trackedStateSet->lastValue);
}

TEST_F(StateSensorCoverage, stateSensorAlertUnknownTransitionCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_LINK_STATE_CONNECTED,
                                  PLDM_STATESET_LINK_STATE_DISCONNECTED};
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_ETHERNET, 1},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_LINKSTATE, possibleStates}});
    pldm::platform_mc::AuxiliaryNames auxNames{{{{"en", "EthPortAlert"}}}};
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis37"};

    StateSensor sensor(37, false, 0x3700, info, &auxNames, associationPath,
                       nullptr);
    sensor.setInventoryPaths({associationPath + "/switch0"}, false);

    sensor.handleSensorEvent(0, PLDM_STATESET_LINK_STATE_DISCONNECTED, 0);
    sensor.handleSensorEvent(0, PLDM_STATESET_LINK_STATE_DISCONNECTED,
                             PLDM_STATESET_LINK_STATE_CONNECTED);

    auto stateSet = std::dynamic_pointer_cast<StateSetEthIBPortLinkState>(
        sensor.stateSets[0]);
    ASSERT_NE(nullptr, stateSet);
    auto [message, arg, level, eventId,
          impacted] = stateSet->getEventData(nullptr);
    EXPECT_EQ("LinkDown", arg);
}

TEST_F(StateSensorCoverage, stateSensorInlineAssociationMultiPathCoverage)
{
    auto sensor = makeTrackingStateSensor(45, 0x4500);
    auto firstStateSet = std::make_shared<AssociatingTrackingStateSet>(
        "Health",
        TrackingStateSet::EventTuple{"msg", "arg", Level::Critical, "", ""});
    auto secondStateSet = std::make_shared<AssociatingTrackingStateSet>(
        "Performance",
        TrackingStateSet::EventTuple{"perf", "arg", Level::Informational, "",
                                     ""},
        PLDM_STATESET_ID_PERFORMANCE);
    sensor->stateSets = {firstStateSet, nullptr, secondStateSet};

    const std::vector<std::string> inventoryPaths{
        "/xyz/openbmc_project/inventory/system/chassis/chassis45/cpu0",
        "/xyz/openbmc_project/inventory/system/chassis/chassis45/cpu1"};
    sensor->setInventoryPaths(inventoryPaths, false);
    EXPECT_FALSE(sensor->isDefaultInventoryAssociated());
    EXPECT_EQ("cpu1", sensor->getAssociationEntityId());
    ASSERT_EQ(2u, firstStateSet->lastAssociations.size());
    ASSERT_EQ(2u, secondStateSet->lastAssociations.size());
    EXPECT_EQ(inventoryPaths.back(),
              firstStateSet->lastAssociations.back().path);
    EXPECT_EQ(inventoryPaths.back(),
              secondStateSet->lastAssociations.back().path);

    auto valuePdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    valuePdr->sensor_id = 0x451;
    valuePdr->entity_type = PLDM_ENTITY_SYS_BOARD;
    valuePdr->entity_instance_num = 1;
    valuePdr->container_id = 1;
    valuePdr->base_unit = PLDM_SENSOR_UNIT_WATTS;
    valuePdr->sensor_data_size = PLDM_SENSOR_DATA_SIZE_UINT8;
    valuePdr->max_readable.value_u8 = 100;
    valuePdr->min_readable.value_u8 = 0;
    valuePdr->hysteresis.value_u8 = 1;
    valuePdr->range_field_format = PLDM_RANGE_FIELD_FORMAT_UINT8;
    valuePdr->supported_thresholds.byte = 0;
    valuePdr->range_field_support.byte = 0;
    valuePdr->resolution = 1.0f;
    valuePdr->offset = 0.0f;
    valuePdr->update_interval = 1.0f;
    std::string numericName{"state_sensor_association_numeric"};
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis45"};
    auto numericSensor = std::make_shared<NumericSensor>(
        45, false, valuePdr, numericName, associationPath, nullptr);
    std::vector<std::shared_ptr<NumericSensor>> numericSensors{numericSensor};

    sensor->associateNumericSensor(numericSensors);
    EXPECT_EQ(1u, firstStateSet->associateCalls);
    EXPECT_EQ(1u, secondStateSet->associateCalls);
    EXPECT_EQ(1u, firstStateSet->associatedSensorCount);
    EXPECT_EQ(EntityInfo(1, PLDM_ENTITY_SYS_BOARD, 1),
              firstStateSet->lastEntityInfo);
}

TEST_F(StateSensorCoverage, stateSensorInlineRefreshAndTimingCoverage)
{
    auto sensor = makeTrackingStateSensor(47, 0x4700);

    EXPECT_FALSE(sensor->isRefreshed());
    sensor->setRefreshed(true);
    EXPECT_TRUE(sensor->isRefreshed());
    sensor->setRefreshed(false);
    EXPECT_FALSE(sensor->isRefreshed());

    sensor->setLastUpdatedTimeStamp(1000);
    sensor->refreshLimitInUsec = 50;
    EXPECT_FALSE(sensor->needsUpdate(1050));
    EXPECT_TRUE(sensor->needsUpdate(1051));

    auto sensorEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    sensorEventInfo->impactedComponent = "CPU47";
    sensor->updateSensorEventInfo(sensorEventInfo);
    EXPECT_EQ(sensorEventInfo, sensor->getSensorEventInfo());
    sensor->updateSensorEventInfo(nullptr);
    EXPECT_EQ(nullptr, sensor->getSensorEventInfo());
}

TEST_F(StateSensorCoverage,
       stateSensorInlineSetInventoryPathsSkipsNullStateSets)
{
    auto sensor = makeTrackingStateSensor(48, 0x4800);
    auto trackedStateSet = std::make_shared<TrackingStateSet>(
        "Health",
        TrackingStateSet::EventTuple{"msg", "arg", Level::Warning, "", ""});
    sensor->stateSets = {nullptr, trackedStateSet};

    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis48/cpu0"}, true);

    EXPECT_TRUE(sensor->isDefaultInventoryAssociated());
    EXPECT_TRUE(sensor->getAssociationEntityId().empty());
    ASSERT_EQ(1u, trackedStateSet->lastAssociations.size());
    EXPECT_EQ("/xyz/openbmc_project/inventory/system/chassis/chassis48/cpu0",
              trackedStateSet->lastAssociations.front().path);
}

TEST_F(StateSensorCoverage,
       stateSensorInlineSetInventoryPathsAllNullStateSetsPreservesDefaults)
{
    auto sensor = makeTrackingStateSensor(49, 0x4900);
    sensor->stateSets = {nullptr, nullptr};

    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis49/cpu0"},
        false);

    EXPECT_TRUE(sensor->isDefaultInventoryAssociated());
    EXPECT_TRUE(sensor->getAssociationEntityId().empty());
}

TEST_F(StateSensorCoverage, stateSensorInlineAssociateNumericSensorSkipsNulls)
{
    auto sensor = makeTrackingStateSensor(50, 0x5000);
    auto trackedStateSet = std::make_shared<AssociatingTrackingStateSet>(
        "Health",
        TrackingStateSet::EventTuple{"msg", "arg", Level::Warning, "", ""});
    sensor->stateSets = {nullptr, trackedStateSet};

    auto sensorPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    sensorPdr->sensor_id = 0x501;
    sensorPdr->entity_type = PLDM_ENTITY_SYS_BOARD;
    sensorPdr->entity_instance_num = 1;
    sensorPdr->container_id = 1;
    sensorPdr->base_unit = PLDM_SENSOR_UNIT_WATTS;
    sensorPdr->sensor_data_size = PLDM_SENSOR_DATA_SIZE_UINT8;
    sensorPdr->max_readable.value_u8 = 100;
    sensorPdr->min_readable.value_u8 = 0;
    sensorPdr->hysteresis.value_u8 = 1;
    sensorPdr->range_field_format = PLDM_RANGE_FIELD_FORMAT_UINT8;
    sensorPdr->supported_thresholds.byte = 0;
    sensorPdr->range_field_support.byte = 0;
    sensorPdr->resolution = 1.0f;
    sensorPdr->offset = 0.0f;
    sensorPdr->update_interval = 1.0f;

    std::string numericName{"state_sensor_association_numeric_50"};
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis50"};
    auto numericSensor = std::make_shared<NumericSensor>(
        50, false, sensorPdr, numericName, associationPath, nullptr);
    std::vector<std::shared_ptr<NumericSensor>> numericSensors{numericSensor};

    sensor->associateNumericSensor(numericSensors);

    EXPECT_EQ(1u, trackedStateSet->associateCalls);
    EXPECT_EQ(1u, trackedStateSet->associatedSensorCount);
    EXPECT_EQ(EntityInfo(1, PLDM_ENTITY_SYS_BOARD, 1),
              trackedStateSet->lastEntityInfo);
}

TEST_F(StateSensorCoverage, stateSensorUpdateSensorNamesFallbackCoverage)
{
    PossibleStates healthStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                PLDM_STATESET_HEALTH_STATE_CRITICAL};
    PossibleStates perfStates{PLDM_STATESET_PERFORMANCE_NORMAL,
                              PLDM_STATESET_PERFORMANCE_THROTTLED};
    StateSetInfo info = std::make_tuple(
        EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
        std::vector<StateSetData>{{PLDM_STATESET_ID_HEALTHSTATE, healthStates},
                                  {PLDM_STATESET_ID_PERFORMANCE, perfStates}});
    pldm::platform_mc::AuxiliaryNames auxNames{{{{"fr", "Etat"}}},
                                               {{{"en", "PerfCtor"}}}};
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis46"};
    StateSensor sensor(46, false, 0x4600, info, &auxNames, associationPath,
                       nullptr);

    auto firstStateSet = std::make_shared<TrackingStateSet>(
        "Health",
        TrackingStateSet::EventTuple{"msg", "arg", Level::Critical, "", ""});
    auto secondStateSet = std::make_shared<TrackingStateSet>(
        "Performance",
        TrackingStateSet::EventTuple{"perf", "arg", Level::Informational, "",
                                     ""},
        PLDM_STATESET_ID_PERFORMANCE);
    sensor.stateSets = {firstStateSet, secondStateSet};

    pldm::platform_mc::AuxiliaryNames updatedNames{{{{"fr", "ToujoursSansEn"}}},
                                                   {{{"en", "PerfRenamed"}}}};
    sensor.updateSensorNames(updatedNames);
    ASSERT_EQ(1u, firstStateSet->updatedNames.size());
    ASSERT_EQ(1u, secondStateSet->updatedNames.size());
    EXPECT_EQ("Id_0", firstStateSet->updatedNames.back());
    EXPECT_EQ("PerfRenamed", secondStateSet->updatedNames.back());

    sensor.setInventoryPaths({associationPath + "/cpu0"}, true);
    EXPECT_TRUE(sensor.isDefaultInventoryAssociated());
    EXPECT_TRUE(sensor.getAssociationEntityId().empty());

    auto sensorEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    sensorEventInfo->impactedComponent = "CPU46";
    sensor.updateSensorEventInfo(sensorEventInfo);
    EXPECT_EQ(sensorEventInfo, sensor.getSensorEventInfo());
    sensor.updateSensorEventInfo(nullptr);
    EXPECT_EQ(nullptr, sensor.getSensorEventInfo());
}

TEST_F(StateSensorDbusMockTest,
       handleSensorEventLowSeverityUnknownTransitionLogsCoverage)
{
    constexpr auto* logObjPath = "/xyz/openbmc_project/logging";
    constexpr auto* logInterface = "xyz.openbmc_project.Logging.Create";
    constexpr auto* logService = "xyz.openbmc_project.Logging";

    auto sensor = makeTrackingStateSensor(44, 0x4400);
    auto trackedStateSet = std::make_shared<TrackingStateSet>(
        "Health",
        TrackingStateSet::EventTuple{"OpenBMC.0.1.CustomNotice", "Recovered",
                                     Level::Notice, "", ""});
    sensor->stateSets = {trackedStateSet};
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis44/cpu0"},
        false);

    const auto severity =
        sdbusplus::xyz::openbmc_project::Logging::server::convertForMessage(
            Level::Notice);

    testing::InSequence seq;
    expectNewMethodCall(logService, logObjPath, logInterface, "Create");
    expectAppendString("OpenBMC.0.1.CustomNotice");
    expectAppendString(severity.c_str());
    expectStringMap({
        {"DEVICE_NAME", "cpu0"},
        {"ERROR_ID", "CPU0_HEALTH_RECOVERED"},
        {"REDFISH_MESSAGE_ARGS", "cpu0 Health,Recovered"},
        {"REDFISH_MESSAGE_ID", "OpenBMC.0.1.CustomNotice"},
        {"xyz.openbmc_project.Logging.Entry.EventId", "CPU0_HEALTH_RECOVERED"},
        {"xyz.openbmc_project.Logging.Entry.Resolution", "None"},
    });
    expectBusCallNoReply();

    sensor->handleSensorEvent(0, PLDM_STATESET_HEALTH_STATE_NORMAL, 0);
}

TEST_F(StateSensorDbusMockTest,
       handleSensorEventLogsWithNonZeroPreviousStateCoverage)
{
    constexpr auto* logObjPath = "/xyz/openbmc_project/logging";
    constexpr auto* logInterface = "xyz.openbmc_project.Logging.Create";
    constexpr auto* logService = "xyz.openbmc_project.Logging";

    auto sensor = makeTrackingStateSensor(45, 0x4500);
    auto trackedStateSet = std::make_shared<TrackingStateSet>(
        "Health",
        TrackingStateSet::EventTuple{"OpenBMC.0.1.CustomCritical", "Critical",
                                     Level::Critical, "", ""});
    sensor->stateSets = {trackedStateSet};
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis45/cpu0"},
        false);

    const auto severity =
        sdbusplus::xyz::openbmc_project::Logging::server::convertForMessage(
            Level::Critical);

    testing::InSequence seq;
    expectNewMethodCall(logService, logObjPath, logInterface, "Create");
    expectAppendString("OpenBMC.0.1.CustomCritical");
    expectAppendString(severity.c_str());
    expectStringMap({
        {"DEVICE_NAME", "cpu0"},
        {"ERROR_ID", "CPU0_HEALTH_CRITICAL"},
        {"REDFISH_MESSAGE_ARGS", "cpu0 Health,Critical"},
        {"REDFISH_MESSAGE_ID", "OpenBMC.0.1.CustomCritical"},
        {"xyz.openbmc_project.Logging.Entry.EventId", "CPU0_HEALTH_CRITICAL"},
        {"xyz.openbmc_project.Logging.Entry.Resolution", "None"},
    });
    expectBusCallNoReply();

    sensor->handleSensorEvent(0, PLDM_STATESET_HEALTH_STATE_CRITICAL,
                              PLDM_STATESET_HEALTH_STATE_NORMAL);
}

TEST_F(StateSensorDbusMockTest, createLogEntrySuccessCoverage)
{
    constexpr auto* logObjPath = "/xyz/openbmc_project/logging";
    constexpr auto* logInterface = "xyz.openbmc_project.Logging.Create";
    constexpr auto* logService = "xyz.openbmc_project.Logging";
    constexpr pldm::tid_t tid = 0x41;
    constexpr uint16_t sensorId = 0x541;

    std::string uuid("00000000-0000-0000-0000-000000000541");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid]->pdrs.emplace_back(makeStateSensorPdr(
        sensorId, PLDM_ENTITY_SYS_BOARD, 1, PLDM_STATESET_ID_HEALTHSTATE));
    ASSERT_TRUE(termini[tid]->parsePDRs());
    auto sensor = termini[tid]->stateSensors.front();

    std::string messageId{"OpenBMC.0.1.StateSensor"};
    std::string arg1{"CPU0 Health"};
    std::string arg2{"Warning"};
    std::string resolution{"Inspect"};

    testing::InSequence seq;
    expectNewMethodCall(logService, logObjPath, logInterface, "Create");
    expectAppendString(messageId.c_str());
    expectAppendString("xyz.openbmc_project.Logging.Entry.Level.Warning");
    expectStringMap({
        {"REDFISH_MESSAGE_ARGS", "CPU0 Health,Warning"},
        {"REDFISH_MESSAGE_ID", messageId},
        {"xyz.openbmc_project.Logging.Entry.Resolution", resolution},
    });
    expectBusCallNoReply();

    sensor->createLogEntry(messageId, arg1, arg2, resolution, Level::Warning);
}

TEST_F(StateSensorDbusMockTest, createLogEntryFailureCoverage)
{
    constexpr auto* logObjPath = "/xyz/openbmc_project/logging";
    constexpr auto* logInterface = "xyz.openbmc_project.Logging.Create";
    constexpr auto* logService = "xyz.openbmc_project.Logging";
    constexpr pldm::tid_t tid = 0x43;
    constexpr uint16_t sensorId = 0x543;

    std::string uuid("00000000-0000-0000-0000-000000000543");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid]->pdrs.emplace_back(makeStateSensorPdr(
        sensorId, PLDM_ENTITY_SYS_BOARD, 1, PLDM_STATESET_ID_HEALTHSTATE));
    ASSERT_TRUE(termini[tid]->parsePDRs());
    auto sensor = termini[tid]->stateSensors.front();

    std::string messageId{"OpenBMC.0.1.StateSensorFailure"};
    std::string arg1{"CPU2 Health"};
    std::string arg2{"Critical"};
    std::string resolution{"Inspect"};

    testing::InSequence seq;
    expectNewMethodCall(logService, logObjPath, logInterface, "Create");
    expectAppendString(messageId.c_str());
    expectAppendString("xyz.openbmc_project.Logging.Entry.Level.Critical");
    expectStringMap({
        {"REDFISH_MESSAGE_ARGS", "CPU2 Health,Critical"},
        {"REDFISH_MESSAGE_ID", messageId},
        {"xyz.openbmc_project.Logging.Entry.Resolution", resolution},
    });
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, 0, testing::_, nullptr))
        .WillOnce(testing::Return(-1));

    sensor->createLogEntry(messageId, arg1, arg2, resolution, Level::Critical);
}

TEST_F(StateSensorDbusMockTest, createLogEntryGetServiceFailureCoverage)
{
    constexpr auto* logObjPath = "/xyz/openbmc_project/logging";
    constexpr auto* logInterface = "xyz.openbmc_project.Logging.Create";
    constexpr auto* logService = "xyz.openbmc_project.Logging";
    constexpr pldm::tid_t tid = 0x48;
    constexpr uint16_t sensorId = 0x548;

    std::string uuid("00000000-0000-0000-0000-000000000548");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid]->pdrs.emplace_back(makeStateSensorPdr(
        sensorId, PLDM_ENTITY_SYS_BOARD, 1, PLDM_STATESET_ID_HEALTHSTATE));
    ASSERT_TRUE(termini[tid]->parsePDRs());
    auto sensor = termini[tid]->stateSensors.front();

    std::string messageId{"OpenBMC.0.1.StateSensorMapperFailure"};
    std::string arg1{"CPU7 Health"};
    std::string arg2{"Critical"};
    std::string resolution{"Inspect"};

    testing::InSequence seq;
    expectNewMethodCallFailure(logService, logObjPath, logInterface, "Create");

    sensor->createLogEntry(messageId, arg1, arg2, resolution, Level::Critical);
}

TEST_F(StateSensorDbusMockTest, createLogEntryAdditionalOemArgsSuccessCoverage)
{
    constexpr auto* logObjPath = "/xyz/openbmc_project/logging";
    constexpr auto* logInterface = "xyz.openbmc_project.Logging.Create";
    constexpr auto* logService = "xyz.openbmc_project.Logging";
    constexpr pldm::tid_t tid = 0x42;
    constexpr uint16_t sensorId = 0x542;

    std::string uuid("00000000-0000-0000-0000-000000000542");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid]->pdrs.emplace_back(makeStateSensorPdr(
        sensorId, PLDM_ENTITY_SYS_BOARD, 1, PLDM_STATESET_ID_HEALTHSTATE));
    ASSERT_TRUE(termini[tid]->parsePDRs());
    auto sensor = termini[tid]->stateSensors.front();

    std::string messageId{"OpenBMC.0.1.StateSensorOem"};
    std::string arg1{"CPU1 Presence"};
    std::string arg2{"Critical"};
    std::string resolution{"Replace"};
    std::string eventId{"OpenBMC.0.2.Sensor"};
    std::string impactedComponent{"CPU1"};

    testing::InSequence seq;
    expectNewMethodCall(logService, logObjPath, logInterface, "Create");
    expectAppendString(messageId.c_str());
    expectAppendString("xyz.openbmc_project.Logging.Entry.Level.Critical");
    expectStringMap({
        {"DEVICE_NAME", impactedComponent},
        {"ERROR_ID", eventId},
        {"REDFISH_MESSAGE_ARGS", "CPU1 Presence,Critical"},
        {"REDFISH_MESSAGE_ID", messageId},
        {"xyz.openbmc_project.Logging.Entry.EventId", eventId},
        {"xyz.openbmc_project.Logging.Entry.Resolution", resolution},
    });
    expectBusCallNoReply();

    sensor->createLogEntryAdditionalOEMArgs(
        messageId, arg1, arg2, resolution, eventId, impactedComponent,
        Level::Critical);
}

TEST_F(StateSensorDbusMockTest,
       createLogEntryAdditionalOemArgsGetServiceFailureCoverage)
{
    constexpr auto* logObjPath = "/xyz/openbmc_project/logging";
    constexpr auto* logInterface = "xyz.openbmc_project.Logging.Create";
    constexpr auto* logService = "xyz.openbmc_project.Logging";
    constexpr pldm::tid_t tid = 0x49;
    constexpr uint16_t sensorId = 0x549;

    std::string uuid("00000000-0000-0000-0000-000000000549");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid]->pdrs.emplace_back(makeStateSensorPdr(
        sensorId, PLDM_ENTITY_SYS_BOARD, 1, PLDM_STATESET_ID_HEALTHSTATE));
    ASSERT_TRUE(termini[tid]->parsePDRs());
    auto sensor = termini[tid]->stateSensors.front();

    std::string messageId{"OpenBMC.0.1.StateSensorOemMapperFailure"};
    std::string arg1{"CPU8 Health"};
    std::string arg2{"Warning"};
    std::string resolution{"Inspect"};
    std::string eventId{"OpenBMC.0.2.MapperFailure"};
    std::string impactedComponent{"CPU8"};

    testing::InSequence seq;
    expectNewMethodCallFailure(logService, logObjPath, logInterface, "Create");

    sensor->createLogEntryAdditionalOEMArgs(
        messageId, arg1, arg2, resolution, eventId, impactedComponent,
        Level::Warning);
}

TEST_F(StateSensorDbusMockTest, createLogEntryAdditionalOemArgsFailureCoverage)
{
    constexpr auto* logObjPath = "/xyz/openbmc_project/logging";
    constexpr auto* logInterface = "xyz.openbmc_project.Logging.Create";
    constexpr auto* logService = "xyz.openbmc_project.Logging";
    constexpr pldm::tid_t tid = 0x44;
    constexpr uint16_t sensorId = 0x544;

    std::string uuid("00000000-0000-0000-0000-000000000544");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid]->pdrs.emplace_back(makeStateSensorPdr(
        sensorId, PLDM_ENTITY_SYS_BOARD, 1, PLDM_STATESET_ID_HEALTHSTATE));
    ASSERT_TRUE(termini[tid]->parsePDRs());
    auto sensor = termini[tid]->stateSensors.front();

    std::string messageId{"OpenBMC.0.1.StateSensorOemFailure"};
    std::string arg1{"CPU3 Health"};
    std::string arg2{"Critical"};
    std::string resolution{"Replace"};
    std::string eventId{"OpenBMC.0.2.SensorFailure"};
    std::string impactedComponent{"CPU3"};

    testing::InSequence seq;
    expectNewMethodCall(logService, logObjPath, logInterface, "Create");
    expectAppendString(messageId.c_str());
    expectAppendString("xyz.openbmc_project.Logging.Entry.Level.Critical");
    expectStringMap({
        {"DEVICE_NAME", impactedComponent},
        {"ERROR_ID", eventId},
        {"REDFISH_MESSAGE_ARGS", "CPU3 Health,Critical"},
        {"REDFISH_MESSAGE_ID", messageId},
        {"xyz.openbmc_project.Logging.Entry.EventId", eventId},
        {"xyz.openbmc_project.Logging.Entry.Resolution", resolution},
    });
    EXPECT_CALL(mock, sd_bus_call(nullptr, nullptr, 0, testing::_, nullptr))
        .WillOnce(testing::Return(-1));

    sensor->createLogEntryAdditionalOEMArgs(
        messageId, arg1, arg2, resolution, eventId, impactedComponent,
        Level::Critical);
}

TEST_F(StateSensorDbusMockTest,
       createLogEntryAdditionalOemArgsWithoutOptionalFieldsCoverage)
{
    constexpr auto* logObjPath = "/xyz/openbmc_project/logging";
    constexpr auto* logInterface = "xyz.openbmc_project.Logging.Create";
    constexpr auto* logService = "xyz.openbmc_project.Logging";
    constexpr pldm::tid_t tid = 0x45;
    constexpr uint16_t sensorId = 0x545;

    std::string uuid("00000000-0000-0000-0000-000000000545");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid]->pdrs.emplace_back(makeStateSensorPdr(
        sensorId, PLDM_ENTITY_SYS_BOARD, 1, PLDM_STATESET_ID_HEALTHSTATE));
    ASSERT_TRUE(termini[tid]->parsePDRs());
    auto sensor = termini[tid]->stateSensors.front();

    std::string messageId{"OpenBMC.0.1.StateSensorOemOptional"};
    std::string arg1{"CPU4 Health"};
    std::string arg2{"Warning"};
    std::string resolution{"Monitor"};
    std::string eventId{};
    std::string impactedComponent{};

    testing::InSequence seq;
    expectNewMethodCall(logService, logObjPath, logInterface, "Create");
    expectAppendString(messageId.c_str());
    expectAppendString("xyz.openbmc_project.Logging.Entry.Level.Warning");
    expectStringMap({
        {"REDFISH_MESSAGE_ARGS", "CPU4 Health,Warning"},
        {"REDFISH_MESSAGE_ID", messageId},
        {"xyz.openbmc_project.Logging.Entry.Resolution", resolution},
    });
    expectBusCallNoReply();

    sensor->createLogEntryAdditionalOEMArgs(
        messageId, arg1, arg2, resolution, eventId, impactedComponent,
        Level::Warning);
}

TEST_F(StateSensorDbusMockTest,
       createLogEntryAdditionalOemArgsEventIdOnlyCoverage)
{
    constexpr auto* logObjPath = "/xyz/openbmc_project/logging";
    constexpr auto* logInterface = "xyz.openbmc_project.Logging.Create";
    constexpr auto* logService = "xyz.openbmc_project.Logging";
    constexpr pldm::tid_t tid = 0x46;
    constexpr uint16_t sensorId = 0x546;

    std::string uuid("00000000-0000-0000-0000-000000000546");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid]->pdrs.emplace_back(makeStateSensorPdr(
        sensorId, PLDM_ENTITY_SYS_BOARD, 1, PLDM_STATESET_ID_HEALTHSTATE));
    ASSERT_TRUE(termini[tid]->parsePDRs());
    auto sensor = termini[tid]->stateSensors.front();

    std::string messageId{"OpenBMC.0.1.StateSensorOemEventOnly"};
    std::string arg1{"CPU5 Health"};
    std::string arg2{"Critical"};
    std::string resolution{"Escalate"};
    std::string eventId{"OpenBMC.0.2.EventOnly"};
    std::string impactedComponent{};

    testing::InSequence seq;
    expectNewMethodCall(logService, logObjPath, logInterface, "Create");
    expectAppendString(messageId.c_str());
    expectAppendString("xyz.openbmc_project.Logging.Entry.Level.Critical");
    expectStringMap({
        {"ERROR_ID", eventId},
        {"REDFISH_MESSAGE_ARGS", "CPU5 Health,Critical"},
        {"REDFISH_MESSAGE_ID", messageId},
        {"xyz.openbmc_project.Logging.Entry.EventId", eventId},
        {"xyz.openbmc_project.Logging.Entry.Resolution", resolution},
    });
    expectBusCallNoReply();

    sensor->createLogEntryAdditionalOEMArgs(
        messageId, arg1, arg2, resolution, eventId, impactedComponent,
        Level::Critical);
}

TEST_F(StateSensorDbusMockTest,
       createLogEntryAdditionalOemArgsImpactedOnlyCoverage)
{
    constexpr auto* logObjPath = "/xyz/openbmc_project/logging";
    constexpr auto* logInterface = "xyz.openbmc_project.Logging.Create";
    constexpr auto* logService = "xyz.openbmc_project.Logging";
    constexpr pldm::tid_t tid = 0x47;
    constexpr uint16_t sensorId = 0x547;

    std::string uuid("00000000-0000-0000-0000-000000000547");
    termini[tid] = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid]->pdrs.emplace_back(makeStateSensorPdr(
        sensorId, PLDM_ENTITY_SYS_BOARD, 1, PLDM_STATESET_ID_HEALTHSTATE));
    ASSERT_TRUE(termini[tid]->parsePDRs());
    auto sensor = termini[tid]->stateSensors.front();

    std::string messageId{"OpenBMC.0.1.StateSensorOemImpactedOnly"};
    std::string arg1{"CPU6 Health"};
    std::string arg2{"Warning"};
    std::string resolution{"Inspect"};
    std::string eventId{};
    std::string impactedComponent{"CPU6"};

    testing::InSequence seq;
    expectNewMethodCall(logService, logObjPath, logInterface, "Create");
    expectAppendString(messageId.c_str());
    expectAppendString("xyz.openbmc_project.Logging.Entry.Level.Warning");
    expectStringMap({
        {"DEVICE_NAME", impactedComponent},
        {"REDFISH_MESSAGE_ARGS", "CPU6 Health,Warning"},
        {"REDFISH_MESSAGE_ID", messageId},
        {"xyz.openbmc_project.Logging.Entry.Resolution", resolution},
    });
    expectBusCallNoReply();

    sensor->createLogEntryAdditionalOEMArgs(
        messageId, arg1, arg2, resolution, eventId, impactedComponent,
        Level::Warning);
}

TEST_F(StateSensorCoverage, stateSensorLongAssociationEntityIdCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_long"};

    StateSensor sensor(0x61, false, 0x6100, info, nullptr, associationPath,
                       nullptr);
    std::vector<std::string> inventoryPaths{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_long/" +
        std::string(72, 'p')};

    sensor.setInventoryPaths(inventoryPaths, false);

    const auto associationEntityId = sensor.getAssociationEntityId();
    EXPECT_EQ(std::string(72, 'p'), associationEntityId);
    EXPECT_FALSE(sensor.isDefaultInventoryAssociated());
}

TEST_F(StateSensorCoverage,
       stateSensorSetInventoryPathsSkipsNullStateSetCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_null"};

    StateSensor sensor(0x62, false, 0x6200, info, nullptr, associationPath,
                       nullptr);
    ASSERT_EQ(1u, sensor.stateSets.size());
    sensor.stateSets[0] = nullptr;

    sensor.setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis_null/cpu0"},
        false);

    EXPECT_TRUE(sensor.isDefaultInventoryAssociated());
    EXPECT_TRUE(sensor.getAssociationEntityId().empty());
}

TEST_F(StateSensorCoverage,
       stateSensorAssociateNumericSensorNullStateSetCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_assoc"};

    StateSensor sensor(0x63, false, 0x6300, info, nullptr, associationPath,
                       nullptr);
    ASSERT_EQ(1u, sensor.stateSets.size());
    sensor.stateSets[0] = nullptr;

    std::vector<std::shared_ptr<NumericSensor>> numericSensors{};
    EXPECT_NO_THROW(sensor.associateNumericSensor(numericSensors));
}

TEST_F(StateSensorCoverage, stateSensorAssociationEntityIdCopyCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 1},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_assoc_copy"};

    StateSensor shortSensor(0x64, false, 0x6400, info, nullptr, associationPath,
                            nullptr);
    shortSensor.setInventoryPaths({associationPath + "/cpu0"}, false);
    EXPECT_EQ("cpu0", shortSensor.getAssociationEntityId());
    EXPECT_FALSE(shortSensor.isDefaultInventoryAssociated());

    StateSensor defaultSensor(0x65, false, 0x6500, info, nullptr,
                              associationPath, nullptr);
    defaultSensor.setInventoryPaths({associationPath + "/cpu1"}, true);
    EXPECT_TRUE(defaultSensor.getAssociationEntityId().empty());
    EXPECT_TRUE(defaultSensor.isDefaultInventoryAssociated());

    StateSensor longSensor(0x66, false, 0x6600, info, nullptr, associationPath,
                           nullptr);
    const std::string longEntityId(88, 'L');
    longSensor.setInventoryPaths(
        {associationPath + "/cpu2", associationPath + "/" + longEntityId},
        false);
    EXPECT_EQ(longEntityId, longSensor.getAssociationEntityId());
    EXPECT_FALSE(longSensor.isDefaultInventoryAssociated());
}

TEST_F(StateSensorCoverage, stateSensorSensorEventInfoCopyCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 2},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_event_info"};

    StateSensor sensor(0x67, false, 0x6700, info, nullptr, associationPath,
                       nullptr);
    EXPECT_EQ(nullptr, sensor.getSensorEventInfo());

    auto shortInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    shortInfo->impactedComponent = "CPU67";
    sensor.updateSensorEventInfo(shortInfo);
    auto shortCopy1 = sensor.getSensorEventInfo();
    auto shortCopy2 = sensor.getSensorEventInfo();
    EXPECT_EQ(shortInfo, shortCopy1);
    EXPECT_EQ(shortCopy1, shortCopy2);
    EXPECT_EQ("CPU67", shortCopy1->impactedComponent);

    auto longInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    longInfo->impactedComponent = std::string(96, 'I');
    longInfo->eventIdsMap.emplace("LinkDown",
                                  "ResourceEvent.1.0." + std::string(40, 'D'));
    sensor.updateSensorEventInfo(longInfo);
    auto longCopy1 = sensor.getSensorEventInfo();
    auto longCopy2 = sensor.getSensorEventInfo();
    EXPECT_EQ(longInfo, longCopy1);
    EXPECT_EQ(longCopy1, longCopy2);
    EXPECT_EQ(std::string(96, 'I'), longCopy1->impactedComponent);
    EXPECT_EQ("ResourceEvent.1.0." + std::string(40, 'D'),
              longCopy1->eventIdsMap.at("LinkDown"));

    sensor.updateSensorEventInfo(nullptr);
    EXPECT_EQ(nullptr, sensor.getSensorEventInfo());
}

TEST_F(StateSensorCoverage, stateSensorRefreshedAndNeedsUpdateBoundaryCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 3},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_refresh"};

    StateSensor sensor(0x68, false, 0x6800, info, nullptr, associationPath,
                       nullptr);
    EXPECT_FALSE(sensor.isRefreshed());
    sensor.setRefreshed(true);
    EXPECT_TRUE(sensor.isRefreshed());
    sensor.setRefreshed(false);
    EXPECT_FALSE(sensor.isRefreshed());

    sensor.refreshLimitInUsec = 50;
    sensor.setLastUpdatedTimeStamp(100);
    EXPECT_FALSE(sensor.needsUpdate(150));
    EXPECT_TRUE(sensor.needsUpdate(151));

    sensor.setLastUpdatedTimeStamp(0);
    EXPECT_FALSE(sensor.needsUpdate(50));
    EXPECT_TRUE(sensor.needsUpdate(51));
}

TEST_F(StateSensorCoverage,
       stateSensorAssociateNumericSensorNonNullStateSetCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 4},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_numeric"};

    StateSensor sensor(0x69, false, 0x6900, info, nullptr, associationPath,
                       nullptr);
    std::vector<std::shared_ptr<NumericSensor>> numericSensors{};

    EXPECT_NO_THROW(sensor.associateNumericSensor(numericSensors));
    sensor.setInventoryPaths({associationPath + "/cpu0"}, false);
    EXPECT_FALSE(sensor.getAssociationEntityId().empty());
}

TEST_F(StateSensorCoverage, stateSensorInlineSetInventoryPathsBadAllocCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 7},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_alloc_path"};
    const std::string longInventoryPath =
        associationPath + "/" + std::string(160, 'a');

    EXPECT_TRUE(state_sensor_test_alloc::exerciseBadAlloc([&] {
        StateSensor sensor(0x6A, false, 0x6A00, info, nullptr, associationPath,
                           nullptr);
        sensor.setInventoryPaths({longInventoryPath}, false);
    }));
}

TEST_F(StateSensorCoverage, stateSensorInlineGetAssociationIdBadAllocCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 8},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_assoc_copy_alloc"};
    const std::string longInventoryPath =
        associationPath + "/" + std::string(192, 'b');

    StateSensor sensor(0x6B, false, 0x6B00, info, nullptr, associationPath,
                       nullptr);
    sensor.setInventoryPaths({longInventoryPath}, false);

    EXPECT_TRUE(state_sensor_test_alloc::exerciseBadAlloc([&] {
        auto copy = sensor.getAssociationEntityId();
        (void)copy;
    }));
}

TEST_F(StateSensorCoverage,
       stateSensorInlineSetInventoryPathsNonCanonicalPathCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 9},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_invalid_path"};

    StateSensor sensor(0x6C, false, 0x6C00, info, nullptr, associationPath,
                       nullptr);

    sensor.setInventoryPaths({"not a valid object path"}, false);
    EXPECT_TRUE(sensor.getAssociationEntityId().empty());
    EXPECT_FALSE(sensor.isDefaultInventoryAssociated());
}

TEST_F(StateSensorCoverage,
       stateSensorInlineSetInventoryPathsExhaustiveBadAllocCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 10},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_exhaustive_alloc"};
    const std::string longInventoryPath =
        associationPath + "/" + std::string(256, 'x');

    EXPECT_TRUE(state_sensor_test_alloc::exerciseAllBadAlloc(
        [&] {
            StateSensor sensor(0x6D, false, 0x6D00, info, nullptr,
                               associationPath, nullptr);
            sensor.setInventoryPaths({longInventoryPath}, false);
        },
        2048));
}

TEST_F(StateSensorCoverage,
       stateSensorInlineSetInventoryPathsExhaustiveBadAllocAcrossPathsCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{2, PLDM_ENTITY_SYS_BOARD, 11},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_exhaustive_alloc_multi"};
    const std::vector<std::string> inventoryPaths{
        associationPath + "/" + std::string(224, 'a'),
        associationPath + "/" + std::string(232, 'b')};

    EXPECT_TRUE(state_sensor_test_alloc::exerciseAllBadAlloc(
        [&] {
            StateSensor sensor(0x6E, false, 0x6E00, info, nullptr,
                               associationPath, nullptr);
            sensor.setInventoryPaths(inventoryPaths, false);
        },
        2048));
}

TEST_F(StateSensorCoverage, stateSensorInlineGetterAndFlagMatrixCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{3, PLDM_ENTITY_SYS_BOARD, 12},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_getter_matrix"};

    auto sensorEventInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    sensorEventInfo->impactedComponent = std::string(128, 'I');
    sensorEventInfo->eventIdsMap.emplace(
        "LinkDown", "ResourceEvent.1.0." + std::string(48, 'D'));

    StateSensor sensor(0x6F, false, 0x6F00, info, nullptr, associationPath,
                       sensorEventInfo);

    EXPECT_EQ(sensorEventInfo, sensor.getSensorEventInfo());
    auto firstInfoCopy = sensor.getSensorEventInfo();
    auto secondInfoCopy = sensor.getSensorEventInfo();
    ASSERT_EQ(sensorEventInfo, firstInfoCopy);
    ASSERT_EQ(firstInfoCopy, secondInfoCopy);
    EXPECT_EQ(std::string(128, 'I'), firstInfoCopy->impactedComponent);
    EXPECT_EQ("ResourceEvent.1.0." + std::string(48, 'D'),
              firstInfoCopy->eventIdsMap.at("LinkDown"));

    sensor.setRefreshed(false);
    EXPECT_FALSE(sensor.isRefreshed());
    sensor.setRefreshed(true);
    EXPECT_TRUE(sensor.isRefreshed());
    sensor.setRefreshed(false);
    EXPECT_FALSE(sensor.isRefreshed());

    sensor.refreshLimitInUsec = 100;
    sensor.setLastUpdatedTimeStamp(1000);
    EXPECT_FALSE(sensor.needsUpdate(1100));
    EXPECT_TRUE(sensor.needsUpdate(1101));

    sensor.setInventoryPaths(
        {associationPath + "/cpu0", associationPath + "/cpu1"}, true);
    EXPECT_TRUE(sensor.isDefaultInventoryAssociated());
    EXPECT_TRUE(sensor.getAssociationEntityId().empty());

    sensor.setInventoryPaths({associationPath + "/" + std::string(128, 'x')},
                             false);
    EXPECT_FALSE(sensor.isDefaultInventoryAssociated());
    EXPECT_FALSE(sensor.getAssociationEntityId().empty());

    sensor.updateSensorEventInfo(nullptr);
    EXPECT_EQ(nullptr, sensor.getSensorEventInfo());
    sensor.updateSensorEventInfo(sensorEventInfo);
    EXPECT_EQ(sensorEventInfo, sensor.getSensorEventInfo());
}

TEST_F(StateSensorCoverage,
       stateSensorInlineGetAssociationIdExhaustiveBadAllocCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{4, PLDM_ENTITY_SYS_BOARD, 13},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_assoc_copy_exhaustive"};
    const std::string longInventoryPath =
        associationPath + "/" + std::string(224, 'c');

    StateSensor sensor(0x70, false, 0x7000, info, nullptr, associationPath,
                       nullptr);
    sensor.setInventoryPaths({longInventoryPath}, false);

    EXPECT_TRUE(state_sensor_test_alloc::exerciseAllBadAlloc(
        [&] {
            auto copy = sensor.getAssociationEntityId();
            (void)copy;
        },
        2048));
}

TEST_F(StateSensorCoverage, stateSensorInlineTransitionMatrixCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{5, PLDM_ENTITY_SYS_BOARD, 14},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_transition_matrix"};

    StateSensor sensor(0x71, false, 0x7100, info, nullptr, associationPath,
                       nullptr);

    EXPECT_EQ(nullptr, sensor.getSensorEventInfo());
    EXPECT_TRUE(sensor.getAssociationEntityId().empty());
    EXPECT_TRUE(sensor.isDefaultInventoryAssociated());

    auto shortInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    shortInfo->impactedComponent = "CPU0";
    auto longInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    longInfo->impactedComponent = std::string(160, 'L');
    longInfo->eventIdsMap.emplace("Thermal",
                                  "ResourceEvent.1.0." + std::string(64, 'T'));

    sensor.updateSensorEventInfo(shortInfo);
    auto shortCopy1 = sensor.getSensorEventInfo();
    auto shortCopy2 = sensor.getSensorEventInfo();
    ASSERT_EQ(shortInfo, shortCopy1);
    ASSERT_EQ(shortCopy1, shortCopy2);

    sensor.updateSensorEventInfo(shortCopy1);
    EXPECT_EQ(shortInfo, sensor.getSensorEventInfo());

    sensor.updateSensorEventInfo(longInfo);
    auto longCopy1 = sensor.getSensorEventInfo();
    auto longCopy2 = sensor.getSensorEventInfo();
    ASSERT_EQ(longInfo, longCopy1);
    ASSERT_EQ(longCopy1, longCopy2);
    EXPECT_EQ(std::string(160, 'L'), longCopy1->impactedComponent);
    EXPECT_EQ("ResourceEvent.1.0." + std::string(64, 'T'),
              longCopy1->eventIdsMap.at("Thermal"));

    sensor.updateSensorEventInfo(nullptr);
    EXPECT_EQ(nullptr, sensor.getSensorEventInfo());
    sensor.updateSensorEventInfo(longInfo);
    EXPECT_EQ(longInfo, sensor.getSensorEventInfo());

    sensor.setInventoryPaths({associationPath + "/cpu0"}, false);
    EXPECT_EQ("cpu0", sensor.getAssociationEntityId());

    sensor.setInventoryPaths({associationPath + "/" + std::string(160, 'x')},
                             false);
    EXPECT_EQ(std::string(160, 'x'), sensor.getAssociationEntityId());
    EXPECT_FALSE(sensor.isDefaultInventoryAssociated());

    sensor.setInventoryPaths({}, true);
    EXPECT_EQ(std::string(160, 'x'), sensor.getAssociationEntityId());
    EXPECT_TRUE(sensor.isDefaultInventoryAssociated());

    sensor.setRefreshed(false);
    EXPECT_FALSE(sensor.isRefreshed());
    sensor.setRefreshed(true);
    EXPECT_TRUE(sensor.isRefreshed());

    sensor.refreshLimitInUsec = 250;
    sensor.setLastUpdatedTimeStamp(1000);
    EXPECT_FALSE(sensor.needsUpdate(1100));
    EXPECT_FALSE(sensor.needsUpdate(1250));
    EXPECT_TRUE(sensor.needsUpdate(1251));
}

TEST_F(StateSensorCoverage, stateSensorInlineRuntimeGetterCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{6, PLDM_ENTITY_SYS_BOARD, 15},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_runtime_getter"};

    StateSensor sensor(0x72, false, 0x7200, info, nullptr, associationPath,
                       nullptr);

    auto getAssoc = &StateSensor::getAssociationEntityId;
    auto updateInfo = &StateSensor::updateSensorEventInfo;
    auto getInfo = &StateSensor::getSensorEventInfo;
    auto setRefreshed = &StateSensor::setRefreshed;
    auto isRefreshed = &StateSensor::isRefreshed;
    auto setTimestamp = &StateSensor::setLastUpdatedTimeStamp;
    auto needsUpdate = &StateSensor::needsUpdate;

    auto shortInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    shortInfo->impactedComponent = "CPU72";
    auto longInfo = std::make_shared<pldm::utils::SensorEventInfo>();
    longInfo->impactedComponent = std::string(192, 'R');
    longInfo->eventIdsMap.emplace("Thermal",
                                  "ResourceEvent.1.0." + std::string(80, 'S'));

    const std::array<std::shared_ptr<pldm::utils::SensorEventInfo>, 4> infos{
        shortInfo, nullptr, longInfo, shortInfo};
    for (const auto& infoPtr : infos)
    {
        (sensor.*updateInfo)(infoPtr);
        auto copy = (sensor.*getInfo)();
        EXPECT_EQ(infoPtr, copy);
    }

    const std::vector<std::pair<std::vector<std::string>, bool>> inventoryCases{
        {{associationPath + "/cpu0"}, false},
        {{associationPath + "/" + std::string(176, 'x')}, false},
        {{}, true}};
    for (const auto& [paths, defaultInventory] : inventoryCases)
    {
        sensor.setInventoryPaths(paths, defaultInventory);
        EXPECT_FALSE((sensor.*getAssoc)().empty());
    }
    EXPECT_TRUE(sensor.isDefaultInventoryAssociated());

    const std::array<bool, 3> refreshedStates{false, true, false};
    for (const auto refreshed : refreshedStates)
    {
        (sensor.*setRefreshed)(refreshed);
        EXPECT_EQ(refreshed, (sensor.*isRefreshed)());
    }

    sensor.refreshLimitInUsec = 250;
    (sensor.*setTimestamp)(1000);
    const std::array<uint64_t, 3> currentTimes{1100, 1250, 1251};
    const std::array<bool, 3> expectedNeedsUpdate{false, false, true};
    for (size_t idx = 0; idx < currentTimes.size(); ++idx)
    {
        EXPECT_EQ(expectedNeedsUpdate[idx],
                  (sensor.*needsUpdate)(currentTimes[idx]));
    }
}

TEST_F(StateSensorCoverage, stateSensorInlineSharedPtrOwnershipMatrixCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{7, PLDM_ENTITY_SYS_BOARD, 16},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_shared_ptr_matrix"};

    StateSensor sensor(0x73, false, 0x7300, info, nullptr, associationPath,
                       nullptr);

    auto makeInfo = [](char fill, std::size_t size) {
        auto value = std::make_shared<pldm::utils::SensorEventInfo>();
        value->impactedComponent = std::string(size, fill);
        value->eventIdsMap.emplace(
            "Event", "ResourceEvent.1.0." + std::string(size / 2 + 1, fill));
        return value;
    };

    auto shared = makeInfo('S', 48);
    auto copied = shared;
    auto aliasOwner = makeInfo('A', 64);
    std::shared_ptr<pldm::utils::SensorEventInfo> alias(aliasOwner,
                                                        aliasOwner.get());
    auto customDeleter = std::shared_ptr<pldm::utils::SensorEventInfo>(
        new pldm::utils::SensorEventInfo{}, [](auto* value) { delete value; });
    customDeleter->impactedComponent = std::string(80, 'C');
    customDeleter->eventIdsMap.emplace(
        "Custom", "ResourceEvent.1.0." + std::string(24, 'C'));
    auto uniqueOwned = std::make_unique<pldm::utils::SensorEventInfo>();
    uniqueOwned->impactedComponent = std::string(96, 'U');
    uniqueOwned->eventIdsMap.emplace(
        "Unique", "ResourceEvent.1.0." + std::string(20, 'U'));
    std::shared_ptr<pldm::utils::SensorEventInfo> fromUnique(
        std::move(uniqueOwned));

    const std::vector<std::shared_ptr<pldm::utils::SensorEventInfo>> variants{
        nullptr, shared, copied, alias, customDeleter, fromUnique};
    for (const auto& value : variants)
    {
        sensor.updateSensorEventInfo(value);
        auto firstCopy = sensor.getSensorEventInfo();
        auto secondCopy = sensor.getSensorEventInfo();
        EXPECT_EQ(value.get(), firstCopy.get());
        EXPECT_EQ(value.get(), secondCopy.get());
        if (value)
        {
            EXPECT_FALSE(firstCopy->impactedComponent.empty());
            EXPECT_FALSE(secondCopy->eventIdsMap.empty());
        }
    }
}

TEST_F(StateSensorCoverage,
       stateSensorInlineAssociationIdBoundaryMatrixCoverage)
{
    PossibleStates possibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL,
                                  PLDM_STATESET_HEALTH_STATE_CRITICAL};
    StateSetInfo info =
        std::make_tuple(EntityInfo{8, PLDM_ENTITY_SYS_BOARD, 17},
                        std::vector<StateSetData>{
                            {PLDM_STATESET_ID_HEALTHSTATE, possibleStates}});
    std::string associationPath{
        "/xyz/openbmc_project/inventory/system/chassis/chassis_assoc_boundary"};

    StateSensor sensor(0x74, false, 0x7400, info, nullptr, associationPath,
                       nullptr);
    auto getAssoc = &StateSensor::getAssociationEntityId;

    const std::vector<std::string> suffixes{
        "a",
        std::string(15, 'b'),
        std::string(16, 'c'),
        std::string(31, 'd'),
        std::string(32, 'e'),
        std::string(96, 'f')};
    for (const auto& suffix : suffixes)
    {
        auto inventoryPath = associationPath;
        inventoryPath.push_back('/');
        inventoryPath.append(suffix);
        sensor.setInventoryPaths({std::move(inventoryPath)}, false);
        auto assocId = (sensor.*getAssoc)();
        EXPECT_EQ(assocId, suffix);
        EXPECT_FALSE(sensor.isDefaultInventoryAssociated());
    }

    sensor.setInventoryPaths({associationPath + "/default"}, true);
    EXPECT_TRUE(sensor.isDefaultInventoryAssociated());
    EXPECT_FALSE((sensor.*getAssoc)().empty());

    sensor.setInventoryPaths({"not a valid object path"}, false);
    EXPECT_TRUE((sensor.*getAssoc)().empty());

    const std::string longSuffix(160, 'z');
    sensor.setInventoryPaths({associationPath + "/" + longSuffix}, false);
    EXPECT_TRUE(state_sensor_test_alloc::exerciseAllBadAlloc(
        [&] {
            auto copy = (sensor.*getAssoc)();
            (void)copy;
        },
        2048));
}
