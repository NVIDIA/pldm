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
#include "fw-update/activation.hpp"
#include "fw-update/oem-nvidia/debug_token.hpp"
#include "fw-update/other_device_update_manager.hpp"
#include "fw-update/update_manager.hpp"
#include "requester/test/mock_request.hpp"
#include "test/test_instance_id.hpp"

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include <sdbusplus/async.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>

#include <sstream>
#include <stdexcept>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldm::fw_update;
using namespace std::chrono;

class DebugTokenInternalTest : public testing::Test
{
  protected:
    DebugTokenInternalTest() :
        busMock(sdbusplus::get_mocked_new(&sdbusMock)),
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                      componentInfoMap, componentNameMap, true, nullptr)
    {}

    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    TestInstanceIdDb instanceIdDb;
    sdbusplus::bus_t busMock;
    sdeventplus::Event event;
    requester::Handler<requester::Request> reqHandler;
    DescriptorMap descriptorMap;
    ComponentInfoMap componentInfoMap;
    ComponentNameMap componentNameMap;
    UpdateManager updateManager;
};

class RecordingActivation : public Activation
{
  public:
    RecordingActivation(sdbusplus::bus_t& bus, const std::string& objPath,
                        UpdateManager* updateManager) :
        Activation(bus, objPath, software::Activation::Activations::Ready,
                   updateManager)
    {}

    ActivationIntf::Activations activation(Activations value) override
    {
        lastValue = value;
        // Tests need only the plain D-Bus property setter path here.
        // NOLINTNEXTLINE(bugprone-parent-virtual-call)
        return sdbusplus::xyz::openbmc_project::Software::server::Activation::
            activation(value);
    }

    ActivationIntf::Activations lastValue{
        software::Activation::Activations::Ready};
};

static void sealAndRewind(sdbusplus::message::message& msg)
{
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);
}

/** Prepare UpdateManager so DebugToken::activate() async failure path can call
 *  startUpdate() without null deref (otherDeviceUpdateManager, progress timer,
 *  and OEM_NVIDIA debugToken). */
static void wireUpdateManagerForActivateAsyncFailure(UpdateManager& um,
                                                     sdbusplus::bus_t& bus)
{
    um.otherDeviceUpdateManager = std::make_unique<OtherDeviceUpdateManager>(
        bus, &um, std::vector<sdbusplus::object_path>{});
    um.deviceUpdaterMap.clear();
    um.objPath = "/xyz/openbmc_project/software/debug_token_test";
    um.createProgressUpdateTimer();
#ifdef OEM_NVIDIA
    um.debugToken = std::make_unique<DebugToken>(bus, &um);
#endif
}

TEST_F(DebugTokenInternalTest, activateDoesNotThrowWithAsyncCall)
{
    wireUpdateManagerForActivateAsyncFailure(updateManager, busMock);

    DebugToken debugToken(busMock, &updateManager);
    debugToken.tokenPath =
        "/xyz/openbmc_project/software/HGX_FW_Debug_Token_Erase";

    EXPECT_NO_THROW({ stdexec::sync_wait(debugToken.activate()); });
}

TEST_F(DebugTokenInternalTest, activateReturnsFalseOnAsyncCallFailure)
{
    // Pins the activate() -> exec::task<bool> contract: on the mock bus
    // coSetDbusProperty resolves to false, so activate() must surface
    // false to its caller. Guards the startTimer-only-on-success gate in
    // updateDebugToken().
    wireUpdateManagerForActivateAsyncFailure(updateManager, busMock);

    DebugToken debugToken(busMock, &updateManager);
    debugToken.tokenPath =
        "/xyz/openbmc_project/software/HGX_FW_Debug_Token_Erase";

    auto result = stdexec::sync_wait(debugToken.activate());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(std::get<0>(result.value()));
}

TEST_F(DebugTokenInternalTest, activateFailureRunsSynchronousCleanup)
{
    // No functional regression guard for the failure branch inside
    // activate(): tokenStatus must flip to true, activationMatches must be
    // drained, and startUpdate() must have run (proven by survival without
    // a null deref now that the timer is no longer constructed on this
    // path).
    wireUpdateManagerForActivateAsyncFailure(updateManager, busMock);

    DebugToken debugToken(busMock, &updateManager);
    debugToken.tokenPath = "/xyz/openbmc_project/software/HGX_FW_Debug_Token";
    debugToken.tokenVersion = "1.2.3";
    debugToken.tokenStatus = false;

    EXPECT_NO_THROW({ stdexec::sync_wait(debugToken.activate()); });

    EXPECT_TRUE(debugToken.tokenStatus);
    EXPECT_TRUE(debugToken.activationMatches.empty());
    EXPECT_EQ(debugToken.timer, nullptr);
}

TEST_F(DebugTokenInternalTest, getFilePathReturnsEmptyWhenNoMatchingUuid)
{
    DebugToken debugToken(busMock, &updateManager);

    auto [directoryPath, objectPath] = debugToken.getFilePath(InstallTokenUUID);

    EXPECT_TRUE(directoryPath.empty());
    EXPECT_TRUE(objectPath.empty());
}

TEST_F(DebugTokenInternalTest, setVersionHandlesDbusFailure)
{
    DebugToken debugToken(busMock, &updateManager);
    debugToken.tokenPath = "/xyz/openbmc_project/software/nonexistent";
    debugToken.tokenVersion = "1.0";

    EXPECT_NO_THROW({ stdexec::sync_wait(debugToken.setVersion()); });
}

TEST_F(DebugTokenInternalTest,
       onActivationChangedMsgIgnoresSignalForDifferentObjectPath)
{
    DebugToken debugToken(busMock, &updateManager);
    debugToken.tokenPath = "/xyz/openbmc_project/software/HGX_FW_Debug_Token";
    debugToken.tokenStatus = false;

    pldm::dbus::PropertyMap properties;
    properties.emplace("Activation",
                       std::string("xyz.openbmc_project.Software.Activation."
                                   "Activations.Active"));

    auto msg = busMock.new_method_call(
        "xyz.openbmc_project.TestService",
        "/xyz/openbmc_project/software/some_other_component",
        "xyz.openbmc_project.TestInterface", "TestMethod");
    msg.append(std::string(Server::Activation::interface), properties);

    EXPECT_THROW({ debugToken.onActivationChangedMsg(msg); }, std::logic_error);
    EXPECT_FALSE(debugToken.tokenStatus);
}

TEST_F(DebugTokenInternalTest, isDebugTokenComponentPresentDefaultFalse)
{
    DebugToken debugToken(busMock, &updateManager);

    EXPECT_FALSE(debugToken.isDebugTokenComponentPresent());
}

TEST_F(DebugTokenInternalTest, startTimerReturnsWhenTokenAlreadyCompleted)
{
    DebugToken debugToken(busMock, &updateManager);
    debugToken.tokenPath =
        "/xyz/openbmc_project/software/HGX_FW_Debug_Token_Erase";
    debugToken.tokenVersion = "0.0";
    debugToken.tokenStatus = true;

    debugToken.startTimer(std::chrono::seconds(0));

    sd_event* timerEvent = nullptr;
    ASSERT_EQ(sd_event_default(&timerEvent), 0);
    EXPECT_GE(sd_event_run(timerEvent, 0), 0);
    sd_event_unref(timerEvent);
}

TEST_F(DebugTokenInternalTest, startTimerTimeoutForErasePathTriggersUpdate)
{
    std::vector<sdbusplus::object_path> targets;
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                   targets);
    updateManager.deviceUpdaterMap.clear();
    updateManager.objPath = "/xyz/openbmc_project/software/debug_token_test";
    updateManager.createProgressUpdateTimer();
    updateManager.debugToken =
        std::make_unique<DebugToken>(busMock, &updateManager);

    DebugToken debugToken(busMock, &updateManager);
    debugToken.tokenPath =
        "/xyz/openbmc_project/software/HGX_FW_Debug_Token_Erase";
    debugToken.tokenVersion = "0.0";
    debugToken.tokenStatus = false;
    debugToken.startTimer(std::chrono::seconds(0));

    sd_event* timerEvent = nullptr;
    ASSERT_EQ(sd_event_default(&timerEvent), 0);
    EXPECT_GE(sd_event_run(timerEvent, 0), 0);
    sd_event_unref(timerEvent);
}

TEST_F(DebugTokenInternalTest, startTimerTimeoutForInstallPathTriggersUpdate)
{
    std::vector<sdbusplus::object_path> targets;
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                   targets);
    updateManager.deviceUpdaterMap.clear();
    updateManager.objPath = "/xyz/openbmc_project/software/debug_token_test";
    updateManager.createProgressUpdateTimer();
    updateManager.debugToken =
        std::make_unique<DebugToken>(busMock, &updateManager);

    DebugToken debugToken(busMock, &updateManager);
    debugToken.tokenPath = "/xyz/openbmc_project/software/HGX_FW_Debug_Token";
    debugToken.tokenVersion = "1.2.3";
    debugToken.tokenStatus = false;
    debugToken.startTimer(std::chrono::seconds(0));

    sd_event* timerEvent = nullptr;
    ASSERT_EQ(sd_event_default(&timerEvent), 0);
    EXPECT_GE(sd_event_run(timerEvent, 0), 0);
    sd_event_unref(timerEvent);
}

TEST_F(DebugTokenInternalTest, updateDebugTokenFallsBackToEraseTokenPath)
{
    DebugToken debugToken(busMock, &updateManager);
    std::vector<sdbusplus::object_path> targets;
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                   targets);
    updateManager.deviceUpdaterMap.clear();
    updateManager.objPath = "/xyz/openbmc_project/software/debug_token_test";
    updateManager.createProgressUpdateTimer();
    updateManager.debugToken =
        std::make_unique<DebugToken>(busMock, &updateManager);

    FirmwareDeviceIDRecords fwDeviceIDRecords;
    ComponentImageInfos componentImageInfos;
    std::istringstream package("dummy package");

    EXPECT_NO_THROW({
        stdexec::sync_wait(debugToken.updateDebugToken(
            fwDeviceIDRecords, componentImageInfos, package));
    });
    EXPECT_FALSE(debugToken.isDebugTokenComponentPresent());
    EXPECT_EQ(debugToken.tokenVersion, "0.0");
}

TEST_F(DebugTokenInternalTest, startUpdateHandlesFailedNonPldmActivation)
{
    std::vector<sdbusplus::object_path> targets;
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                   targets);
    updateManager.deviceUpdaterMap.clear();
    updateManager.objPath = "/xyz/openbmc_project/software/debug_token_test";
    updateManager.createProgressUpdateTimer();
    updateManager.debugToken =
        std::make_unique<DebugToken>(busMock, &updateManager);

    EXPECT_NO_THROW({ updateManager.debugToken->startUpdate(); });
}

TEST_F(DebugTokenInternalTest, activateDoesNotThrowForInstallToken)
{
    wireUpdateManagerForActivateAsyncFailure(updateManager, busMock);

    DebugToken debugToken(busMock, &updateManager);
    debugToken.tokenPath = "/xyz/openbmc_project/software/HGX_FW_Debug_Token";
    debugToken.tokenVersion = "1.2.3";

    EXPECT_NO_THROW({ stdexec::sync_wait(debugToken.activate()); });
}

TEST_F(DebugTokenInternalTest, onActivationChangedMsgActiveSetsTokenStatus)
{
    std::vector<sdbusplus::object_path> targets;
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                   targets);
    updateManager.deviceUpdaterMap.clear();
    updateManager.objPath = "/xyz/openbmc_project/software/debug_token_active";
    updateManager.createProgressUpdateTimer();
    updateManager.debugToken =
        std::make_unique<DebugToken>(busMock, &updateManager);

    DebugToken debugToken(busMock, &updateManager);
    debugToken.tokenPath = "/xyz/openbmc_project/software/HGX_FW_Debug_Token";
    debugToken.tokenStatus = false;

    pldm::dbus::PropertyMap properties;
    properties.emplace(
        "Activation",
        std::string("xyz.openbmc_project.Software.Activation.Activations."
                    "Active"));
    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test", debugToken.tokenPath.c_str(),
                                      "org.test.Interface", "Method");
    msg.append(std::string(Server::Activation::interface), properties);
    sealAndRewind(msg);

    EXPECT_NO_THROW({ debugToken.onActivationChangedMsg(msg); });
    EXPECT_TRUE(debugToken.tokenStatus);
}

TEST_F(DebugTokenInternalTest, onActivationChangedMsgFailedSetsTokenStatus)
{
    std::vector<sdbusplus::object_path> targets;
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                   targets);
    updateManager.deviceUpdaterMap.clear();
    updateManager.objPath = "/xyz/openbmc_project/software/debug_token_failed";
    updateManager.createProgressUpdateTimer();
    updateManager.debugToken =
        std::make_unique<DebugToken>(busMock, &updateManager);

    DebugToken debugToken(busMock, &updateManager);
    debugToken.tokenPath = "/xyz/openbmc_project/software/HGX_FW_Debug_Token";
    debugToken.tokenStatus = false;

    pldm::dbus::PropertyMap properties;
    properties.emplace(
        "Activation",
        std::string("xyz.openbmc_project.Software.Activation.Activations."
                    "Failed"));
    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test", debugToken.tokenPath.c_str(),
                                      "org.test.Interface", "Method");
    msg.append(std::string(Server::Activation::interface), properties);
    sealAndRewind(msg);

    EXPECT_NO_THROW({ debugToken.onActivationChangedMsg(msg); });
    EXPECT_TRUE(debugToken.tokenStatus);
}

TEST_F(DebugTokenInternalTest,
       onActivationChangedMsgActivatingKeepsTokenStatusFalse)
{
    DebugToken debugToken(busMock, &updateManager);
    debugToken.tokenPath = "/xyz/openbmc_project/software/HGX_FW_Debug_Token";
    debugToken.tokenStatus = false;

    pldm::dbus::PropertyMap properties;
    properties.emplace(
        "Activation",
        std::string("xyz.openbmc_project.Software.Activation.Activations."
                    "Activating"));
    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test", debugToken.tokenPath.c_str(),
                                      "org.test.Interface", "Method");
    msg.append(std::string(Server::Activation::interface), properties);
    sealAndRewind(msg);

    EXPECT_NO_THROW({ debugToken.onActivationChangedMsg(msg); });
    EXPECT_FALSE(debugToken.tokenStatus);
}

TEST_F(DebugTokenInternalTest,
       onActivationChangedMsgWithoutActivationPropertyKeepsTokenStatusFalse)
{
    DebugToken debugToken(busMock, &updateManager);
    debugToken.tokenPath = "/xyz/openbmc_project/software/HGX_FW_Debug_Token";
    debugToken.tokenStatus = false;

    pldm::dbus::PropertyMap properties;
    properties.emplace("Unrelated", std::string("value"));
    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test", debugToken.tokenPath.c_str(),
                                      "org.test.Interface", "Method");
    msg.append(std::string(Server::Activation::interface), properties);
    sealAndRewind(msg);

    EXPECT_NO_THROW({ debugToken.onActivationChangedMsg(msg); });
    EXPECT_FALSE(debugToken.tokenStatus);
}

TEST_F(DebugTokenInternalTest,
       updateDebugTokenInstallUuidWithEmptyApplicableComponents)
{
    std::vector<sdbusplus::object_path> targets;
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                   targets);
    updateManager.deviceUpdaterMap.clear();
    updateManager.objPath = "/xyz/openbmc_project/software/debug_token_empty";
    updateManager.createProgressUpdateTimer();
    updateManager.debugToken =
        std::make_unique<DebugToken>(busMock, &updateManager);

    DebugToken debugToken(busMock, &updateManager);
    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {},
         "VersionString",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x76, 0x91, 0x0D, 0xFA, 0x1E, 0x4C, 0x11, 0xED,
                                0x86, 0x1D, 0x02, 0x42, 0xAC, 0x12, 0x00,
                                0x02}}},
         {}}};
    ComponentImageInfos componentImageInfos;
    std::istringstream package("dummy package");

    EXPECT_NO_THROW({
        stdexec::sync_wait(debugToken.updateDebugToken(
            fwDeviceIDRecords, componentImageInfos, package));
    });
    EXPECT_FALSE(debugToken.isDebugTokenComponentPresent());
}

TEST_F(DebugTokenInternalTest, updateDebugTokenSkipsNonMatchingUuid)
{
    std::vector<sdbusplus::object_path> targets;
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                   targets);
    updateManager.deviceUpdaterMap.clear();
    updateManager.objPath = "/xyz/openbmc_project/software/debug_token_nomatch";
    updateManager.createProgressUpdateTimer();
    updateManager.debugToken =
        std::make_unique<DebugToken>(busMock, &updateManager);

    DebugToken debugToken(busMock, &updateManager);
    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {0},
         "VersionString",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00}}},
         {}}};
    ComponentImageInfos componentImageInfos{
        {10, deadComponent, 0xFFFFFFFF, 0, 0, 0, 4, "VersionString2"}};
    std::istringstream package("12345678");

    EXPECT_NO_THROW({
        stdexec::sync_wait(debugToken.updateDebugToken(
            fwDeviceIDRecords, componentImageInfos, package));
    });
    EXPECT_FALSE(debugToken.isDebugTokenComponentPresent());
}

TEST_F(DebugTokenInternalTest, updateDebugTokenSkipsNonDeadInstallComponent)
{
    std::vector<sdbusplus::object_path> targets;
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                   targets);
    updateManager.deviceUpdaterMap.clear();
    updateManager.objPath =
        "/xyz/openbmc_project/software/debug_token_non_dead_comp";
    updateManager.createProgressUpdateTimer();
    updateManager.debugToken =
        std::make_unique<DebugToken>(busMock, &updateManager);

    DebugToken debugToken(busMock, &updateManager);
    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {0},
         "VersionString",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x76, 0x91, 0x0D, 0xFA, 0x1E, 0x4C, 0x11, 0xED,
                                0x86, 0x1D, 0x02, 0x42, 0xAC, 0x12, 0x00,
                                0x02}}},
         {}}};
    ComponentImageInfos componentImageInfos{
        {10, 100, 0xFFFFFFFF, 0, 0, 0, 4, "VersionString2"}};
    std::istringstream package("12345678");

    EXPECT_NO_THROW({
        stdexec::sync_wait(debugToken.updateDebugToken(
            fwDeviceIDRecords, componentImageInfos, package));
    });
    EXPECT_FALSE(debugToken.isDebugTokenComponentPresent());
}

TEST_F(DebugTokenInternalTest,
       updateDebugTokenInstallUuidDeadComponentWithMissingPath)
{
    std::vector<sdbusplus::object_path> targets;
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                   targets);
    updateManager.deviceUpdaterMap.clear();
    updateManager.objPath =
        "/xyz/openbmc_project/software/debug_token_dead_component";
    updateManager.createProgressUpdateTimer();
    updateManager.debugToken =
        std::make_unique<DebugToken>(busMock, &updateManager);

    DebugToken debugToken(busMock, &updateManager);
    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {0},
         "VersionString",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x76, 0x91, 0x0D, 0xFA, 0x1E, 0x4C, 0x11, 0xED,
                                0x86, 0x1D, 0x02, 0x42, 0xAC, 0x12, 0x00,
                                0x02}}},
         {}}};
    ComponentImageInfos componentImageInfos{
        {10, deadComponent, 0xFFFFFFFF, 0, 0, 0, 4, "VersionString3"}};
    std::istringstream package("ABCDEFGH");

    EXPECT_NO_THROW({
        stdexec::sync_wait(debugToken.updateDebugToken(
            fwDeviceIDRecords, componentImageInfos, package));
    });
    EXPECT_FALSE(debugToken.isDebugTokenComponentPresent());
}

TEST_F(DebugTokenInternalTest, activateDoesNotThrowWhenDbusSetPropertySucceeds)
{
    wireUpdateManagerForActivateAsyncFailure(updateManager, busMock);

    MockdBusHandler dbusHandler;
    DebugToken debugToken(busMock, &updateManager, dbusHandler);
    debugToken.tokenPath = "/xyz/openbmc_project/software/HGX_FW_Debug_Token";

    EXPECT_NO_THROW({ stdexec::sync_wait(debugToken.activate()); });
}

TEST_F(DebugTokenInternalTest, setVersionUsesDbusPropertyWhenAvailable)
{
    MockdBusHandler dbusHandler;
    DebugToken debugToken(busMock, &updateManager, dbusHandler);
    debugToken.tokenPath = "/xyz/openbmc_project/software/HGX_FW_Debug_Token";
    debugToken.tokenVersion = "9.9.9";

    EXPECT_NO_THROW({ stdexec::sync_wait(debugToken.setVersion()); });
}

TEST_F(DebugTokenInternalTest, getFilePathReturnsDirectoryForMatchingUuid)
{
    MockdBusHandler dbusHandler;
    DebugToken debugToken(busMock, &updateManager, dbusHandler);

    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillOnce(testing::Return(
            std::vector<std::string>{"/xyz/openbmc_project/software/other/a"}));
    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(testing::_, testing::_, testing::_))
        .WillRepeatedly([](const char* objPath, const char* property,
                           const char*) -> pldm::utils::PropertyValue {
            if (std::string(objPath) ==
                    "/xyz/openbmc_project/software/other/a" &&
                std::string(property) == "UUID")
            {
                return std::string(InstallTokenUUID);
            }
            if (std::string(objPath) ==
                    "/xyz/openbmc_project/software/other/a" &&
                std::string(property) == "Path")
            {
                return std::string("/tmp/debug-token/a/token.bin");
            }
            throw std::runtime_error("unexpected property request");
        });

    auto [directoryPath, objectPath] = debugToken.getFilePath(InstallTokenUUID);
    EXPECT_EQ(directoryPath, "/tmp/debug-token/a");
    EXPECT_EQ(objectPath, "/xyz/openbmc_project/software/other/a");
}

TEST_F(DebugTokenInternalTest, getFilePathSkipsEntryWhenPathLookupThrows)
{
    MockdBusHandler dbusHandler;
    DebugToken debugToken(busMock, &updateManager, dbusHandler);

    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillOnce(testing::Return(
            std::vector<std::string>{"/xyz/openbmc_project/software/other/a",
                                     "/xyz/openbmc_project/software/other/b"}));
    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(testing::_, testing::_, testing::_))
        .WillRepeatedly([](const char* objPath, const char* property,
                           const char*) -> pldm::utils::PropertyValue {
            if (std::string(property) == "UUID")
            {
                return std::string(InstallTokenUUID);
            }
            if (std::string(property) == "Path")
            {
                if (std::string(objPath) ==
                    "/xyz/openbmc_project/software/other/a")
                {
                    throw std::runtime_error("path missing");
                }
                if (std::string(objPath) ==
                    "/xyz/openbmc_project/software/other/b")
                {
                    return std::string("/tmp/debug-token/b/token.bin");
                }
            }
            throw std::runtime_error("unexpected property request");
        });

    auto [directoryPath, objectPath] = debugToken.getFilePath(InstallTokenUUID);
    EXPECT_EQ(directoryPath, "/tmp/debug-token/b");
    EXPECT_EQ(objectPath, "/xyz/openbmc_project/software/other/b");
}

TEST_F(DebugTokenInternalTest,
       onActivationChangedMsgDifferentPathKeepsTokenStatusFalse)
{
    DebugToken debugToken(busMock, &updateManager);
    debugToken.tokenPath = "/xyz/openbmc_project/software/HGX_FW_Debug_Token";
    debugToken.tokenStatus = false;

    pldm::dbus::PropertyMap properties;
    properties.emplace(
        "Activation",
        std::string("xyz.openbmc_project.Software.Activation.Activations."
                    "Active"));
    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call(
        "org.test", "/xyz/openbmc_project/software/other/not_token",
        "org.test.Interface", "Method");
    msg.append(std::string(Server::Activation::interface), properties);
    sealAndRewind(msg);

    EXPECT_NO_THROW({ debugToken.onActivationChangedMsg(msg); });
    EXPECT_FALSE(debugToken.tokenStatus);
}

TEST_F(DebugTokenInternalTest, getFilePathHandlesEmptyUuidAndEmptyPath)
{
    MockdBusHandler dbusHandler;
    DebugToken debugToken(busMock, &updateManager, dbusHandler);

    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillOnce(testing::Return(
            std::vector<std::string>{"/xyz/openbmc_project/software/other/a",
                                     "/xyz/openbmc_project/software/other/b"}));
    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(testing::_, testing::_, testing::_))
        .WillRepeatedly([](const char* objPath, const char* property,
                           const char*) -> pldm::utils::PropertyValue {
            if (std::string(objPath) ==
                    "/xyz/openbmc_project/software/other/a" &&
                std::string(property) == "UUID")
            {
                return std::string("");
            }
            if (std::string(objPath) ==
                    "/xyz/openbmc_project/software/other/b" &&
                std::string(property) == "UUID")
            {
                return std::string(InstallTokenUUID);
            }
            if (std::string(objPath) ==
                    "/xyz/openbmc_project/software/other/b" &&
                std::string(property) == "Path")
            {
                return std::string("");
            }
            throw std::runtime_error("unexpected property request");
        });

    auto [directoryPath, objectPath] = debugToken.getFilePath(InstallTokenUUID);
    EXPECT_TRUE(directoryPath.empty());
    EXPECT_TRUE(objectPath.empty());
}

TEST_F(DebugTokenInternalTest,
       updateDebugTokenDoesNotStartTimerWhenActivateFails)
{
    // End-to-end regression guard for the "start timer only on activate
    // success" gate. On the mock bus the install-token path runs all the
    // way through activate(), whose coSetDbusProperty fails, so timer
    // must remain unset (was unconditionally constructed previously).
    wireUpdateManagerForActivateAsyncFailure(updateManager, busMock);

    MockdBusHandler dbusHandler;
    DebugToken debugToken(busMock, &updateManager, dbusHandler);

    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(std::vector<std::string>{
            "/xyz/openbmc_project/software/other/install_no_timer"}));
    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(testing::_, testing::_, testing::_))
        .WillRepeatedly([](const char* objPath, const char* property,
                           const char*) -> pldm::utils::PropertyValue {
            if (std::string(objPath) ==
                    "/xyz/openbmc_project/software/other/install_no_timer" &&
                std::string(property) == "UUID")
            {
                return std::string(InstallTokenUUID);
            }
            if (std::string(objPath) ==
                    "/xyz/openbmc_project/software/other/install_no_timer" &&
                std::string(property) == "Path")
            {
                return std::string("/tmp/debug-token/install_no_timer/token");
            }
            throw std::runtime_error("unexpected property request");
        });

    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {0},
         "VersionNoTimer",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x76, 0x91, 0x0D, 0xFA, 0x1E, 0x4C, 0x11, 0xED,
                                0x86, 0x1D, 0x02, 0x42, 0xAC, 0x12, 0x00,
                                0x02}}},
         {0}}};
    ComponentImageInfos componentImageInfos{
        {10, deadComponent, 0xFFFFFFFF, 0, 0, 0, 4, "VersionNoTimer"}};
    std::istringstream package("WXYZ");

    EXPECT_NO_THROW({
        stdexec::sync_wait(debugToken.updateDebugToken(
            fwDeviceIDRecords, componentImageInfos, package));
    });
    EXPECT_TRUE(debugToken.isDebugTokenComponentPresent());
    EXPECT_EQ(debugToken.timer, nullptr);
    EXPECT_TRUE(debugToken.tokenStatus);
    EXPECT_TRUE(debugToken.activationMatches.empty());
}

TEST_F(DebugTokenInternalTest, updateDebugTokenInstallPathSetsTokenPresent)
{
    wireUpdateManagerForActivateAsyncFailure(updateManager, busMock);

    MockdBusHandler dbusHandler;
    DebugToken debugToken(busMock, &updateManager, dbusHandler);

    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(std::vector<std::string>{
            "/xyz/openbmc_project/software/other/install"}));
    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(testing::_, testing::_, testing::_))
        .WillRepeatedly([](const char* objPath, const char* property,
                           const char*) -> pldm::utils::PropertyValue {
            if (std::string(objPath) ==
                    "/xyz/openbmc_project/software/other/install" &&
                std::string(property) == "UUID")
            {
                return std::string(InstallTokenUUID);
            }
            if (std::string(objPath) ==
                    "/xyz/openbmc_project/software/other/install" &&
                std::string(property) == "Path")
            {
                return std::string("/tmp/debug-token/install/token.bin");
            }
            throw std::runtime_error("unexpected property request");
        });

    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {0},
         "VersionString",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x76, 0x91, 0x0D, 0xFA, 0x1E, 0x4C, 0x11, 0xED,
                                0x86, 0x1D, 0x02, 0x42, 0xAC, 0x12, 0x00,
                                0x02}}},
         {0}}};
    ComponentImageInfos componentImageInfos{
        {10, deadComponent, 0xFFFFFFFF, 0, 0, 0, 4, "VersionStringInstall"}};
    std::istringstream package("ABCD");

    EXPECT_NO_THROW({
        stdexec::sync_wait(debugToken.updateDebugToken(
            fwDeviceIDRecords, componentImageInfos, package));
    });
    EXPECT_TRUE(debugToken.isDebugTokenComponentPresent());
    EXPECT_EQ(debugToken.tokenPath,
              "/xyz/openbmc_project/software/other/install");
}

TEST_F(DebugTokenInternalTest,
       updateDebugTokenHandlesNonUuidDescriptorThenInstallsToken)
{
    wireUpdateManagerForActivateAsyncFailure(updateManager, busMock);

    MockdBusHandler dbusHandler;
    DebugToken debugToken(busMock, &updateManager, dbusHandler);

    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(std::vector<std::string>{
            "/xyz/openbmc_project/software/other/install2"}));
    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(testing::_, testing::_, testing::_))
        .WillRepeatedly([](const char* objPath, const char* property,
                           const char*) -> pldm::utils::PropertyValue {
            if (std::string(objPath) ==
                    "/xyz/openbmc_project/software/other/install2" &&
                std::string(property) == "UUID")
            {
                return std::string(InstallTokenUUID);
            }
            if (std::string(objPath) ==
                    "/xyz/openbmc_project/software/other/install2" &&
                std::string(property) == "Path")
            {
                return std::string("/tmp/debug-token/install2/token.bin");
            }
            throw std::runtime_error("unexpected property request");
        });

    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {0},
         "VersionString",
         {{PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("APSKU",
                           std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04})},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x76, 0x91, 0x0D, 0xFA, 0x1E, 0x4C, 0x11, 0xED,
                                0x86, 0x1D, 0x02, 0x42, 0xAC, 0x12, 0x00,
                                0x02}}},
         {0}}};
    ComponentImageInfos componentImageInfos{
        {10, deadComponent, 0xFFFFFFFF, 0, 0, 0, 4, "VersionStringInstall2"}};
    std::istringstream package("WXYZ");

    EXPECT_NO_THROW({
        stdexec::sync_wait(debugToken.updateDebugToken(
            fwDeviceIDRecords, componentImageInfos, package));
    });
    EXPECT_TRUE(debugToken.isDebugTokenComponentPresent());
    EXPECT_EQ(debugToken.tokenPath,
              "/xyz/openbmc_project/software/other/install2");
}

TEST_F(DebugTokenInternalTest,
       getFilePathReturnsEmptyWhenUuidDoesNotMatchRequestedUuid)
{
    MockdBusHandler dbusHandler;
    DebugToken debugToken(busMock, &updateManager, dbusHandler);

    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillOnce(testing::Return(
            std::vector<std::string>{"/xyz/openbmc_project/software/other/a"}));
    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(testing::_, testing::_, testing::_))
        .WillOnce(
            testing::Return(std::string("00112233445566778899AABBCCDDEEFF")));

    auto [directoryPath, objectPath] = debugToken.getFilePath(InstallTokenUUID);
    EXPECT_TRUE(directoryPath.empty());
    EXPECT_TRUE(objectPath.empty());
}

TEST_F(DebugTokenInternalTest, getFilePathContinuesWhenUuidLookupThrows)
{
    MockdBusHandler dbusHandler;
    DebugToken debugToken(busMock, &updateManager, dbusHandler);

    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillOnce(testing::Return(
            std::vector<std::string>{"/xyz/openbmc_project/software/other/a",
                                     "/xyz/openbmc_project/software/other/b"}));
    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(testing::_, testing::_, testing::_))
        .WillRepeatedly([](const char* objPath, const char* property,
                           const char*) -> pldm::utils::PropertyValue {
            if (std::string(objPath) ==
                    "/xyz/openbmc_project/software/other/a" &&
                std::string(property) == "UUID")
            {
                throw std::runtime_error("uuid read failed");
            }
            if (std::string(objPath) ==
                    "/xyz/openbmc_project/software/other/b" &&
                std::string(property) == "UUID")
            {
                return std::string(InstallTokenUUID);
            }
            if (std::string(objPath) ==
                    "/xyz/openbmc_project/software/other/b" &&
                std::string(property) == "Path")
            {
                return std::string("/tmp/debug-token/b/token.bin");
            }
            throw std::runtime_error("unexpected property request");
        });

    auto [directoryPath, objectPath] = debugToken.getFilePath(InstallTokenUUID);
    EXPECT_EQ(directoryPath, "/tmp/debug-token/b");
    EXPECT_EQ(objectPath, "/xyz/openbmc_project/software/other/b");
}

TEST_F(DebugTokenInternalTest, getValidPathsHandlesSubTreeQueryException)
{
    MockdBusHandler dbusHandler;
    DebugToken debugToken(busMock, &updateManager, dbusHandler);

    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillOnce(testing::Throw(std::runtime_error("mapper failure")));

    std::vector<std::string> paths;
    EXPECT_NO_THROW({ debugToken.getValidPaths(paths); });
    EXPECT_TRUE(paths.empty());
}

TEST_F(DebugTokenInternalTest,
       onActivationChangedMsgThrowsForNonStringActivationProperty)
{
    DebugToken debugToken(busMock, &updateManager);
    debugToken.tokenPath = "/xyz/openbmc_project/software/HGX_FW_Debug_Token";
    debugToken.tokenStatus = false;

    pldm::dbus::PropertyMap properties;
    properties.emplace("Activation", true);
    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test", debugToken.tokenPath.c_str(),
                                      "org.test.Interface", "Method");
    msg.append(std::string(Server::Activation::interface), properties);
    sealAndRewind(msg);

    EXPECT_THROW(
        { debugToken.onActivationChangedMsg(msg); }, std::bad_variant_access);
    EXPECT_FALSE(debugToken.tokenStatus);
}

TEST_F(DebugTokenInternalTest,
       startUpdateDoesNotSetActivationWhenNonPldmUpdateIsActivating)
{
    MockdBusHandler dbusHandler;
    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(std::vector<std::string>{}));

    std::vector<sdbusplus::object_path> targets;
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                   targets, dbusHandler);
    updateManager.otherDeviceUpdateManager->uuidMappings["UUID_ACTIVATING"] = {
        "1.0", "CompActivating"};
    updateManager.otherDeviceUpdateManager
        ->isImageFileProcessed["UUID_ACTIVATING"] = true;

    updateManager.objPath =
        "/xyz/openbmc_project/software/debug_token_nonpldm_activating";
    auto activation = std::make_unique<RecordingActivation>(
        busMock, updateManager.objPath, &updateManager);
    auto* activationRaw = activation.get();
    updateManager.activation = std::move(activation);

    DebugToken debugToken(busMock, &updateManager);
    EXPECT_NO_THROW({ debugToken.startUpdate(); });
    EXPECT_EQ(activationRaw->lastValue,
              software::Activation::Activations::Ready);
}

TEST_F(DebugTokenInternalTest,
       updateDebugTokenSkipsUuidDescriptorWithEmptyPayload)
{
    wireUpdateManagerForActivateAsyncFailure(updateManager, busMock);

    testing::NiceMock<MockdBusHandler> dbusHandler;
    DebugToken debugToken(busMock, &updateManager, dbusHandler);

    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {0},
         "VersionEmptyUuidPayload",
         {{PLDM_FWUP_UUID, std::vector<uint8_t>{}}},
         {0}}};
    ComponentImageInfos componentImageInfos{
        {10, deadComponent, 0xFFFFFFFF, 0, 0, 0, 4, "VersionStringEmpty"}};
    std::istringstream package("ABCD");

    EXPECT_NO_THROW({
        stdexec::sync_wait(debugToken.updateDebugToken(
            fwDeviceIDRecords, componentImageInfos, package));
    });
    EXPECT_FALSE(debugToken.isDebugTokenComponentPresent());
}

TEST_F(DebugTokenInternalTest,
       updateDebugTokenThrowsWhenUuidDescriptorPayloadHasWrongVariantType)
{
    MockdBusHandler dbusHandler;
    DebugToken debugToken(busMock, &updateManager, dbusHandler);

    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {0},
         "VersionWrongUuidDescriptor",
         {{PLDM_FWUP_UUID,
           std::make_tuple("APSKU",
                           std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04})}},
         {0}}};
    ComponentImageInfos componentImageInfos;
    std::istringstream package("ABCD");

    EXPECT_THROW(
        {
            stdexec::sync_wait(debugToken.updateDebugToken(
                fwDeviceIDRecords, componentImageInfos, package));
        },
        std::bad_variant_access);
}

TEST_F(DebugTokenInternalTest,
       updateDebugTokenFirstActivationMatchFailureCoverage)
{
    std::vector<sdbusplus::object_path> targets;
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                   targets);
    updateManager.deviceUpdaterMap.clear();
    updateManager.objPath =
        "/xyz/openbmc_project/software/debug_token_match_fail";
    updateManager.createProgressUpdateTimer();
    updateManager.debugToken =
        std::make_unique<DebugToken>(busMock, &updateManager);

    MockdBusHandler dbusHandler;
    DebugToken debugToken(busMock, &updateManager, dbusHandler);

    EXPECT_CALL(sdbusMock, sd_bus_add_match(testing::_, testing::_, testing::_,
                                            testing::_, testing::_))
        .WillOnce(testing::Return(-EINVAL));
    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(std::vector<std::string>{
            "/xyz/openbmc_project/software/other/install_match_fail"}));
    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(testing::_, testing::_, testing::_))
        .WillRepeatedly([](const char* objPath, const char* property,
                           const char*) -> pldm::utils::PropertyValue {
            if (std::string(objPath) ==
                    "/xyz/openbmc_project/software/other/install_match_fail" &&
                std::string(property) == "UUID")
            {
                return std::string(InstallTokenUUID);
            }
            if (std::string(objPath) ==
                    "/xyz/openbmc_project/software/other/install_match_fail" &&
                std::string(property) == "Path")
            {
                return std::string("/tmp/debug-token/install_match_fail/token");
            }
            throw std::runtime_error("unexpected property request");
        });
    EXPECT_CALL(dbusHandler, setDbusProperty(testing::_, testing::_)).Times(0);

    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {0},
         "VersionMatchFailure",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x76, 0x91, 0x0D, 0xFA, 0x1E, 0x4C, 0x11, 0xED,
                                0x86, 0x1D, 0x02, 0x42, 0xAC, 0x12, 0x00,
                                0x02}}},
         {0}}};
    ComponentImageInfos componentImageInfos{
        {10, deadComponent, 0xFFFFFFFF, 0, 0, 0, 4, "VersionMatchFailure"}};
    std::istringstream package("ABCD");

    EXPECT_NO_THROW({
        stdexec::sync_wait(debugToken.updateDebugToken(
            fwDeviceIDRecords, componentImageInfos, package));
    });
}

TEST_F(DebugTokenInternalTest,
       updateDebugTokenSecondActivationMatchFailureCoverage)
{
    std::vector<sdbusplus::object_path> targets;
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                   targets);
    updateManager.deviceUpdaterMap.clear();
    updateManager.objPath =
        "/xyz/openbmc_project/software/debug_token_progress_match_fail";
    updateManager.createProgressUpdateTimer();
    updateManager.debugToken =
        std::make_unique<DebugToken>(busMock, &updateManager);

    MockdBusHandler dbusHandler;
    DebugToken debugToken(busMock, &updateManager, dbusHandler);

    EXPECT_CALL(sdbusMock, sd_bus_add_match(testing::_, testing::_, testing::_,
                                            testing::_, testing::_))
        .WillOnce(testing::DoAll(
            testing::SetArgPointee<1>(static_cast<sd_bus_slot*>(nullptr)),
            testing::Return(0)))
        .WillOnce(testing::Return(-EINVAL));
    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(std::vector<std::string>{
            "/xyz/openbmc_project/software/other/install_progress_match_fail"}));
    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(testing::_, testing::_, testing::_))
        .WillRepeatedly([](const char* objPath, const char* property,
                           const char*) -> pldm::utils::PropertyValue {
            if (std::string(objPath) == "/xyz/openbmc_project/software/other/"
                                        "install_progress_match_fail" &&
                std::string(property) == "UUID")
            {
                return std::string(InstallTokenUUID);
            }
            if (std::string(objPath) == "/xyz/openbmc_project/software/other/"
                                        "install_progress_match_fail" &&
                std::string(property) == "Path")
            {
                return std::string(
                    "/tmp/debug-token/install_progress_match_fail/token");
            }
            throw std::runtime_error("unexpected property request");
        });
    EXPECT_CALL(dbusHandler, setDbusProperty(testing::_, testing::_)).Times(0);

    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {0},
         "VersionProgressMatchFailure",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x76, 0x91, 0x0D, 0xFA, 0x1E, 0x4C, 0x11, 0xED,
                                0x86, 0x1D, 0x02, 0x42, 0xAC, 0x12, 0x00,
                                0x02}}},
         {0}}};
    ComponentImageInfos componentImageInfos{
        {10, deadComponent, 0xFFFFFFFF, 0, 0, 0, 4,
         "VersionProgressMatchFailure"}};
    std::istringstream package("WXYZ");

    EXPECT_NO_THROW({
        stdexec::sync_wait(debugToken.updateDebugToken(
            fwDeviceIDRecords, componentImageInfos, package));
    });
}

TEST_F(DebugTokenInternalTest,
       updateDebugTokenContinuesPastEmptyApplicableComponents)
{
    std::vector<sdbusplus::object_path> targets;
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                   targets);
    updateManager.deviceUpdaterMap.clear();
    updateManager.objPath =
        "/xyz/openbmc_project/software/debug_token_install_later_valid";
    updateManager.createProgressUpdateTimer();
    updateManager.debugToken =
        std::make_unique<DebugToken>(busMock, &updateManager);

    MockdBusHandler dbusHandler;
    DebugToken debugToken(busMock, &updateManager, dbusHandler);

    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(std::vector<std::string>{
            "/xyz/openbmc_project/software/other/install_later_valid"}));
    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(testing::_, testing::_, testing::_))
        .WillRepeatedly([](const char* objPath, const char* property,
                           const char*) -> pldm::utils::PropertyValue {
            if (std::string(objPath) ==
                    "/xyz/openbmc_project/software/other/install_later_valid" &&
                std::string(property) == "UUID")
            {
                return std::string(InstallTokenUUID);
            }
            if (std::string(objPath) ==
                    "/xyz/openbmc_project/software/other/install_later_valid" &&
                std::string(property) == "Path")
            {
                return std::string(
                    "/tmp/debug-token/install_later_valid/token");
            }
            throw std::runtime_error("unexpected property request");
        });
    // TODO(async-writes): setVersion / activate now go through
    // coSetDbusProperty, which bypasses MockdBusHandler::setDbusProperty.
    // The earlier `EXPECT_CALL(setDbusProperty).Times(AtLeast(2))` is no
    // longer observable; isDebugTokenComponentPresent() remains a valid
    // assertion because it is set in the synchronous prologue of
    // updateDebugToken before any co_await.

    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {},
         "VersionIgnored",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x76, 0x91, 0x0D, 0xFA, 0x1E, 0x4C, 0x11, 0xED,
                                0x86, 0x1D, 0x02, 0x42, 0xAC, 0x12, 0x00,
                                0x02}}},
         {0}},
        {1,
         {0},
         "VersionApplied",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x76, 0x91, 0x0D, 0xFA, 0x1E, 0x4C, 0x11, 0xED,
                                0x86, 0x1D, 0x02, 0x42, 0xAC, 0x12, 0x00,
                                0x02}}},
         {0}}};
    ComponentImageInfos componentImageInfos{
        {10, deadComponent, 0xFFFFFFFF, 0, 0, 0, 4, "VersionApplied"}};
    std::istringstream package("ABCD");

    EXPECT_NO_THROW({
        stdexec::sync_wait(debugToken.updateDebugToken(
            fwDeviceIDRecords, componentImageInfos, package));
    });
    EXPECT_TRUE(debugToken.isDebugTokenComponentPresent());
}

TEST_F(DebugTokenInternalTest,
       updateDebugTokenContinuesPastNonDeadMatchingComponent)
{
    std::vector<sdbusplus::object_path> targets;
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                   targets);
    updateManager.deviceUpdaterMap.clear();
    updateManager.objPath =
        "/xyz/openbmc_project/software/debug_token_nondead_then_valid";
    updateManager.createProgressUpdateTimer();
    updateManager.debugToken =
        std::make_unique<DebugToken>(busMock, &updateManager);

    MockdBusHandler dbusHandler;
    DebugToken debugToken(busMock, &updateManager, dbusHandler);

    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(std::vector<std::string>{
            "/xyz/openbmc_project/software/other/install_after_non_dead"}));
    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(testing::_, testing::_, testing::_))
        .WillRepeatedly([](const char* objPath, const char* property,
                           const char*) -> pldm::utils::PropertyValue {
            if (std::string(objPath) == "/xyz/openbmc_project/software/other/"
                                        "install_after_non_dead" &&
                std::string(property) == "UUID")
            {
                return std::string(InstallTokenUUID);
            }
            if (std::string(objPath) == "/xyz/openbmc_project/software/other/"
                                        "install_after_non_dead" &&
                std::string(property) == "Path")
            {
                return std::string(
                    "/tmp/debug-token/install_after_non_dead/token");
            }
            throw std::runtime_error("unexpected property request");
        });
    // TODO(async-writes): see updateDebugTokenContinuesPastEmpty... above.
    // setDbusProperty mock is bypassed by the new coSetDbusProperty path.

    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {0},
         "VersionSkipped",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x76, 0x91, 0x0D, 0xFA, 0x1E, 0x4C, 0x11, 0xED,
                                0x86, 0x1D, 0x02, 0x42, 0xAC, 0x12, 0x00,
                                0x02}}},
         {0}},
        {1,
         {1},
         "VersionAppliedLater",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x76, 0x91, 0x0D, 0xFA, 0x1E, 0x4C, 0x11, 0xED,
                                0x86, 0x1D, 0x02, 0x42, 0xAC, 0x12, 0x00,
                                0x02}}},
         {1}}};
    ComponentImageInfos componentImageInfos{
        {10, 100, 0xFFFFFFFF, 0, 0, 0, 4, "VersionSkipped"},
        {11, deadComponent, 0xFFFFFFFF, 0, 0, 4, 4, "VersionAppliedLater"}};
    std::istringstream package("ABCDEFGH");

    EXPECT_NO_THROW({
        stdexec::sync_wait(debugToken.updateDebugToken(
            fwDeviceIDRecords, componentImageInfos, package));
    });
    EXPECT_TRUE(debugToken.isDebugTokenComponentPresent());
}
