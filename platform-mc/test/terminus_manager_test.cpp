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

#include "mock_terminus_manager.hpp"
#include "platform-mc/terminus_manager.hpp"
#include "pldmd/dbus_impl_requester.hpp"
#include "requester/handler.hpp"
#include "requester/mctp_endpoint_discovery.hpp"
#include "requester/request.hpp"

#include <sdbusplus/timer.hpp>
#include <sdeventplus/event.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace std::chrono;
using namespace pldm;

using ::testing::AtLeast;
using ::testing::Between;
using ::testing::Exactly;
using ::testing::NiceMock;
using ::testing::Return;

const uint8_t mockTerminusManagerLocalEid = 0x08;

class TerminusManagerTest : public testing::Test
{
  protected:
    TerminusManagerTest() :
        bus(pldm::utils::DBusHandler::getBus()),
        event(sdeventplus::Event::get_default()),
        dbusImplRequester(bus, "/xyz/openbmc_project/pldm"),
        reqHandler(event, dbusImplRequester, sockManager, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, dbusImplRequester, termini,
                        mockTerminusManagerLocalEid, nullptr),
        mockTerminusManager(event, reqHandler, dbusImplRequester, termini,
                            mockTerminusManagerLocalEid, nullptr)
    {}

    sdbusplus::bus::bus& bus;
    sdeventplus::Event event;
    pldm::dbus_api::Requester dbusImplRequester;
    pldm::mctp_socket::Manager sockManager;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    pldm::platform_mc::TerminusManager terminusManager;
    pldm::platform_mc::MockTerminusManager mockTerminusManager;
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
};

TEST_F(TerminusManagerTest, mapTidTest)
{
    pldm::MctpInfo mctpInfo1(
        1, "f72d6f90-5675-11ed-9b6a-0242ac120002",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 0,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe");

    // look up a unmapped mctpInfo, returned tid should be null
    auto tid1 = terminusManager.toTid(mctpInfo1);
    EXPECT_EQ(tid1, std::nullopt);

    // assign tid to mctpInfo, the returned tid should not be null
    tid1 = terminusManager.mapTid(mctpInfo1);
    EXPECT_NE(tid1, std::nullopt);

    // look up mapped mctpInfo for tid, return mctpInfo should matched faster
    // mctpInfo
    auto mctpInfo2 = terminusManager.toMctpInfo(tid1.value());
    EXPECT_EQ(mctpInfo1, mctpInfo2.value());

    // unmap tid and then look up the unmapped mctpInfo, return tid should be
    // null
    terminusManager.unmapTid(tid1.value());

    tid1 = terminusManager.toTid(mctpInfo1);
    EXPECT_EQ(tid1, std::nullopt);
}

TEST_F(TerminusManagerTest, preferredMediumAndBindingTest)
{
    pldm::MctpInfo mctpInfo1(
        1, "f72d6f90-5675-11ed-9b6a-0242ac120002",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus", 0,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus");
    pldm::MctpInfo mctpInfo1_Faster(
        2, "f72d6f90-5675-11ed-9b6a-0242ac120002",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 0,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe");
    pldm::MctpInfo mctpInfo1_Slower(
        3, "f72d6f90-5675-11ed-9b6a-0242ac120002",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.Serial", 0,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.Serial");
    pldm::MctpInfo mctpInfo1_SameMediumSlowerBinding(
        3, "f72d6f90-5675-11ed-9b6a-0242ac120002",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 0,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.Serial");

    // assign tid to mctpInfo, the returned tid should not be null
    auto tid1 = terminusManager.mapTid(mctpInfo1);
    EXPECT_NE(tid1, std::nullopt);

    // assign a tid for another mctpInfo with the same UUID but faster medium,
    // return tid should be the same
    auto tid2 = terminusManager.mapTid(mctpInfo1_Faster);
    EXPECT_EQ(tid2, tid1);

    // assign a tid for another mctpInfo with the same UUID but slower medium,
    // return tid should be null
    tid2 = terminusManager.mapTid(mctpInfo1_Slower);
    EXPECT_EQ(tid2, std::nullopt);

    // assign a tip for another mctpInfo with the same UUID but slower binding,
    // return tid should be null
    tid2 = terminusManager.mapTid(mctpInfo1_SameMediumSlowerBinding);
    EXPECT_EQ(tid2, std::nullopt);

    // look up mapped mctpInfo for tid, return mctpInfo should be matched to
    // faster mctpInfo
    auto mctpInfo2 = terminusManager.toMctpInfo(tid1.value());
    EXPECT_NE(mctpInfo1, mctpInfo2.value());
    EXPECT_EQ(mctpInfo1_Faster, mctpInfo2.value());
}

TEST_F(TerminusManagerTest, negativeMapTidTest)
{
    // map null EID(0) to TID
    pldm::MctpInfo m0(0, "", "", 0, "");
    auto mappedTid = terminusManager.mapTid(m0);
    EXPECT_EQ(mappedTid, std::nullopt);

    // map broadcast EID(0xff) to TID
    pldm::MctpInfo m1(0xff, "", "", 0, "");
    mappedTid = terminusManager.mapTid(m1);
    EXPECT_EQ(mappedTid, std::nullopt);

    // look up an unmapped MctpInfo to TID
    pldm::MctpInfo m2(1, "", "", 0, "");
    mappedTid = terminusManager.toTid(m2);
    EXPECT_EQ(mappedTid, std::nullopt);

    // look up reserved TID(0)
    auto mappedEid = terminusManager.toMctpInfo(0);
    EXPECT_EQ(mappedEid, std::nullopt);

    // look up reserved TID(0xff)
    mappedEid = terminusManager.toMctpInfo(0xff);
    EXPECT_EQ(mappedEid, std::nullopt);

    // look up an unmapped TID
    mappedEid = terminusManager.toMctpInfo(1);
    EXPECT_EQ(mappedEid, std::nullopt);
    /* disabled the test case since this is an invald test case on nvidia
       platforms.
        // map two mctpInfo with same EID but different UUID and network Id
        pldm::MctpInfo m3(12, "f72d6f90-5675-11ed-9b6a-0242ac120002",
                          "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
       1,
                          "");
        pldm::MctpInfo m4(12, "f72d6f90-5675-11ed-9b6a-0242ac120012",
                          "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
       2,
                          "");

        auto mappedTid3 = terminusManager.mapTid(m3);
        auto mappedTid4 = terminusManager.mapTid(m4);
        EXPECT_NE(mappedTid3.value(), mappedTid4.value());
    */
    // map same mctpInfo twice
    pldm::MctpInfo m5(13, "f72d6f90-5675-11ed-9b6a-0242ac120013",
                      "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 3,
                      "");
    auto mappedTid5 = terminusManager.mapTid(m5);
    auto mappedTid6 = terminusManager.mapTid(m5);
    EXPECT_EQ(mappedTid5.value(), mappedTid6.value());
}

TEST_F(TerminusManagerTest, getLocalEidTest)
{
    auto localEid = terminusManager.getLocalEid();
    EXPECT_EQ(localEid, mockTerminusManagerLocalEid);
}

TEST_F(TerminusManagerTest, discoverMctpTerminusTest)
{
    const size_t getTidRespLen = 2;
    const size_t setTidRespLen = 1;
    const size_t getPldmTypesRespLen = 9;

    // 0.discover a mctp list
    auto rc = mockTerminusManager.clearQueuedResponses();
    EXPECT_EQ(rc, PLDM_SUCCESS);

    std::array<uint8_t, sizeof(pldm_msg_hdr) + getTidRespLen> getTidResp0{
        0x00, PLDM_BASE, PLDM_GET_TID, 0x00, 0x00};
    rc = mockTerminusManager.enqueueResponse((pldm_msg*)getTidResp0.data(),
                                             sizeof(getTidResp0));
    EXPECT_EQ(rc, PLDM_SUCCESS);
    std::array<uint8_t, sizeof(pldm_msg_hdr) + setTidRespLen> setTidResp0{
        0x00, PLDM_BASE, PLDM_SET_TID, 0x00};
    rc = mockTerminusManager.enqueueResponse((pldm_msg*)setTidResp0.data(),
                                             sizeof(setTidResp0));
    EXPECT_EQ(rc, PLDM_SUCCESS);
    std::array<uint8_t, sizeof(pldm_msg_hdr) + getPldmTypesRespLen>
        getPldmTypesResp0{0x00, PLDM_BASE, PLDM_GET_PLDM_TYPES,
                          0x00, 0x01,      0x00,
                          0x00, 0x00,      0x00,
                          0x00, 0x00,      0x00};
    rc = mockTerminusManager.enqueueResponse(
        (pldm_msg*)getPldmTypesResp0.data(), sizeof(getPldmTypesResp0));
    EXPECT_EQ(rc, PLDM_SUCCESS);

    pldm::MctpInfos mctpInfos{};
    mctpInfos.emplace_back(pldm::MctpInfo(12, "", "", 1, ""));
    mockTerminusManager.discoverMctpTerminus(mctpInfos);
    EXPECT_EQ(1, termini.size());

    // 1.discover the same mctp list again
    rc = mockTerminusManager.clearQueuedResponses();
    EXPECT_EQ(rc, PLDM_SUCCESS);

    std::array<uint8_t, sizeof(pldm_msg_hdr) + getTidRespLen> getTidResp1{
        0x00, PLDM_BASE, PLDM_GET_TID, 0x00, 0x01};
    rc = mockTerminusManager.enqueueResponse((pldm_msg*)getTidResp1.data(),
                                             sizeof(getTidResp1));
    EXPECT_EQ(rc, PLDM_SUCCESS);
    rc = mockTerminusManager.enqueueResponse((pldm_msg*)setTidResp0.data(),
                                             sizeof(setTidResp0));
    EXPECT_EQ(rc, PLDM_SUCCESS);
    rc = mockTerminusManager.enqueueResponse(
        (pldm_msg*)getPldmTypesResp0.data(), sizeof(getPldmTypesResp0));
    EXPECT_EQ(rc, PLDM_SUCCESS);

    mockTerminusManager.discoverMctpTerminus(mctpInfos);
    EXPECT_EQ(1, termini.size());
}

TEST_F(TerminusManagerTest, negativeDiscoverMctpTerminusTest)
{
    const size_t getTidRespLen = 2;
    const size_t setTidRespLen = 1;
    const size_t getPldmTypesRespLen = 9;

    // 0.terminus returns reserved tid
    std::array<uint8_t, sizeof(pldm_msg_hdr) + getTidRespLen> getTidResp0{
        0x00, PLDM_BASE, PLDM_GET_TID, 0x00, PLDM_TID_RESERVED};
    auto rc = mockTerminusManager.enqueueResponse((pldm_msg*)getTidResp0.data(),
                                                  sizeof(getTidResp0));
    EXPECT_EQ(rc, PLDM_SUCCESS);

    pldm::MctpInfos mctpInfos{};
    mctpInfos.emplace_back(pldm::MctpInfo(12, "", "", 1, ""));
    mockTerminusManager.discoverMctpTerminus(mctpInfos);
    EXPECT_EQ(0, termini.size());

    // 1.terminus return cc=pldm_error for set tid
    std::array<uint8_t, sizeof(pldm_msg_hdr) + getTidRespLen> getTidResp1{
        0x00, PLDM_BASE, PLDM_GET_TID, 0x00, 0x00};
    std::array<uint8_t, sizeof(pldm_msg_hdr) + setTidRespLen> setTidResp1{
        0x00, PLDM_BASE, PLDM_SET_TID, PLDM_ERROR};

    rc = mockTerminusManager.enqueueResponse((pldm_msg*)getTidResp1.data(),
                                             sizeof(getTidResp1));
    EXPECT_EQ(rc, PLDM_SUCCESS);
    rc = mockTerminusManager.enqueueResponse((pldm_msg*)setTidResp1.data(),
                                             sizeof(setTidResp1));
    EXPECT_EQ(rc, PLDM_SUCCESS);
    mockTerminusManager.discoverMctpTerminus(mctpInfos);
    EXPECT_EQ(0, termini.size());

    // 2.terminus return cc=unsupported_pldm_cmd for set tid cmd and return
    // cc=pldm_error for get pldm types cmd
    std::array<uint8_t, sizeof(pldm_msg_hdr) + getTidRespLen> getTidResp2{
        0x00, PLDM_BASE, PLDM_GET_TID, 0x00, 0x00};
    std::array<uint8_t, sizeof(pldm_msg_hdr) + setTidRespLen> setTidResp2{
        0x00, PLDM_BASE, PLDM_SET_TID, PLDM_ERROR_UNSUPPORTED_PLDM_CMD};
    std::array<uint8_t, sizeof(pldm_msg_hdr) + getPldmTypesRespLen>
        getPldmTypesResp2{0x00,       PLDM_BASE, PLDM_GET_PLDM_TYPES,
                          PLDM_ERROR, 0x01,      0x00,
                          0x00,       0x00,      0x00,
                          0x00,       0x00,      0x00};
    rc = mockTerminusManager.enqueueResponse((pldm_msg*)getTidResp2.data(),
                                             sizeof(getTidResp2));
    EXPECT_EQ(rc, PLDM_SUCCESS);
    rc = mockTerminusManager.enqueueResponse((pldm_msg*)setTidResp2.data(),
                                             sizeof(setTidResp2));
    EXPECT_EQ(rc, PLDM_SUCCESS);

    rc = mockTerminusManager.enqueueResponse(
        (pldm_msg*)getPldmTypesResp2.data(), sizeof(getPldmTypesResp2));
    EXPECT_EQ(rc, PLDM_SUCCESS);
    mockTerminusManager.discoverMctpTerminus(mctpInfos);
    EXPECT_EQ(0, termini.size());
}
