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

#include "platform-mc/numeric_effecter.hpp"
#include "platform-mc/terminus.hpp"
#include "platform-mc/terminus_manager.hpp"

#include <gtest/gtest.h>

using namespace pldm::platform_mc;

class TestNumericEffecter : public ::testing::Test
{
  public:
    TestNumericEffecter() :
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

TEST_F(TestNumericEffecter, verifyNumericEffecterInventoryPath)
{
    uint16_t sensorId = 0x0801;
    std::string uuid1("00000000-0000-0000-0000-000000000001");
    auto t1 = Terminus(1, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid1,
                       terminusManager);
    std::vector<uint8_t> pdr1{
        0x0,
        0x0,
        0x0,
        0x1,                       // record handle
        0x1,                       // PDRHeaderVersion
        PLDM_NUMERIC_EFFECTER_PDR, // PDRType
        0x0,
        0x0, // recordChangeNumber
        0,
        54, // dataLength
        0,
        0, // PLDMTerminusHandle
        static_cast<uint8_t>(sensorId & 0xFF),
        static_cast<uint8_t>((sensorId >> 8) & 0xFF), // effecterID=0x0801
        PLDM_ENTITY_PROC_IO_MODULE,
        0, // entityType
        1,
        0, // entityInstanceNumber
        0x1,
        0x0, // containerID=1
        0x0,
        0x0,                         // effecterSematicID
        PLDM_NO_INIT,                // effecterInit
        false,                       // effecterAuxiliaryNames PDR
        PLDM_SENSOR_UNIT_WATTS,      // baseUnit
        0,                           // unitModifier
        0,                           // rateUnit
        0,                           // baseOEMUnitHandle
        0,                           // auxUnit
        0,                           // auxUnitModifier
        0,                           // auxrateUnit
        0,                           // auxOEMUnitHandle
        true,                        // isLinear
        PLDM_SENSOR_DATA_SIZE_UINT8, // effecterDataSize
        1,
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
        0,
        0,
        0,
        0, // stateTransitionInterval
        0,
        0,
        1,
        0,                             // TransitionInterval
        1,                             // maxSettable
        0,                             // minSettable
        PLDM_RANGE_FIELD_FORMAT_UINT8, // rangeFieldFormat
        0x3F,                          // rangeFieldSupport
        0,                             // nominalValue
        0,                             // normalMax
        0,                             // normalMin
        0,                             // ratedMax
        0                              // ratedMin
    };

    t1.pdrs.emplace_back(pdr1);
    auto rc = t1.parsePDRs();
    EXPECT_EQ(true, rc);
    EXPECT_EQ(1, t1.numericEffecterPdrs.size());
    EXPECT_EQ(1, t1.numericEffecters.size());

    auto numericEffecterPdr = t1.numericEffecterPdrs[0];

    std::string sensorName{"test1"};
    std::string inventoryPath{
        "/xyz/openbmc_project/inventroy/Item/Board/PLDM_device_1"};
    NumericEffecter sensor(0x01, true, numericEffecterPdr, sensorName,
                          inventoryPath, terminusManager);

    std::vector<std::string> paths{
        "/xyz/openbmc_project/inventory/system/board/cpu0"};

    sensor.setInventoryPaths(paths);

    auto assocs = sensor.getAssociation();
    EXPECT_EQ(1, assocs.size());
    for (auto& assoc : assocs)
    {
        auto& [forward, reverse, objectPath] = assoc;
        EXPECT_EQ("power_controls", reverse);
        EXPECT_EQ(paths[0], objectPath);
    }

    paths.emplace_back("/xyz/openbmc_project/inventory/system/board/hgx_cpu0");
    sensor.setInventoryPaths(paths);

    assocs = sensor.getAssociation();
    EXPECT_EQ(2, assocs.size());
    uint8_t counter = 0;
    for (auto& assoc : assocs)
    {
        auto& [forward, reverse, objectPath] = assoc;
        EXPECT_EQ("power_controls", reverse);
        for (auto& p: paths)
        {
            if (p == objectPath)
            {
                counter++;
            }
        }
    }
    EXPECT_EQ(counter, assocs.size());
}
