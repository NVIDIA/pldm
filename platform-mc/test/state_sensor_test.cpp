#include "libpldm/entity.h"

#include "oem/nvidia/platform-mc/state_set/memorySpareChannel.hpp"
#include "platform-mc/state_sensor.hpp"
#include "platform-mc/state_set.hpp"
#include "platform-mc/terminus.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldm::platform_mc;
using namespace pldm;

TEST(TestOemStateSensor, memorySpareChannelPresence)
{
    uint16_t sensorId = 1;
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       *(static_cast<TerminusManager*>(nullptr)));
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                   // record handle
        0x1,                   // PDRHeaderVersion
        PLDM_STATE_SENSOR_PDR, // PDRType
        0x0,
        0x0, // recordChangeNumber
        0x0,
        0x11, // dataLength
        0,
        0, // PLDMTerminusHandle
        static_cast<uint8_t>(sensorId & 0xFF),
        static_cast<uint8_t>((sensorId >> 8) & 0xFF),
        PLDM_ENTITY_MEMORY_CONTROLLER,
        0, // entityType=Memory controller (143)
        1,
        0, // entityInstanceNumber
        0x1,
        0x0,          // containerID=1
        PLDM_NO_INIT, // sensorInit
        false,        // sensorAuxiliaryNamesPDR
        1,            // compositeSensorCount
        static_cast<uint8_t>(PLDM_STATESET_ID_PRESENCE & 0xFF),
        static_cast<uint8_t>((PLDM_STATESET_ID_PRESENCE >> 8) &
                             0xFF), // stateSetID (13)
        0x1,                        // possibleStatesSize
        0x3                         // possibleStates
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
        true,
        stateSetMemorySpareChannel->ValueIntf->memorySpareChannelPresence());

    // Should be false for PLDM_STATESET_PRESENCE_NOT_PRESENT
    stateSensor->updateReading(true, true, 0,
                               PLDM_STATESET_PRESENCE_NOT_PRESENT);
    EXPECT_EQ(
        false,
        stateSetMemorySpareChannel->ValueIntf->memorySpareChannelPresence());

    // Should be false for invalid state set
    stateSensor->updateReading(true, true, 0, 0);
    EXPECT_EQ(
        false,
        stateSetMemorySpareChannel->ValueIntf->memorySpareChannelPresence());
}
