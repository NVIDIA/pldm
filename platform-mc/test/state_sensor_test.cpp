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

#include "common/instance_id.hpp"
#include "oem/nvidia/platform-mc/remoteDebug.hpp"
#include "oem/nvidia/platform-mc/state_set/memoryPerformance.hpp"
#include "oem/nvidia/platform-mc/state_set/memorySpareChannel.hpp"
#include "oem/nvidia/platform-mc/state_set/nvlink.hpp"
#include "oem/nvidia/platform-mc/state_set/processorPowerBreak.hpp"
#include "platform-mc/state_set.hpp"
#include "platform-mc/state_set/ethIBPortLinkState.hpp"
#include "platform-mc/state_set/healthState.hpp"
#include "platform-mc/state_set/pciePortLinkState.hpp"
#include "platform-mc/state_set/performance.hpp"
#include "platform-mc/state_set/powerSupplyInput.hpp"
#include "platform-mc/terminus.hpp"
#include "platform-mc/terminus_manager.hpp"
#include "test/test_instance_id.hpp"

#include <sdeventplus/event.hpp>

#include <chrono>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

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

    sdbusplus::bus::bus& bus;
    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    requester::Handler<requester::Request> reqHandler;
    TerminusManager terminusManager;
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
};

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
        sensor->updateReading(true, true, 0, 0xFF);
        auto [msgUnknown, argUnknown, levelUnknown, eventUnknown,
              impactedUnknown] = stateSet->getEventData(nullptr);
        EXPECT_EQ("Unknown", argUnknown);
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

        sensor->updateReading(true, true, 0, 0xFF);
        auto [msgUnknown, argUnknown, levelUnknown, eventUnknown,
              impactedUnknown] = stateSet->getEventData(nullptr);
        EXPECT_EQ("Unknown", argUnknown);

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
