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
#include "common/test/mocked_utils.hpp"
#include "common/types.hpp"
#include "common/utils.hpp" // NOLINT(misc-include-cleaner)
#include "fw-update/package_parser.hpp"
#include "requester/handler.hpp"
#include "requester/request.hpp"

#include <sdbusplus/message/native_types.hpp>
#include <sdeventplus/event.hpp>
#include <xyz/openbmc_project/Common/error.hpp>
#include <xyz/openbmc_project/Software/Activation/server.hpp>

#include <chrono>
#include <cstdint>
#include <ios>
#include <iterator>
#include <memory>

#include "gmock/gmock.h"
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "fw-update/item_update_manager.hpp"
#undef private
#include "test/test_instance_id.hpp"

#include <fcntl.h>
#include <libpldm/base.h>
#include <libpldm/firmware_update.h>
#include <systemd/sd-event.h>
#include <unistd.h>

#include <sdbusplus/bus.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>

#include <filesystem>
#include <fstream>
#include <vector>

#include <gtest/gtest.h>

using namespace pldm;
using namespace pldm::fw_update;
using namespace std::chrono;

class ItemUpdateManagerTest : public testing::Test
{
  protected:
    ItemUpdateManagerTest() :
        busMock(sdbusplus::get_mocked_new(&sdbusMock)),
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100))
    {
        // Extract the descriptors carried by the reference test package so an
        // ItemUpdateManager built with them can associate the package to a
        // (synthetic) device and drive the full processPackage() path.
        std::ifstream pkg("./test_pkg", std::ios::binary);
        packageBytes.assign(std::istreambuf_iterator<char>(pkg),
                            std::istreambuf_iterator<char>());
        auto parser = parsePkgHeader(packageBytes.data(), packageBytes.size());
        if (parser)
        {
            parser->parse(packageBytes, packageBytes.size());
            const auto& records = parser->getFwDeviceIDRecords();
            if (!records.empty())
            {
                matchingDescriptors = std::get<Descriptors>(records[0]);
            }
        }
    }

    void waitEventExpiry(milliseconds timeout)
    {
        while (true)
        {
            auto sleepTime = duration_cast<microseconds>(timeout);
            if (!sd_event_run(event.get(), sleepTime.count()))
            {
                break;
            }
        }
    }

    static std::vector<uint8_t> makeRequest(uint8_t command)
    {
        std::vector<uint8_t> buf(sizeof(pldm_msg_hdr) + 1, 0);
        auto* msg = reinterpret_cast<pldm_msg*>(buf.data());
        msg->hdr.request = 1;
        msg->hdr.type = PLDM_FWUP;
        msg->hdr.command = command;
        return buf;
    }

    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    TestInstanceIdDb instanceIdDb;
    sdbusplus::bus_t busMock;
    sdeventplus::Event event;
    requester::Handler<requester::Request> reqHandler;
    std::vector<uint8_t> packageBytes;
    Descriptors matchingDescriptors;
    ComponentInfo componentInfo;
    Descriptors emptyDescriptors;
};

TEST_F(ItemUpdateManagerTest, AssociatePkgToDeviceMatchAndMismatch)
{
    ItemUpdateManager manager(1, event, reqHandler, instanceIdDb,
                              "/xyz/openbmc_project/software/item_assoc", "0",
                              matchingDescriptors, componentInfo);

    // Build a record set whose descriptors exactly equal the caller's
    // descriptors -> std::includes matches -> offset 0.
    FirmwareDeviceIDRecords records;
    records.emplace_back(DeviceUpdateOptionFlags{}, ApplicableComponents{},
                         ComponentImageSetVersion{"v1"}, matchingDescriptors,
                         FirmwareDevicePackageData{});
    auto offset = manager.associatePkgToDevice(records, matchingDescriptors);
    ASSERT_TRUE(offset.has_value());
    EXPECT_EQ(*offset, 0u);

    // A record that demands a descriptor absent from the caller set does not
    // match.
    Descriptors extra = matchingDescriptors;
    extra.emplace(0xABCD, DescriptorData{0xDE, 0xAD});
    FirmwareDeviceIDRecords noMatch;
    noMatch.emplace_back(DeviceUpdateOptionFlags{}, ApplicableComponents{},
                         ComponentImageSetVersion{"v1"}, extra,
                         FirmwareDevicePackageData{});
    EXPECT_FALSE(
        manager.associatePkgToDevice(noMatch, matchingDescriptors).has_value());
}

TEST_F(ItemUpdateManagerTest, HandleRequestNoDeviceUpdater)
{
    ItemUpdateManager manager(2, event, reqHandler, instanceIdDb,
                              "/xyz/openbmc_project/software/item_noupd", "0",
                              emptyDescriptors, componentInfo);

    auto req = makeRequest(PLDM_REQUEST_FIRMWARE_DATA);
    auto* msg = reinterpret_cast<pldm_msg*>(req.data());
    auto resp = manager.handleRequest(2, PLDM_REQUEST_FIRMWARE_DATA, msg,
                                      req.size() - sizeof(pldm_msg_hdr));
    ASSERT_GE(resp.size(), sizeof(pldm_msg_hdr) + 1);
    auto* respMsg = reinterpret_cast<pldm_msg*>(resp.data());
    EXPECT_EQ(respMsg->payload[0], PLDM_FWUP_COMMAND_NOT_EXPECTED);
}

TEST_F(ItemUpdateManagerTest, ResetActivationState)
{
    ItemUpdateManager manager(3, event, reqHandler, instanceIdDb,
                              "/xyz/openbmc_project/software/item_reset", "0",
                              emptyDescriptors, componentInfo);
    manager.updateInProgress = true;
    EXPECT_NO_THROW(manager.resetActivationState());
    EXPECT_FALSE(manager.updateInProgress);
}

TEST_F(ItemUpdateManagerTest, StartUpdateGuardRejectsBadFd)
{
    ItemUpdateManager manager(4, event, reqHandler, instanceIdDb,
                              "/xyz/openbmc_project/software/item_badfd", "0",
                              emptyDescriptors, componentInfo);

    sdbusplus::message::unix_fd badFd{-1};
    EXPECT_THROW(
        manager.startUpdate(
            badFd, ApplyTimeIntf::RequestedApplyTimes::Immediate, false, {}),
        sdbusplus::xyz::openbmc_project::Common::Error::Unavailable);
}

TEST_F(ItemUpdateManagerTest, StartUpdateGuardRejectsConcurrentUpdate)
{
    ItemUpdateManager manager(5, event, reqHandler, instanceIdDb,
                              "/xyz/openbmc_project/software/item_busy", "0",
                              emptyDescriptors, componentInfo);
    manager.updateInProgress = true;

    int fd = open("./test_pkg", O_RDONLY);
    ASSERT_GE(fd, 0);
    sdbusplus::message::unix_fd wrapped{fd};
    EXPECT_THROW(
        manager.startUpdate(
            wrapped, ApplyTimeIntf::RequestedApplyTimes::Immediate, false, {}),
        sdbusplus::xyz::openbmc_project::Common::Error::Unavailable);
    close(fd);
}

TEST_F(ItemUpdateManagerTest, ProcessPackageRejectsTooSmallPackage)
{
    ItemUpdateManager manager(6, event, reqHandler, instanceIdDb,
                              "/xyz/openbmc_project/software/item_small", "0",
                              emptyDescriptors, componentInfo);
    manager.objPathWithSwId = "/xyz/openbmc_project/software/item_small_sw";

    // A tiny file cannot even hold the package header information struct.
    auto tmp = std::filesystem::temp_directory_path() / "pldm_small_pkg";
    {
        std::ofstream ofs(tmp, std::ios::binary);
        const char data[] = {0x01, 0x02, 0x03, 0x04};
        ofs.write(data, sizeof(data));
    }
    manager.packageMap = std::make_unique<pldm::utils::MMapHandler>(tmp);
    EXPECT_FALSE(manager.processPackage());
    std::filesystem::remove(tmp);
}

TEST_F(ItemUpdateManagerTest, ProcessPackageRejectsInvalidHeader)
{
    ItemUpdateManager manager(7, event, reqHandler, instanceIdDb,
                              "/xyz/openbmc_project/software/item_badhdr", "0",
                              emptyDescriptors, componentInfo);
    manager.objPathWithSwId = "/xyz/openbmc_project/software/item_badhdr_sw";

    // Large enough to pass the size guard but not a valid PLDM header.
    auto tmp = std::filesystem::temp_directory_path() / "pldm_bad_hdr_pkg";
    {
        std::ofstream ofs(tmp, std::ios::binary);
        std::vector<char> junk(256, 0x5A);
        ofs.write(junk.data(), junk.size());
    }
    manager.packageMap = std::make_unique<pldm::utils::MMapHandler>(tmp);
    EXPECT_FALSE(manager.processPackage());
    std::filesystem::remove(tmp);
}

TEST_F(ItemUpdateManagerTest, ProcessPackageNoDeviceAssociation)
{
    // Valid package, but the manager's descriptor set is empty so no device
    // ID record can be associated.
    ItemUpdateManager manager(8, event, reqHandler, instanceIdDb,
                              "/xyz/openbmc_project/software/item_noassoc", "0",
                              emptyDescriptors, componentInfo);
    manager.objPathWithSwId = "/xyz/openbmc_project/software/item_noassoc_sw";
    ASSERT_FALSE(packageBytes.empty());
    manager.packageMap = std::make_unique<pldm::utils::MMapHandler>(
        std::filesystem::path{"./test_pkg"});
    EXPECT_FALSE(manager.processPackage());
}

TEST_F(ItemUpdateManagerTest, ProcessPackageSuccessDrivesLifecycle)
{
    ASSERT_FALSE(matchingDescriptors.empty());
    ItemUpdateManager manager(9, event, reqHandler, instanceIdDb,
                              "/xyz/openbmc_project/software/item_ok", "0",
                              matchingDescriptors, componentInfo);
    manager.objPathWithSwId = "/xyz/openbmc_project/software/item_ok_sw";
    manager.packageMap = std::make_unique<pldm::utils::MMapHandler>(
        std::filesystem::path{"./test_pkg"});

    ASSERT_TRUE(manager.processPackage());
    ASSERT_NE(manager.deviceUpdater, nullptr);

    // handleRequest now routes through the live deviceUpdater. With no
    // component update in flight each command returns COMMAND_NOT_EXPECTED.
    for (uint8_t cmd : {PLDM_REQUEST_FIRMWARE_DATA, PLDM_TRANSFER_COMPLETE,
                        PLDM_VERIFY_COMPLETE, PLDM_APPLY_COMPLETE})
    {
        auto req = makeRequest(cmd);
        auto* msg = reinterpret_cast<pldm_msg*>(req.data());
        EXPECT_NO_THROW(manager.handleRequest(
            9, cmd, msg, req.size() - sizeof(pldm_msg_hdr)));
    }

    // An unexpected command hits the invalid-data encode branch.
    {
        auto req = makeRequest(0xFF);
        auto* msg = reinterpret_cast<pldm_msg*>(req.data());
        auto resp = manager.handleRequest(9, 0xFF, msg,
                                          req.size() - sizeof(pldm_msg_hdr));
        auto* respMsg = reinterpret_cast<pldm_msg*>(resp.data());
        EXPECT_EQ(respMsg->payload[0], PLDM_ERROR_INVALID_DATA);
    }

    // Progress refresh path with a live (but idle) deviceUpdater.
    EXPECT_NO_THROW(manager.markComponentUpdateCompleted());

    // activatePackage arms a deferred handler; it must not run to completion
    // here (no real device), so we do not drain the event loop.
    EXPECT_EQ(manager.activatePackage(),
              software::Activation::Activations::Activating);

    // Completion (failure) tears down all transient state.
    EXPECT_NO_THROW(manager.updateDeviceCompletion(9, false));
    EXPECT_FALSE(manager.updateInProgress);
}

TEST_F(ItemUpdateManagerTest, StartUpdateDeferredProcessesPackage)
{
    // Full happy path through the public D-Bus entry point: startUpdate ->
    // processFd (defer) -> processPackage on event dispatch.
    ItemUpdateManager manager(10, event, reqHandler, instanceIdDb,
                              "/xyz/openbmc_project/software/item_start", "0",
                              matchingDescriptors, componentInfo);
    ASSERT_FALSE(matchingDescriptors.empty());

    int fd = open("./test_pkg", O_RDONLY);
    ASSERT_GE(fd, 0);
    sdbusplus::message::unix_fd wrapped{fd};

    auto objPath = manager.startUpdate(
        wrapped, ApplyTimeIntf::RequestedApplyTimes::Immediate, false, {});
    close(fd);
    EXPECT_FALSE(std::string(objPath).empty());
    EXPECT_TRUE(manager.updateInProgress);

    // Dispatch the deferred handler: this mmaps the dup'd fd and runs
    // processPackage() to success (descriptors match), so the defer lambda
    // returns without throwing.
    waitEventExpiry(milliseconds(200));
    EXPECT_NE(manager.deviceUpdater, nullptr);
}

// Exercise the MMapHandler accessors (mutable/const getData, const getBytes and
// getChars views) over the reference package file.
TEST_F(ItemUpdateManagerTest, MMapHandlerAccessors)
{
    pldm::utils::MMapHandler handler{std::filesystem::path{"./test_pkg"}};
    const auto& constHandler = handler;

    ASSERT_GT(handler.getSize(), 0U);

    char* mutableData = handler.getData();
    ASSERT_NE(mutableData, nullptr);

    const char* constData = constHandler.getData();
    ASSERT_NE(constData, nullptr);
    EXPECT_EQ(mutableData, constData);

    auto byteView = constHandler.getBytes();
    EXPECT_EQ(byteView.size(), handler.getSize());

    auto charView = constHandler.getChars();
    EXPECT_EQ(charView.size(), handler.getSize());
}

// Cover PackageParser::getFormatVersion() on a successfully parsed package.
TEST_F(ItemUpdateManagerTest, PackageParserFormatVersion)
{
    ASSERT_FALSE(packageBytes.empty());
    auto parser = parsePkgHeader(packageBytes.data(), packageBytes.size());
    ASSERT_NE(parser, nullptr);
    EXPECT_NO_THROW((void)parser->getFormatVersion());
}
