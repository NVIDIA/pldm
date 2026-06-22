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

#include "../../test/test_valgrind_utils.hpp"
#include "common/instance_id.hpp"
#include "mock_terminus_manager.hpp"
#include "requester/handler.hpp"
#include "requester/mctp_endpoint_discovery.hpp"
#include "requester/request.hpp"
#include "test/test_instance_id.hpp"

#include <sdbusplus/timer.hpp>
#include <sdeventplus/event.hpp>

#include <filesystem>
#include <fstream>
#include <thread>

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

static std::vector<uint8_t> makeGetTerminusUidResp(
    const std::array<uint8_t, 16>& uid, uint8_t completionCode = PLDM_SUCCESS)
{
    std::vector<uint8_t> response(
        sizeof(pldm_msg_hdr) + PLDM_GET_TERMINUS_UID_RESP_BYTES, 0);
    auto* responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    responseMsg->hdr.type = PLDM_PLATFORM;
    responseMsg->hdr.command = PLDM_GET_TERMINUS_UID;
    responseMsg->payload[0] = completionCode;
    memcpy(responseMsg->payload + 1, uid.data(), uid.size());
    return response;
}

static Request makeSimpleRequest(uint8_t instanceId)
{
    Request request(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
    auto* requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    requestMsg->hdr.instance_id = instanceId;
    requestMsg->hdr.request = 1;
    return request;
}

class TerminusManagerTest : public testing::Test
{
  protected:
    TerminusManagerTest() :
        bus(pldm::utils::DBusHandler::getBus()),
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, instanceIdDb, termini,
                        mockTerminusManagerLocalEid, nullptr),
        mockTerminusManager(event, reqHandler, instanceIdDb, termini,
                            mockTerminusManagerLocalEid, nullptr)
    {}

    sdbusplus::bus_t& bus;
    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    pldm::platform_mc::TerminusManager terminusManager;
    pldm::platform_mc::MockTerminusManager mockTerminusManager;
    std::map<pldm::tid_t, std::shared_ptr<pldm::platform_mc::Terminus>> termini;
};

TEST_F(TerminusManagerTest, mapTidTest)
{
    pldm::MctpInfo mctpInfo1(
        1, "f72d6f90-5675-11ed-9b6a-0242ac120002",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 0, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);

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
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus", 0, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus", std::nullopt);
    pldm::MctpInfo mctpInfo1_Faster(
        2, "f72d6f90-5675-11ed-9b6a-0242ac120002",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 0, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    pldm::MctpInfo mctpInfo1_Slower(
        3, "f72d6f90-5675-11ed-9b6a-0242ac120002",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.Serial", 0, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.Serial", std::nullopt);
    pldm::MctpInfo mctpInfo1_SameMediumSlowerBinding(
        3, "f72d6f90-5675-11ed-9b6a-0242ac120002",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 0, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.Serial", std::nullopt);

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

// mapTid must tolerate empty or unrecognized medium/binding strings without
// throwing; unknown values rank below any known medium/binding so an existing
// known mapping is preserved.
TEST_F(TerminusManagerTest, mapTidUnknownMediumOrBindingDoesNotThrow)
{
    pldm::MctpInfo known(
        1, "f72d6f90-5675-11ed-9b6a-0242ac120002",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 0, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    auto knownTid = terminusManager.mapTid(known);
    ASSERT_NE(knownTid, std::nullopt);

    pldm::MctpInfo emptyMediumBinding(2, "f72d6f90-5675-11ed-9b6a-0242ac120002",
                                      "", 0, std::nullopt, "", std::nullopt);
    EXPECT_NO_THROW({
        auto tid = terminusManager.mapTid(emptyMediumBinding);
        EXPECT_EQ(tid, std::nullopt);
    });

    pldm::MctpInfo unknownMedium(
        3, "f72d6f90-5675-11ed-9b6a-0242ac120002",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.NotARealMedium", 0,
        std::nullopt, "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe",
        std::nullopt);
    EXPECT_NO_THROW({
        auto tid = terminusManager.mapTid(unknownMedium);
        EXPECT_EQ(tid, std::nullopt);
    });

    pldm::MctpInfo unknownBinding(
        4, "f72d6f90-5675-11ed-9b6a-0242ac120002",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 0, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.NotARealBinding",
        std::nullopt);
    EXPECT_NO_THROW({
        auto tid = terminusManager.mapTid(unknownBinding);
        EXPECT_EQ(tid, std::nullopt);
    });
}

TEST_F(TerminusManagerTest, sameUuidWithEqualTransportIsNotPreferred)
{
    const pldm::MctpInfo existing(
        12, "f72d6f90-5675-11ed-9b6a-0242ac1200bb",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    const pldm::MctpInfo duplicateTransport(
        13, "f72d6f90-5675-11ed-9b6a-0242ac1200bb",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);

    auto tid = terminusManager.mapTid(existing);
    ASSERT_TRUE(tid.has_value());
    EXPECT_EQ(terminusManager.mapTid(duplicateTransport), std::nullopt);
    EXPECT_EQ(terminusManager.toMctpInfo(*tid), existing);
}

TEST_F(TerminusManagerTest, negativeMapTidTest)
{
    // map null EID(0) to TID
    pldm::MctpInfo m0(0, "", "", 0, std::nullopt, "", std::nullopt);
    auto mappedTid = terminusManager.mapTid(m0);
    EXPECT_EQ(mappedTid, std::nullopt);

    // map broadcast EID(0xff) to TID
    pldm::MctpInfo m1(0xff, "", "", 0, std::nullopt, "", std::nullopt);
    mappedTid = terminusManager.mapTid(m1);
    EXPECT_EQ(mappedTid, std::nullopt);

    // look up an unmapped MctpInfo to TID
    pldm::MctpInfo m2(1, "", "", 0, std::nullopt, "", std::nullopt);
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
                      std::nullopt, "", std::nullopt);
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
    mctpInfos.emplace_back(
        pldm::MctpInfo(12, "", "", 1, std::nullopt, "", std::nullopt));
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
    mctpInfos.emplace_back(
        pldm::MctpInfo(12, "", "", 1, std::nullopt, "", std::nullopt));
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

TEST_F(TerminusManagerTest, staticConfigAndResumeCoverage)
{
    namespace fs = std::filesystem;
    auto tmpRoot = fs::temp_directory_path() / "pldm_terminus_manager_tests";
    fs::create_directories(tmpRoot);

    const auto missingCfg = (tmpRoot / "missing.json").string();
    EXPECT_NO_THROW(terminusManager.loadStaticTerminusConfig(missingCfg));

    const auto invalidCfg = tmpRoot / "invalid.json";
    {
        std::ofstream out(invalidCfg);
        out << "{ this is not valid json }";
    }
    EXPECT_NO_THROW(
        terminusManager.loadStaticTerminusConfig(invalidCfg.string()));

    const auto validCfg = tmpRoot / "valid.json";
    {
        std::ofstream out(validCfg);
        out << R"({
  "PLDMTermini": [
    {"EID": 12, "Name": "CPU0", "TerminusName": "ProcessorModule_0", "Instance": 0},
    {"EID": 255, "Name": "", "TerminusName": "", "Instance": -1}
  ]
})";
    }
    EXPECT_NO_THROW(
        terminusManager.loadStaticTerminusConfig(validCfg.string()));

    auto resumeUnmappedRc =
        stdexec::sync_wait(mockTerminusManager.resumeTid(0x44));
    ASSERT_TRUE(resumeUnmappedRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*resumeUnmappedRc));

    const pldm::MctpInfo mctpInfo(
        12, "f72d6f90-5675-11ed-9b6a-0242ac1200aa",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    ASSERT_TRUE(mockTerminusManager.mapTid(mctpInfo, 0x22).has_value());

    std::vector<uint8_t> setTidResp{0x00, PLDM_BASE, PLDM_SET_TID,
                                    PLDM_SUCCESS};
    ASSERT_EQ(PLDM_SUCCESS, mockTerminusManager.enqueueResponse(setTidResp));

    auto resumeMappedRc =
        stdexec::sync_wait(mockTerminusManager.resumeTid(0x22));
    ASSERT_TRUE(resumeMappedRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*resumeMappedRc));

    fs::remove(validCfg);
    fs::remove(invalidCfg);
}

TEST_F(TerminusManagerTest, discoverPlatformTerminusUidCoverage)
{
    const size_t getTidRespLen = 2;
    const size_t setTidRespLen = 1;
    const size_t getPldmTypesRespLen = 9;

    ASSERT_EQ(PLDM_SUCCESS, mockTerminusManager.clearQueuedResponses());

    std::array<uint8_t, sizeof(pldm_msg_hdr) + getTidRespLen> getTidResp{
        0x00, PLDM_BASE, PLDM_GET_TID, 0x00, 0x00};
    ASSERT_EQ(PLDM_SUCCESS,
              mockTerminusManager.enqueueResponse((pldm_msg*)getTidResp.data(),
                                                  sizeof(getTidResp)));

    std::array<uint8_t, sizeof(pldm_msg_hdr) + setTidRespLen> setTidResp{
        0x00, PLDM_BASE, PLDM_SET_TID, PLDM_SUCCESS};
    ASSERT_EQ(PLDM_SUCCESS,
              mockTerminusManager.enqueueResponse((pldm_msg*)setTidResp.data(),
                                                  sizeof(setTidResp)));

    std::array<uint8_t, sizeof(pldm_msg_hdr) + getPldmTypesRespLen>
        getPldmTypesResp{
            0x00,
            PLDM_BASE,
            PLDM_GET_PLDM_TYPES,
            0x00,
            static_cast<uint8_t>((1 << PLDM_BASE) | (1 << PLDM_PLATFORM)),
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00};
    ASSERT_EQ(PLDM_SUCCESS, mockTerminusManager.enqueueResponse(
                                (pldm_msg*)getPldmTypesResp.data(),
                                sizeof(getPldmTypesResp)));

    const std::array<uint8_t, 16> uid{0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC,
                                      0xDE, 0xF0, 0x11, 0x22, 0x33, 0x44,
                                      0x55, 0x66, 0x77, 0x88};
    auto uidResp = makeGetTerminusUidResp(uid);
    ASSERT_EQ(PLDM_SUCCESS, mockTerminusManager.enqueueResponse(uidResp));

    pldm::MctpInfos mctpInfos{};
    mctpInfos.emplace_back(pldm::MctpInfo(
        12, "00000000-0000-0000-0000-000000000000",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt));
    mockTerminusManager.discoverMctpTerminus(mctpInfos);

    ASSERT_EQ(1u, termini.size());
    auto terminus = termini.begin()->second;
    ASSERT_NE(nullptr, terminus);
    EXPECT_TRUE(terminus->doesSupport(PLDM_PLATFORM));
    EXPECT_EQ("12345678-9abc-def0-1122-334455667788", terminus->getUuid());
    EXPECT_NE(nullptr, mockTerminusManager.getTerminus(
                           "12345678-9abc-def0-1122-334455667788"));
}

TEST_F(TerminusManagerTest, sendRecvAndResumeEdgeCoverage)
{
    Request request(sizeof(pldm_msg_hdr));
    const pldm_msg* responseMsg = nullptr;
    size_t responseLen = 0;

    auto unmappedRc = stdexec::sync_wait(mockTerminusManager.SendRecvPldmMsg(
        0x77, request, &responseMsg, &responseLen));
    ASSERT_TRUE(unmappedRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*unmappedRc));

    const pldm::MctpInfo mctpInfo(
        26, "f72d6f90-5675-11ed-9b6a-0242ac120026",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    ASSERT_TRUE(mockTerminusManager.mapTid(mctpInfo, 0x26).has_value());

    auto noResponseRc = stdexec::sync_wait(mockTerminusManager.SendRecvPldmMsg(
        0x26, request, &responseMsg, &responseLen));
    ASSERT_TRUE(noResponseRc.has_value());
    EXPECT_EQ(PLDM_ERROR, std::get<0>(*noResponseRc));

    std::vector<uint8_t> shortSetTidResp{0x00, PLDM_BASE, PLDM_SET_TID,
                                         PLDM_SUCCESS, 0x00};
    ASSERT_EQ(PLDM_SUCCESS,
              mockTerminusManager.enqueueResponse(shortSetTidResp));
    auto shortResumeRc =
        stdexec::sync_wait(mockTerminusManager.resumeTid(0x26));
    ASSERT_TRUE(shortResumeRc.has_value());
    EXPECT_EQ(PLDM_ERROR_INVALID_LENGTH, std::get<0>(*shortResumeRc));
}

TEST_F(TerminusManagerTest, sendRecvPldmMsgOverMctpSuccessCoverage)
{
    if (pldm::test::runningOnValgrind())
    {
        GTEST_SKIP() << "covered by the normal pass; valgrind blocks this "
                        "stdexec response path";
    }

    constexpr mctp_eid_t eid = 0x41;
    const auto instanceId = instanceIdDb.next(eid).value();
    auto request = makeSimpleRequest(instanceId);

    const pldm_msg* responseMsg = nullptr;
    size_t responseLen = 0;

    Response response(sizeof(pldm_msg_hdr) + sizeof(uint8_t), 0);
    const auto* responsePtr =
        reinterpret_cast<const pldm_msg*>(response.data());

    std::thread responder([&]() {
        std::this_thread::sleep_for(milliseconds(20));
        reqHandler.handleResponse(eid, instanceId, 0, 0, responsePtr,
                                  response.size());
    });

    auto rc = stdexec::sync_wait(terminusManager.SendRecvPldmMsgOverMctp(
        eid, request, &responseMsg, &responseLen));
    responder.join();

    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
    EXPECT_EQ(responsePtr, responseMsg);
    EXPECT_EQ(response.size(), responseLen);
}

TEST_F(TerminusManagerTest, sendRecvPldmMsgOverMctpTimeoutSuppressionCoverage)
{
    if (pldm::test::runningOnValgrind())
    {
        GTEST_SKIP() << "covered by the normal pass; valgrind delays this "
                        "timeout injection path";
    }

    constexpr mctp_eid_t eid = 0x42;

    auto runTimeout = [&](uint8_t instanceId) {
        auto request = makeSimpleRequest(instanceId);
        const pldm_msg* responseMsg = reinterpret_cast<const pldm_msg*>(0x1);
        size_t responseLen = 1;

        std::thread expiryInjector([&]() {
            std::this_thread::sleep_for(milliseconds(20));
            reqHandler.instanceIdExpiryCallBack({eid, instanceId, 0, 0});
        });

        auto rc = stdexec::sync_wait(terminusManager.SendRecvPldmMsgOverMctp(
            eid, request, &responseMsg, &responseLen));
        expiryInjector.join();

        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(PLDM_ERROR_NOT_READY, std::get<0>(*rc));
        EXPECT_EQ(nullptr, responseMsg);
        EXPECT_EQ(0u, responseLen);
    };

    runTimeout(instanceIdDb.next(eid).value());
    runTimeout(instanceIdDb.next(eid).value());
}

TEST_F(TerminusManagerTest, sendRecvPldmMsgOverMctpCancellationCoverage)
{
    constexpr mctp_eid_t eid = 0x43;
    const auto instanceId = instanceIdDb.next(eid).value();

    exec::async_scope scope;
    const pldm_msg* responseMsg = nullptr;
    size_t responseLen = 0;
    bool stopped = false;

    scope.spawn(stdexec::just() | stdexec::let_value([&]() -> exec::task<void> {
                    auto request = makeSimpleRequest(instanceId);
                    auto rc = co_await terminusManager.SendRecvPldmMsgOverMctp(
                        eid, request, &responseMsg, &responseLen);
                    (void)rc;
                    EXPECT_TRUE(false);
                    co_return;
                }) | stdexec::upon_stopped([&] { stopped = true; }),
                exec::default_task_context<void>(stdexec::inline_scheduler{}));

    scope.request_stop();
    EXPECT_TRUE(stopped);
    stdexec::sync_wait(scope.on_empty());
}

TEST_F(TerminusManagerTest, explicitTidCollisionAndGetTerminusCoverage)
{
    const pldm::MctpInfo firstInfo(
        30, "f72d6f90-5675-11ed-9b6a-0242ac120030",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);
    const pldm::MctpInfo secondInfo(
        31, "f72d6f90-5675-11ed-9b6a-0242ac120031",
        "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 1, std::nullopt,
        "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt);

    ASSERT_TRUE(terminusManager.mapTid(firstInfo, 0x30).has_value());
    EXPECT_EQ(std::nullopt, terminusManager.mapTid(secondInfo, 0x30));

    std::string uuid("00000000-0000-0000-0000-000000000030");
    termini[0x30] = std::make_shared<pldm::platform_mc::Terminus>(
        0x30, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);

    EXPECT_NE(nullptr, terminusManager.getTerminus(uuid));
    EXPECT_EQ(nullptr, terminusManager.getTerminus(
                           "00000000-0000-0000-0000-000000000099"));
}
