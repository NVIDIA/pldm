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

#include "fw-update/fw_update_utility.hpp"
#include "fw-update/update.hpp"
#include "fw-update/update_manager.hpp"
#include "test/test_instance_id.hpp"

#include <linux/mctp.h>
#include <systemd/sd-event.h>

#include <sdbusplus/test/sdbus_mock.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

using namespace pldm;
using namespace pldm::fw_update;
using namespace std::chrono;

namespace
{

size_t g_transportEventCalls = 0;
bool g_lastTransportEventCritical = false;

} // namespace

namespace pldm::transport
{

/** @brief Test double for the libpldmutils definition.
 *
 *  The test executable's definition takes precedence over the shared
 *  library's at link time (same interposition technique as
 *  unpack_pldm_header in request_test.cpp), so handleTransportError()
 *  calls land here and the tests can observe the forwarded arguments.
 */
void createMctpTransportRedfishEvent(
    mctp_eid_t /*eid*/, const std::string& /*commandName*/,
    uint32_t /*errorCode*/, uint8_t /*binding*/, uint8_t /*direction*/,
    const std::string& /*logNamespace*/, bool critical)
{
    ++g_transportEventCalls;
    g_lastTransportEventCritical = critical;
}

} // namespace pldm::transport

namespace
{

int createTempFile(const std::vector<uint8_t>& payload)
{
    std::array<char, 64> pathTemplate{
        "/tmp/pldm_update_and_utility_test_XXXXXX"};
    int fd = mkstemp(pathTemplate.data());
    if (fd < 0)
    {
        return fd;
    }

    std::remove(pathTemplate.data());

    if (!payload.empty())
    {
        auto bytesWritten = write(fd, payload.data(), payload.size());
        if (bytesWritten != static_cast<ssize_t>(payload.size()))
        {
            close(fd);
            return -1;
        }
        lseek(fd, 0, SEEK_SET);
    }

    return fd;
}

pldm::transport::MctpError makeTransportError(
    mctp_eid_t srcEid, mctp_eid_t destEid, uint8_t pldmType)
{
    pldm::transport::MctpError error{};
    error.direction = MCTP_DIR_TX;
    error.src_eid = srcEid;
    error.dest_eid = destEid;
    error.error_code = EHOSTUNREACH;
    error.binding = MCTP_BINDING_UNKNOWN;
    error.msg_type = MCTP_MSG_TYPE_PLDM;
    error.payload_len = 4;
    error.payload[1] = pldmType;
    return error;
}

} // namespace

class UpdateAndUtilityTest : public testing::Test
{
  protected:
    UpdateAndUtilityTest() :
        busMock(sdbusplus::get_mocked_new(&sdbusMock)),
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                      componentInfoMap, componentNameMap, true, nullptr)
    {}

    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    TestInstanceIdDb instanceIdDb;
    sdbusplus::bus::bus busMock;
    sdeventplus::Event event;
    requester::Handler<requester::Request> reqHandler;
    DescriptorMap descriptorMap;
    ComponentInfoMap componentInfoMap;
    ComponentNameMap componentNameMap;
    UpdateManager updateManager;
};

TEST_F(UpdateAndUtilityTest, startUpdate_invalidFdThrowsRuntimeError)
{
    Update update(busMock, "/xyz/openbmc_project/software/test_update",
                  &updateManager);
    bool sawExpectedError = false;

    try
    {
        auto path = update.startUpdate(
            sdbusplus::message::unix_fd{-1},
            ApplyTimeIntf::RequestedApplyTimes::Immediate, false, {}, false);
        (void)path;
    }
    catch (const std::runtime_error& e)
    {
        sawExpectedError =
            std::string(e.what()).find("duplicate image file descriptor") !=
            std::string::npos;
    }

    EXPECT_TRUE(sawExpectedError);
}

TEST_F(UpdateAndUtilityTest, startUpdate_emptyImageThrowsInvalidImage)
{
    int imageFd = createTempFile({});
    ASSERT_GE(imageFd, 0);

    Update update(busMock, "/xyz/openbmc_project/software/test_update_empty",
                  &updateManager);
    std::string errName;

    try
    {
        auto path = update.startUpdate(
            sdbusplus::message::unix_fd{imageFd},
            ApplyTimeIntf::RequestedApplyTimes::Immediate, false, {}, false);
        (void)path;
    }
    catch (const sdbusplus::exception::generated_exception& e)
    {
        errName = e.name();
    }

    close(imageFd);
    EXPECT_EQ(errName,
              "xyz.openbmc_project.Software.Update.Error.InvalidImage");
}

TEST_F(UpdateAndUtilityTest, startUpdate_validImageReturnsSoftwareObjectPath)
{
    int imageFd = createTempFile({0x01, 0x02, 0x03, 0x04});
    ASSERT_GE(imageFd, 0);

    Update update(busMock, "/xyz/openbmc_project/software/test_update_valid",
                  &updateManager);
    auto path = update.startUpdate(sdbusplus::message::unix_fd{imageFd},
                                   ApplyTimeIntf::RequestedApplyTimes::OnReset,
                                   false, {}, false);

    close(imageFd);

    EXPECT_FALSE(path.str.empty());
    EXPECT_NE(path.str.find("/xyz/openbmc_project/software/"),
              std::string::npos);
}

TEST_F(UpdateAndUtilityTest, getImageStream_readsMappedPayload)
{
    constexpr std::array<char, 4> expectedBytes{'A', 'B', 'C', 'D'};
    int imageFd = createTempFile({static_cast<uint8_t>(expectedBytes[0]),
                                  static_cast<uint8_t>(expectedBytes[1]),
                                  static_cast<uint8_t>(expectedBytes[2]),
                                  static_cast<uint8_t>(expectedBytes[3])});
    ASSERT_GE(imageFd, 0);

    Update update(busMock, "/xyz/openbmc_project/software/test_update_stream",
                  &updateManager);
    auto path = update.startUpdate(sdbusplus::message::unix_fd{imageFd},
                                   ApplyTimeIntf::RequestedApplyTimes::OnReset,
                                   false, {}, false);
    (void)path;

    auto& stream = update.getImageStream();
    stream.clear();
    stream.seekg(0, std::ios::beg);

    std::array<char, expectedBytes.size()> actualBytes{};
    stream.read(actualBytes.data(),
                static_cast<std::streamsize>(actualBytes.size()));

    close(imageFd);

    EXPECT_EQ(stream.gcount(),
              static_cast<std::streamsize>(expectedBytes.size()));
    EXPECT_EQ(actualBytes, expectedBytes);
}

TEST_F(UpdateAndUtilityTest, handleTransportError_clearsStoredError)
{
    constexpr mctp_eid_t eid = 9;
    auto transportError = makeTransportError(8, eid, PLDM_FWUP);
    reqHandler.storeTransportError(transportError);
    ASSERT_TRUE(reqHandler.hasTransportError(eid));

    g_transportEventCalls = 0;
    g_lastTransportEventCritical = true;
    handleTransportError(reqHandler, eid, "RequestUpdate", PLDM_FWUP);

    EXPECT_FALSE(reqHandler.hasTransportError(eid));
    EXPECT_EQ(g_transportEventCalls, 1u);
    EXPECT_FALSE(g_lastTransportEventCritical);
}

TEST_F(UpdateAndUtilityTest, handleTransportError_criticalModeClearsError)
{
    constexpr mctp_eid_t eid = 9;
    auto transportError = makeTransportError(8, eid, PLDM_FWUP);
    reqHandler.storeTransportError(transportError);
    ASSERT_TRUE(reqHandler.hasTransportError(eid));

    // Critical mode (pre-update validation): the same transport
    // event is emitted, at Critical severity, and the queued error is
    // cleared exactly as in the default mode.
    g_transportEventCalls = 0;
    g_lastTransportEventCritical = false;
    handleTransportError(reqHandler, eid, "QueryDeviceIdentifiers", PLDM_FWUP,
                         true);

    EXPECT_FALSE(reqHandler.hasTransportError(eid));
    EXPECT_EQ(g_transportEventCalls, 1u);
    EXPECT_TRUE(g_lastTransportEventCritical);
}

TEST_F(UpdateAndUtilityTest, handleTransportError_keepsMismatchedTypeError)
{
    constexpr mctp_eid_t eid = 9;
    auto transportError = makeTransportError(8, eid, PLDM_PLATFORM);
    reqHandler.storeTransportError(transportError);
    ASSERT_TRUE(reqHandler.hasTransportError(eid));

    handleTransportError(reqHandler, eid, "RequestUpdate", PLDM_FWUP);

    EXPECT_TRUE(reqHandler.hasTransportError(eid));
}
