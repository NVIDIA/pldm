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

#include "oem/nvidia/platform-mc/state_set/memorySpareChannel.hpp"
#include "platform-mc/state_sensor.hpp"
#include "platform-mc/state_set.hpp"
#include "platform-mc/terminus.hpp"
#include "platform-mc/terminus_manager.hpp"

#include <sdeventplus/event.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldm::platform_mc;
using namespace pldm;

TEST(TestOemStateSensor, memorySpareChannelPresence)
{
    uint16_t sensorId = 1;
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    sdbusplus::bus::bus& bus(pldm::utils::DBusHandler::getBus());
    sdeventplus::Event event(sdeventplus::Event::get_default());
    dbus_api::Requester dbusImplRequester(bus, "/xyz/openbmc_project/pldm");
    mctp_socket::Manager sockManager;
    requester::Handler<requester::Request> reqHandler(event, dbusImplRequester,
                                                      sockManager, false);
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
    TerminusManager terminusManager(event, reqHandler, dbusImplRequester,
                                    termini, 0x8, nullptr);
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
