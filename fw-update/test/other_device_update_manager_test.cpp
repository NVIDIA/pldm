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
#include "libpldm/firmware_update.h"

#include "common/test/mocked_utils.hpp"
#include "fw-update/other_device_update_manager.hpp"
#include "fw-update/update_manager.hpp"
#include "requester/handler.hpp"
#include "requester/test/mock_request.hpp"
#include "test/test_instance_id.hpp"

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/sdbus.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldm;
using namespace pldm::fw_update;
using namespace std::chrono;

class OtherDeviceUpdateManagerTest : public testing::Test
{
  protected:
    OtherDeviceUpdateManagerTest() :
        busMock(sdbusplus::get_mocked_new(&sdbusMock)),
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                      componentInfoMap, componentNameMap, true, nullptr)
    {}

    void swapStaticDbusBus()
    {
        auto& staticBus = pldm::utils::DBusHandler::getBus();
        savedStaticBus.emplace(std::move(staticBus));
        staticBus = sdbusplus::get_mocked_new(&sdbusMock);
        staticBusSwapped = true;
    }

    void TearDown() override
    {
        if (staticBusSwapped)
        {
            pldm::utils::DBusHandler::getBus() = std::move(*savedStaticBus);
            savedStaticBus.reset();
            staticBusSwapped = false;
        }
    }

    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    sdbusplus::bus::bus busMock;
    std::vector<sdbusplus::message::object_path> updatePolicyTargets;
    std::optional<sdbusplus::bus_t> savedStaticBus;
    bool staticBusSwapped = false;
    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    requester::Handler<requester::Request> reqHandler;
    DescriptorMap descriptorMap;
    ComponentInfoMap componentInfoMap;
    ComponentNameMap componentNameMap;
    UpdateManager updateManager;
};

static void sealAndRewind(sdbusplus::message::message& msg)
{
    sd_bus_message_seal(msg.get(), 0, 0);
    sd_bus_message_rewind(msg.get(), true);
}

TEST_F(OtherDeviceUpdateManagerTest, activate)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);

    bool result = otherDeviceUpdateManager.activate();

    EXPECT_EQ(result, true);
}

TEST_F(OtherDeviceUpdateManagerTest, onActivationChangedMsg)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);

    std::variant<bool, uint8_t, int16_t, uint16_t, int32_t, uint32_t, int64_t,
                 uint64_t, double, std::string, std::vector<uint8_t>,
                 std::vector<uint64_t>>
        value{"test"};

    pldm::dbus::PropertyMap properties;

    properties.insert(
        std::pair<std::string,
                  std::variant<bool, uint8_t, int16_t, uint16_t, int32_t,
                               uint32_t, int64_t, uint64_t, double, std::string,
                               std::vector<uint8_t>, std::vector<uint64_t>>>(
            "/xyz/openbmc_project/pldm", value));

    EXPECT_NO_THROW({
        otherDeviceUpdateManager.onActivationChanged(
            "/xyz/openbmc_project/pldm", properties);
    });
}

TEST_F(OtherDeviceUpdateManagerTest, setUpdatePolicy)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    swapStaticDbusBus();
    EXPECT_CALL(sdbusMock, sd_bus_call(testing::_, testing::_, dbusTimeout,
                                       testing::_, testing::_))
        .WillOnce([](sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*,
                     sd_bus_message** reply) {
            if (reply != nullptr)
            {
                *reply = nullptr;
            }
            return -EINVAL;
        });

    bool result =
        otherDeviceUpdateManager.setUpdatePolicy("/xyz/openbmc_project/pldm");

    EXPECT_EQ(result, false);
}

TEST_F(OtherDeviceUpdateManagerTest, getNumberOfProcessedImages)
{
    int expectedResult(0);

    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);

    int result = otherDeviceUpdateManager.getNumberOfProcessedImages();

    EXPECT_EQ(result, expectedResult);
}

TEST_F(OtherDeviceUpdateManagerTest, getValidTargets)
{
    int expectedResult(0);

    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);

    size_t result = otherDeviceUpdateManager.getValidTargets();

    EXPECT_EQ(result, expectedResult);
}

TEST_F(OtherDeviceUpdateManagerTest, extractOtherDevicePkgs)
{
    int expectedResult(0);

    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);

    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {0},
         "VersionString2",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}}},
         {}},
    };

    ComponentImageInfos compImageInfos{
        {10, 100, 0xFFFFFFFF, 0, 0, 139, 27, "VersionString2"}};

    std::istringstream dummyStream("10 20 30 40");

    size_t result = otherDeviceUpdateManager.extractOtherDevicePkgs(
        fwDeviceIDRecords, compImageInfos, dummyStream);

    EXPECT_EQ(result, expectedResult);
}

TEST_F(OtherDeviceUpdateManagerTest, fetchDescriptorsFromPackage_ValidUuidSku)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);

    FirmwareDeviceIDRecord fwDeviceIDRecord = {
        1,
        {0x00},
        "VersionString2",
        {{PLDM_FWUP_UUID,
          std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                               0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6, 0x75}},
         {PLDM_FWUP_VENDOR_DEFINED,
          std::make_tuple("APSKU",
                          std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04})}},
        {}};

    auto result =
        otherDeviceUpdateManager.fetchDescriptorsFromPackage(fwDeviceIDRecord);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->first, "162023C93EC5411595F448701D49D675");
    EXPECT_EQ(result->second, "0X01020304");
}

TEST_F(OtherDeviceUpdateManagerTest,
       fetchDescriptorsFromPackage_InvalidSkuDescriptorSize)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);

    FirmwareDeviceIDRecord fwDeviceIDRecord = {
        1,
        {0x00},
        "VersionString2",
        {{PLDM_FWUP_UUID,
          std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                               0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6, 0x75}},
         {PLDM_FWUP_VENDOR_DEFINED,
          std::make_tuple("APSKU", std::vector<uint8_t>{0x01, 0x02, 0x03})}},
        {}};

    auto result =
        otherDeviceUpdateManager.fetchDescriptorsFromPackage(fwDeviceIDRecord);
    EXPECT_FALSE(result.has_value());
}

TEST_F(OtherDeviceUpdateManagerTest, txComponentImageDeadComponentIsSkipped)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    std::istringstream package("0123456789");

    ComponentImageInfo deadCompInfo = {10, deadComponent,   0xFFFFFFFF, 0, 0, 0,
                                       4,  "VersionString2"};
    auto state = otherDeviceUpdateManager.txComponentImage(
        "/tmp/should_not_be_created", deadCompInfo, package);

    EXPECT_EQ(state, TransferPackageState::SKIPPED);
}

TEST_F(OtherDeviceUpdateManagerTest, txComponentImageTruncatedPackageFails)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    std::istringstream package("1234");

    ComponentImageInfo compInfo = {10, 100, 0xFFFFFFFF, 0,
                                   0,  2,   8,          "VersionString2"};
    auto state = otherDeviceUpdateManager.txComponentImage(
        "/tmp/tx_component_truncated", compInfo, package);

    EXPECT_EQ(state, TransferPackageState::FAILED);
}

TEST_F(OtherDeviceUpdateManagerTest, txComponentImageSuccessWritesBytes)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    std::istringstream package("ABCDEFGH");

    auto outPath = (std::filesystem::temp_directory_path() /
                    "pldm_other_device_tx_component_image_test.bin")
                       .string();
    ComponentImageInfo compInfo = {10, 100, 0xFFFFFFFF, 0,
                                   0,  2,   4,          "VersionString2"};

    auto state =
        otherDeviceUpdateManager.txComponentImage(outPath, compInfo, package);
    EXPECT_EQ(state, TransferPackageState::SUCCESS);

    std::ifstream written(outPath, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(written)),
                     std::istreambuf_iterator<char>());
    EXPECT_EQ(data, "CDEF");
    std::filesystem::remove(outPath);
}

TEST_F(OtherDeviceUpdateManagerTest, txMultipleComponentsReturnsFailedOnSkip)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    std::istringstream package("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    std::string dirPath = std::filesystem::temp_directory_path().string();

    ComponentImageInfos compImageInfos = {
        {10, deadComponent, 0xFFFFFFFF, 0, 0, 0, 4, "VersionString2"},
        {10, 100, 0xFFFFFFFF, 0, 0, 4, 4, "VersionString3"}};
    ApplicableComponents applicableCompVec = {0, 1};

    auto state = otherDeviceUpdateManager.txMultipleComponents(
        dirPath, applicableCompVec, compImageInfos, package,
        "/xyz/openbmc_project/software/other/test", "TESTUUID");
    EXPECT_EQ(state, TransferPackageState::FAILED);
}

TEST_F(OtherDeviceUpdateManagerTest,
       txMultipleComponentsReturnsSuccessForValidComponents)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    std::istringstream package("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    auto rootDir = std::filesystem::temp_directory_path() /
                   "pldm_other_device_tx_multiple_success";
    std::filesystem::create_directories(rootDir / "100");
    std::filesystem::create_directories(rootDir / "101");

    ComponentImageInfos compImageInfos = {
        {10, 100, 0xFFFFFFFF, 0, 0, 0, 4, "VersionString2"},
        {10, 101, 0xFFFFFFFF, 0, 0, 4, 4, "VersionString3"}};
    ApplicableComponents applicableCompVec = {0, 1};

    auto state = otherDeviceUpdateManager.txMultipleComponents(
        rootDir.string(), applicableCompVec, compImageInfos, package,
        "/xyz/openbmc_project/software/other/test", "TESTUUID");
    EXPECT_EQ(state, TransferPackageState::SUCCESS);
    EXPECT_TRUE(otherDeviceUpdateManager.uuidMappings.contains("TESTUUID"));
    std::filesystem::remove_all(rootDir);
}

TEST_F(OtherDeviceUpdateManagerTest, getOverAllActivationStateTransitions)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    auto activeEntry = std::make_unique<OtherDeviceUpdateActivation>();
    activeEntry->activationState = Server::Activation::Activations::Active;
    auto failedEntry = std::make_unique<OtherDeviceUpdateActivation>();
    failedEntry->activationState = Server::Activation::Activations::Failed;
    auto activatingEntry = std::make_unique<OtherDeviceUpdateActivation>();
    activatingEntry->activationState =
        Server::Activation::Activations::Activating;

    otherDeviceUpdateManager.otherDevices["/active"] = std::move(activeEntry);
    otherDeviceUpdateManager.otherDevices["/failed"] = std::move(failedEntry);
    EXPECT_EQ(otherDeviceUpdateManager.getOverAllActivationState(),
              Server::Activation::Activations::Failed);

    otherDeviceUpdateManager.otherDevices["/activating"] =
        std::move(activatingEntry);
    EXPECT_EQ(otherDeviceUpdateManager.getOverAllActivationState(),
              Server::Activation::Activations::Activating);
}

TEST_F(OtherDeviceUpdateManagerTest, onActivationChangedUpdatesTrackedEntry)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    auto tracked = std::make_unique<OtherDeviceUpdateActivation>();
    tracked->activationState = Server::Activation::Activations::Ready;
    tracked->requestedActivation =
        Server::Activation::RequestedActivations::None;
    otherDeviceUpdateManager.otherDevices["/xyz/openbmc_project/software/other/"
                                          "entry"] = std::move(tracked);

    pldm::dbus::PropertyMap properties;
    properties.emplace(
        "Activation",
        std::string("xyz.openbmc_project.Software.Activation.Activations."
                    "Active"));
    properties.emplace("RequestedActivation",
                       std::string("xyz.openbmc_project.Software.Activation."
                                   "RequestedActivations.Active"));

    otherDeviceUpdateManager.onActivationChanged(
        "/xyz/openbmc_project/software/other/entry", properties);

    EXPECT_EQ(otherDeviceUpdateManager
                  .otherDevices["/xyz/openbmc_project/software/other/entry"]
                  ->activationState,
              Server::Activation::Activations::Active);
    EXPECT_EQ(otherDeviceUpdateManager
                  .otherDevices["/xyz/openbmc_project/software/other/entry"]
                  ->requestedActivation,
              Server::Activation::RequestedActivations::Active);
}

TEST_F(OtherDeviceUpdateManagerTest,
       onActivationChangedMissingPropertiesLeavesStateUnchanged)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    const std::string objPath = "/xyz/openbmc_project/software/other/entry5";
    auto tracked = std::make_unique<OtherDeviceUpdateActivation>();
    tracked->activationState = Server::Activation::Activations::Ready;
    tracked->requestedActivation =
        Server::Activation::RequestedActivations::None;
    otherDeviceUpdateManager.otherDevices[objPath] = std::move(tracked);

    pldm::dbus::PropertyMap properties;
    properties.emplace("Unrelated", std::string("value"));
    otherDeviceUpdateManager.onActivationChanged(objPath, properties);
    otherDeviceUpdateManager.onActivationChanged(
        "/xyz/openbmc_project/software/other/not_present", properties);

    EXPECT_EQ(otherDeviceUpdateManager.otherDevices[objPath]->activationState,
              Server::Activation::Activations::Ready);
    EXPECT_EQ(
        otherDeviceUpdateManager.otherDevices[objPath]->requestedActivation,
        Server::Activation::RequestedActivations::None);
}

TEST_F(OtherDeviceUpdateManagerTest,
       onActivationChangedMsgActiveMarksOtherDeviceComplete)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    const std::string objPath = "/xyz/openbmc_project/software/other/entry2";
    const std::string uuid = "UUID_ENTRY2";

    auto tracked = std::make_unique<OtherDeviceUpdateActivation>();
    tracked->uuid = uuid;
    tracked->activationState = Server::Activation::Activations::Ready;
    tracked->requestedActivation =
        Server::Activation::RequestedActivations::None;
    otherDeviceUpdateManager.otherDevices[objPath] = std::move(tracked);
    otherDeviceUpdateManager.uuidMappings[uuid] = {"1.0", "CompEntry2"};

    updateManager.deviceUpdaterMap.emplace(0x1, nullptr);
    updateManager.totalNumComponentUpdates = 2;
    updateManager.compUpdateCompletedCount = 0;
    updateManager.otherDeviceComponents[uuid] = true;

    pldm::dbus::PropertyMap properties;
    properties.emplace(
        "Activation",
        std::string("xyz.openbmc_project.Software.Activation.Activations."
                    "Active"));

    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test", objPath.c_str(),
                                      "org.test.Interface", "Method");
    msg.append(std::string(Server::Activation::interface), properties);
    sealAndRewind(msg);

    EXPECT_NO_THROW({ otherDeviceUpdateManager.onActivationChangedMsg(msg); });
    EXPECT_TRUE(updateManager.otherDeviceCompleted.contains(uuid));
    EXPECT_TRUE(updateManager.otherDeviceCompleted[uuid]);
    EXPECT_NE(updateManager.listCompNames.find("CompEntry2"),
              std::string::npos);
}

TEST_F(OtherDeviceUpdateManagerTest, onActivationChangedMsgIgnoresUnknownPath)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    pldm::dbus::PropertyMap properties;
    properties.emplace(
        "Activation",
        std::string("xyz.openbmc_project.Software.Activation.Activations."
                    "Active"));

    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call(
        "org.test", "/xyz/openbmc_project/software/other/unknown",
        "org.test.Interface", "Method");
    msg.append(std::string(Server::Activation::interface), properties);
    sealAndRewind(msg);

    EXPECT_NO_THROW({ otherDeviceUpdateManager.onActivationChangedMsg(msg); });
    EXPECT_TRUE(updateManager.otherDeviceCompleted.empty());
}

TEST_F(OtherDeviceUpdateManagerTest, interfaceAddedAddsTrackedOtherDevice)
{
    MockdBusHandler dbusHandler;
    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(std::vector<std::string>{}));
    EXPECT_CALL(dbusHandler, setDbusProperty(testing::_, testing::_))
        .Times(testing::AtLeast(2));

    OtherDeviceUpdateManager otherDeviceUpdateManager(
        busMock, &updateManager, updatePolicyTargets, dbusHandler);
    otherDeviceUpdateManager.startWatchingInterfaceAddition();
    ASSERT_NE(otherDeviceUpdateManager.interfaceAddedMatch, nullptr);

    const std::string objPath = "/xyz/openbmc_project/software/other/new_entry";
    const std::string uuid = "AABBCCDDEEFF00112233445566778899";
    otherDeviceUpdateManager.uuidMappings[uuid] = {"2.0", "CompNew"};

    pldm::dbus::PropertyMap uuidProperties;
    uuidProperties.emplace("UUID", uuid);
    pldm::dbus::InterfaceMap interfaces;
    interfaces.emplace("xyz.openbmc_project.Common.UUID", uuidProperties);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test",
                                      "/xyz/openbmc_project/software/other",
                                      "org.test.Interface", "Method");
    msg.append(sdbusplus::message::object_path(objPath), interfaces);
    sealAndRewind(msg);

    EXPECT_NO_THROW({ otherDeviceUpdateManager.interfaceAdded(msg); });
    EXPECT_TRUE(otherDeviceUpdateManager.otherDevices.contains(objPath));
    EXPECT_TRUE(otherDeviceUpdateManager.isImageFileProcessed.contains(uuid));
}

TEST_F(OtherDeviceUpdateManagerTest, interfaceAddedSkipsExistingTrackedDevice)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    otherDeviceUpdateManager.startWatchingInterfaceAddition();
    ASSERT_NE(otherDeviceUpdateManager.interfaceAddedMatch, nullptr);

    const std::string objPath = "/xyz/openbmc_project/software/other/existing";
    const std::string existingUuid = "EXISTING_UUID";
    const std::string incomingUuid = "AABBCCDDEEFF00112233445566778899";
    auto tracked = std::make_unique<OtherDeviceUpdateActivation>();
    tracked->uuid = existingUuid;
    otherDeviceUpdateManager.otherDevices[objPath] = std::move(tracked);
    otherDeviceUpdateManager.uuidMappings[incomingUuid] = {"2.0", "CompNew"};

    pldm::dbus::PropertyMap uuidProperties;
    uuidProperties.emplace("UUID", incomingUuid);
    pldm::dbus::InterfaceMap interfaces;
    interfaces.emplace("xyz.openbmc_project.Common.UUID", uuidProperties);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test",
                                      "/xyz/openbmc_project/software/other",
                                      "org.test.Interface", "Method");
    msg.append(sdbusplus::message::object_path(objPath), interfaces);
    sealAndRewind(msg);

    EXPECT_NO_THROW({ otherDeviceUpdateManager.interfaceAdded(msg); });
    EXPECT_EQ(otherDeviceUpdateManager.otherDevices[objPath]->uuid,
              existingUuid);
}

TEST_F(OtherDeviceUpdateManagerTest, activateTrackedOtherDeviceFailurePath)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    const std::string path = "/xyz/openbmc_project/software/other/activate_1";
    const std::string uuid = "UUID_ACTIVATE_1";

    auto tracked = std::make_unique<OtherDeviceUpdateActivation>();
    tracked->uuid = uuid;
    tracked->activationState = Server::Activation::Activations::Ready;
    tracked->requestedActivation =
        Server::Activation::RequestedActivations::None;
    otherDeviceUpdateManager.otherDevices[path] = std::move(tracked);
    otherDeviceUpdateManager.uuidMappings[uuid] = {"3.0", "CompActivate"};

    updateManager.deviceUpdaterMap.emplace(0x1, nullptr);
    updateManager.totalNumComponentUpdates = 2;
    updateManager.compUpdateCompletedCount = 0;
    updateManager.otherDeviceComponents[uuid] = false;

    auto result = otherDeviceUpdateManager.activate();
    EXPECT_FALSE(result);
    EXPECT_TRUE(updateManager.otherDeviceCompleted.contains(uuid));
    EXPECT_FALSE(updateManager.otherDeviceCompleted[uuid]);
}

TEST_F(OtherDeviceUpdateManagerTest,
       onActivationChangedMsgFailedMarksOtherDeviceFailed)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    const std::string objPath = "/xyz/openbmc_project/software/other/entry3";
    const std::string uuid = "UUID_ENTRY3";

    auto tracked = std::make_unique<OtherDeviceUpdateActivation>();
    tracked->uuid = uuid;
    tracked->activationState = Server::Activation::Activations::Ready;
    tracked->requestedActivation =
        Server::Activation::RequestedActivations::None;
    otherDeviceUpdateManager.otherDevices[objPath] = std::move(tracked);
    otherDeviceUpdateManager.uuidMappings[uuid] = {"1.0", "CompEntry3"};
    otherDeviceUpdateManager.targets.clear();
    otherDeviceUpdateManager.targets.emplace_back(
        "/xyz/openbmc_project/software/other/not_matching");

    updateManager.deviceUpdaterMap.emplace(0x1, nullptr);
    updateManager.totalNumComponentUpdates = 2;
    updateManager.compUpdateCompletedCount = 0;
    updateManager.otherDeviceComponents[uuid] = false;

    pldm::dbus::PropertyMap properties;
    properties.emplace(
        "Activation",
        std::string("xyz.openbmc_project.Software.Activation.Activations."
                    "Failed"));
    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test", objPath.c_str(),
                                      "org.test.Interface", "Method");
    msg.append(std::string(Server::Activation::interface), properties);
    sealAndRewind(msg);

    EXPECT_NO_THROW({ otherDeviceUpdateManager.onActivationChangedMsg(msg); });
    EXPECT_TRUE(updateManager.otherDeviceCompleted.contains(uuid));
    EXPECT_FALSE(updateManager.otherDeviceCompleted[uuid]);
}

TEST_F(OtherDeviceUpdateManagerTest,
       onActivationChangedMsgActiveWithTargetMismatchSkipsCompName)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    const std::string objPath = "/xyz/openbmc_project/software/other/entry4";
    const std::string uuid = "UUID_ENTRY4";

    auto tracked = std::make_unique<OtherDeviceUpdateActivation>();
    tracked->uuid = uuid;
    tracked->activationState = Server::Activation::Activations::Ready;
    tracked->requestedActivation =
        Server::Activation::RequestedActivations::None;
    otherDeviceUpdateManager.otherDevices[objPath] = std::move(tracked);
    otherDeviceUpdateManager.uuidMappings[uuid] = {"1.0", "CompEntry4"};
    otherDeviceUpdateManager.targets.clear();
    otherDeviceUpdateManager.targets.emplace_back(
        "/xyz/openbmc_project/software/other/not_matching");

    updateManager.deviceUpdaterMap.emplace(0x1, nullptr);
    updateManager.totalNumComponentUpdates = 2;
    updateManager.compUpdateCompletedCount = 0;
    updateManager.otherDeviceComponents[uuid] = true;
    updateManager.listCompNames.clear();

    pldm::dbus::PropertyMap properties;
    properties.emplace(
        "Activation",
        std::string("xyz.openbmc_project.Software.Activation.Activations."
                    "Active"));
    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test", objPath.c_str(),
                                      "org.test.Interface", "Method");
    msg.append(std::string(Server::Activation::interface), properties);
    sealAndRewind(msg);

    EXPECT_NO_THROW({ otherDeviceUpdateManager.onActivationChangedMsg(msg); });
    EXPECT_TRUE(updateManager.otherDeviceCompleted.contains(uuid));
    EXPECT_TRUE(updateManager.otherDeviceCompleted[uuid]);
    EXPECT_TRUE(updateManager.listCompNames.empty());
}

TEST_F(OtherDeviceUpdateManagerTest, interfaceAddedReturnsWhenWatchNotStarted)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    const std::string objPath = "/xyz/openbmc_project/software/other/new_entry";
    const std::string uuid = "AABBCCDDEEFF00112233445566778899";

    pldm::dbus::PropertyMap uuidProperties;
    uuidProperties.emplace("UUID", uuid);
    pldm::dbus::InterfaceMap interfaces;
    interfaces.emplace("xyz.openbmc_project.Common.UUID", uuidProperties);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test",
                                      "/xyz/openbmc_project/software/other",
                                      "org.test.Interface", "Method");
    msg.append(sdbusplus::message::object_path(objPath), interfaces);
    sealAndRewind(msg);

    EXPECT_NO_THROW({ otherDeviceUpdateManager.interfaceAdded(msg); });
    EXPECT_FALSE(otherDeviceUpdateManager.otherDevices.contains(objPath));
}

TEST_F(OtherDeviceUpdateManagerTest,
       interfaceAddedWithoutUuidInterfaceClearsWatchOnEmptyPendingMap)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    otherDeviceUpdateManager.startWatchingInterfaceAddition();
    ASSERT_NE(otherDeviceUpdateManager.interfaceAddedMatch, nullptr);

    const std::string objPath = "/xyz/openbmc_project/software/other/new_entry";
    pldm::dbus::PropertyMap versionProperties;
    versionProperties.emplace("Version", std::string("1.0"));
    pldm::dbus::InterfaceMap interfaces;
    interfaces.emplace("xyz.openbmc_project.Software.Version",
                       versionProperties);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test",
                                      "/xyz/openbmc_project/software/other",
                                      "org.test.Interface", "Method");
    msg.append(sdbusplus::message::object_path(objPath), interfaces);
    sealAndRewind(msg);

    EXPECT_NO_THROW({ otherDeviceUpdateManager.interfaceAdded(msg); });
    EXPECT_EQ(otherDeviceUpdateManager.interfaceAddedMatch, nullptr);
}

TEST_F(OtherDeviceUpdateManagerTest, startTimerTimeoutMarksIncompleteAsFailed)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    otherDeviceUpdateManager.startWatchingInterfaceAddition();
    ASSERT_NE(otherDeviceUpdateManager.interfaceAddedMatch, nullptr);

    const std::string uuid = "UUID_TIMEOUT";
    otherDeviceUpdateManager.isImageFileProcessed[uuid] = false;
    otherDeviceUpdateManager.uuidMappings[uuid] = {"3.0", "CompTimeout"};

    updateManager.deviceUpdaterMap.emplace(0x1, nullptr);
    updateManager.totalNumComponentUpdates = 2;
    updateManager.compUpdateCompletedCount = 0;
    updateManager.otherDeviceComponents[uuid] = false;

    otherDeviceUpdateManager.startTimer(0);
    EXPECT_GE(sd_event_run(event.get(), 1000), 0);

    EXPECT_EQ(otherDeviceUpdateManager.interfaceAddedMatch, nullptr);
    EXPECT_TRUE(updateManager.otherDeviceCompleted.contains(uuid));
    EXPECT_FALSE(updateManager.otherDeviceCompleted[uuid]);
}

TEST_F(OtherDeviceUpdateManagerTest, setUpdatePolicyReturnsTrueWhenDbusSucceeds)
{
    MockdBusHandler dbusHandler;
    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(std::vector<std::string>{}));

    OtherDeviceUpdateManager otherDeviceUpdateManager(
        busMock, &updateManager, updatePolicyTargets, dbusHandler);

    EXPECT_CALL(dbusHandler, setDbusProperty(testing::_, testing::_)).Times(1);
    EXPECT_TRUE(otherDeviceUpdateManager.setUpdatePolicy(
        "/xyz/openbmc_project/software/other/new"));
}

TEST_F(OtherDeviceUpdateManagerTest,
       buildDeviceDescriptorMapAddsSkuAndUuidOnlyEntries)
{
    MockdBusHandler dbusHandler;
    const std::string objPath = "/xyz/openbmc_project/software/other/device_a";
    const std::string uuid = "aabbccddeeff00112233445566778899";
    const std::string sku = "0x01020304";

    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(std::vector<std::string>{objPath}));
    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(testing::_, testing::_, testing::_))
        .WillRepeatedly([&](const char* objectPath, const char* property,
                            const char*) -> pldm::utils::PropertyValue {
            if (std::string(objectPath) == objPath &&
                std::string(property) == "UUID")
            {
                return uuid;
            }
            if (std::string(objectPath) == objPath &&
                std::string(property) == "SKU")
            {
                return sku;
            }
            if (std::string(objectPath) == objPath &&
                std::string(property) == "Path")
            {
                return std::string("/tmp/other/device_a/token.bin");
            }
            throw std::runtime_error("unexpected property request");
        });

    OtherDeviceUpdateManager otherDeviceUpdateManager(
        busMock, &updateManager, updatePolicyTargets, dbusHandler);
    otherDeviceUpdateManager.buildDeviceDescriptorMap();

    const auto fullKey =
        std::make_pair(std::string("AABBCCDDEEFF00112233445566778899"),
                       std::string("0X01020304"));
    const auto uuidOnlyKey = std::make_pair(
        std::string("AABBCCDDEEFF00112233445566778899"), std::string(""));

    EXPECT_TRUE(
        otherDeviceUpdateManager.otherDeviceDescriptorMap.contains(fullKey));
    EXPECT_TRUE(otherDeviceUpdateManager.otherDeviceDescriptorMap.contains(
        uuidOnlyKey));
}

TEST_F(OtherDeviceUpdateManagerTest,
       buildDeviceDescriptorMapSkipsEntriesWithMissingPath)
{
    MockdBusHandler dbusHandler;
    const std::string objPathA = "/xyz/openbmc_project/software/other/device_a";
    const std::string objPathB = "/xyz/openbmc_project/software/other/device_b";

    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(
            testing::Return(std::vector<std::string>{objPathA, objPathB}));
    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(testing::_, testing::_, testing::_))
        .WillRepeatedly([&](const char* objectPath, const char* property,
                            const char*) -> pldm::utils::PropertyValue {
            if (std::string(property) == "UUID")
            {
                if (std::string(objectPath) == objPathA)
                {
                    return std::string("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
                }
                if (std::string(objectPath) == objPathB)
                {
                    return std::string("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
                }
            }
            if (std::string(property) == "SKU")
            {
                if (std::string(objectPath) == objPathA)
                {
                    return std::string("0xaaaaaaaa");
                }
                if (std::string(objectPath) == objPathB)
                {
                    throw std::runtime_error("sku missing");
                }
            }
            if (std::string(property) == "Path")
            {
                if (std::string(objectPath) == objPathA)
                {
                    throw std::runtime_error("path missing");
                }
                if (std::string(objectPath) == objPathB)
                {
                    return std::string("/tmp/other/device_b/token.bin");
                }
            }
            throw std::runtime_error("unexpected property request");
        });

    OtherDeviceUpdateManager otherDeviceUpdateManager(
        busMock, &updateManager, updatePolicyTargets, dbusHandler);
    otherDeviceUpdateManager.buildDeviceDescriptorMap();

    const auto keyA =
        std::make_pair(std::string("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
                       std::string("0XAAAAAAAA"));
    const auto keyB = std::make_pair(
        std::string("BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"), std::string(""));

    EXPECT_FALSE(
        otherDeviceUpdateManager.otherDeviceDescriptorMap.contains(keyA));
    EXPECT_TRUE(
        otherDeviceUpdateManager.otherDeviceDescriptorMap.contains(keyB));
}

TEST_F(OtherDeviceUpdateManagerTest,
       updateValidTargetsCountsOnlyNonEmptyUuidAndIgnoresErrors)
{
    MockdBusHandler dbusHandler;
    const std::string objPathA = "/xyz/openbmc_project/software/other/a";
    const std::string objPathB = "/xyz/openbmc_project/software/other/b";
    const std::string objPathC = "/xyz/openbmc_project/software/other/c";

    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(
            std::vector<std::string>{objPathA, objPathB, objPathC}));
    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(testing::_, testing::_, testing::_))
        .WillRepeatedly([&](const char* objectPath, const char* property,
                            const char*) -> pldm::utils::PropertyValue {
            if (std::string(property) != "UUID")
            {
                throw std::runtime_error("only UUID is expected");
            }
            if (std::string(objectPath) == objPathA)
            {
                return std::string("ABCDEF");
            }
            if (std::string(objectPath) == objPathB)
            {
                return std::string("");
            }
            if (std::string(objectPath) == objPathC)
            {
                throw std::runtime_error("uuid lookup failed");
            }
            throw std::runtime_error("unexpected object path");
        });

    OtherDeviceUpdateManager otherDeviceUpdateManager(
        busMock, &updateManager, updatePolicyTargets, dbusHandler);
    otherDeviceUpdateManager.updateValidTargets();

    EXPECT_EQ(otherDeviceUpdateManager.getValidTargets(), 1);
}

TEST_F(OtherDeviceUpdateManagerTest, activateReturnsTrueForTrackedDeviceSuccess)
{
    MockdBusHandler dbusHandler;
    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(std::vector<std::string>{}));
    EXPECT_CALL(dbusHandler, setDbusProperty(testing::_, testing::_)).Times(1);

    OtherDeviceUpdateManager otherDeviceUpdateManager(
        busMock, &updateManager, updatePolicyTargets, dbusHandler);

    auto tracked = std::make_unique<OtherDeviceUpdateActivation>();
    tracked->uuid = "UUID_ACTIVATE_OK";
    tracked->activationState = Server::Activation::Activations::Ready;
    tracked->requestedActivation =
        Server::Activation::RequestedActivations::None;
    otherDeviceUpdateManager.otherDevices["/xyz/openbmc_project/software/other/"
                                          "activate_ok"] = std::move(tracked);
    otherDeviceUpdateManager.uuidMappings["UUID_ACTIVATE_OK"] = {
        "1.0", "ComponentActivateOK"};

    EXPECT_TRUE(otherDeviceUpdateManager.activate());
}

TEST_F(OtherDeviceUpdateManagerTest,
       interfaceAddedSetsExtendedVersionAndPolicyWhenUuidAppears)
{
    MockdBusHandler dbusHandler;
    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(std::vector<std::string>{}));
    EXPECT_CALL(dbusHandler, setDbusProperty(testing::_, testing::_))
        .Times(testing::AtLeast(2));

    OtherDeviceUpdateManager otherDeviceUpdateManager(
        busMock, &updateManager, updatePolicyTargets, dbusHandler);
    otherDeviceUpdateManager.startWatchingInterfaceAddition();
    ASSERT_NE(otherDeviceUpdateManager.interfaceAddedMatch, nullptr);

    const std::string objPath = "/xyz/openbmc_project/software/other/new_uuid";
    const std::string uuid = "00112233445566778899AABBCCDDEEFF";
    otherDeviceUpdateManager.uuidMappings[uuid] = {"5.0", "CompAdded"};

    pldm::dbus::PropertyMap uuidProperties;
    uuidProperties.emplace("UUID", uuid);
    pldm::dbus::InterfaceMap interfaces;
    interfaces.emplace("xyz.openbmc_project.Common.UUID", uuidProperties);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test",
                                      "/xyz/openbmc_project/software/other",
                                      "org.test.Interface", "Method");
    msg.append(sdbusplus::message::object_path(objPath), interfaces);
    sealAndRewind(msg);

    EXPECT_NO_THROW({ otherDeviceUpdateManager.interfaceAdded(msg); });
    EXPECT_TRUE(otherDeviceUpdateManager.otherDevices.contains(objPath));
}

TEST_F(OtherDeviceUpdateManagerTest,
       extractOtherDevicePkgsProcessesSingleMatchingImage)
{
    MockdBusHandler dbusHandler;
    const std::string objPath = "/xyz/openbmc_project/software/other/match";
    const std::string uuid = "76910DFA1E4C11ED861D0242AC120002";
    const std::string sku = "0X01020304";

    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(std::vector<std::string>{objPath}));
    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(testing::_, testing::_, testing::_))
        .WillRepeatedly([&](const char* objectPath, const char* property,
                            const char*) -> pldm::utils::PropertyValue {
            if (std::string(objectPath) == objPath &&
                std::string(property) == "UUID")
            {
                return uuid;
            }
            if (std::string(objectPath) == objPath &&
                std::string(property) == "SKU")
            {
                return sku;
            }
            if (std::string(objectPath) == objPath &&
                std::string(property) == "Path")
            {
                return std::string("/tmp/other/match/token.bin");
            }
            throw std::runtime_error("unexpected property request");
        });

    OtherDeviceUpdateManager otherDeviceUpdateManager(
        busMock, &updateManager, updatePolicyTargets, dbusHandler);
    std::filesystem::create_directories("/tmp/other/match");

    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {0},
         "VersionString",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x76, 0x91, 0x0D, 0xFA, 0x1E, 0x4C, 0x11, 0xED,
                                0x86, 0x1D, 0x02, 0x42, 0xAC, 0x12, 0x00,
                                0x02}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("APSKU",
                           std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04})}},
         {0}}};
    ComponentImageInfos compImageInfos{
        {10, 100, 0xFFFFFFFF, 0, 0, 0, 4, "VersionString2"}};
    std::istringstream package("ABCD1234");

    auto result = otherDeviceUpdateManager.extractOtherDevicePkgs(
        fwDeviceIDRecords, compImageInfos, package);

    EXPECT_EQ(result, 1);
    EXPECT_TRUE(otherDeviceUpdateManager.isImageFileProcessed.contains(uuid));
    EXPECT_FALSE(otherDeviceUpdateManager.isImageFileProcessed[uuid]);
    std::filesystem::remove_all("/tmp/other/match");
}

TEST_F(OtherDeviceUpdateManagerTest,
       extractOtherDevicePkgsSkipsInvalidAndUnmatchedRecords)
{
    MockdBusHandler dbusHandler;
    const std::string objPath = "/xyz/openbmc_project/software/other/match2";
    const std::string uuid = "76910DFA1E4C11ED861D0242AC120002";
    const std::string sku = "0X01020304";

    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(std::vector<std::string>{objPath}));
    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(testing::_, testing::_, testing::_))
        .WillRepeatedly([&](const char* objectPath, const char* property,
                            const char*) -> pldm::utils::PropertyValue {
            if (std::string(objectPath) == objPath &&
                std::string(property) == "UUID")
            {
                return uuid;
            }
            if (std::string(objectPath) == objPath &&
                std::string(property) == "SKU")
            {
                return sku;
            }
            if (std::string(objectPath) == objPath &&
                std::string(property) == "Path")
            {
                return std::string("/tmp/other/match2/token.bin");
            }
            throw std::runtime_error("unexpected property request");
        });

    OtherDeviceUpdateManager otherDeviceUpdateManager(
        busMock, &updateManager, updatePolicyTargets, dbusHandler);
    std::filesystem::create_directories("/tmp/other/match2");

    FirmwareDeviceIDRecords fwDeviceIDRecords{
        // Invalid APSKU size => fetchDescriptorsFromPackage returns nullopt
        {1,
         {0},
         "VersionInvalidSku",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x76, 0x91, 0x0D, 0xFA, 0x1E, 0x4C, 0x11, 0xED,
                                0x86, 0x1D, 0x02, 0x42, 0xAC, 0x12, 0x00,
                                0x02}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("APSKU", std::vector<uint8_t>{0x01, 0x02, 0x03})}},
         {0}},
        // No UUID descriptor => empty UUID
        {1,
         {0},
         "VersionNoUuid",
         {{PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("APSKU",
                           std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04})}},
         {0}},
        // UUID+SKU not found in descriptor map
        {1,
         {0},
         "VersionNoMatch",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("APSKU",
                           std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04})}},
         {0}},
        // Matching descriptors but invalid applicable components
        {1,
         {},
         "VersionEmptyApplicable",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x76, 0x91, 0x0D, 0xFA, 0x1E, 0x4C, 0x11, 0xED,
                                0x86, 0x1D, 0x02, 0x42, 0xAC, 0x12, 0x00,
                                0x02}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("APSKU",
                           std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04})}},
         {}}};
    ComponentImageInfos compImageInfos{
        {10, 100, 0xFFFFFFFF, 0, 0, 0, 4, "VersionString2"}};
    std::istringstream package("ABCD1234");

    auto result = otherDeviceUpdateManager.extractOtherDevicePkgs(
        fwDeviceIDRecords, compImageInfos, package);

    EXPECT_EQ(result, 0);
    std::filesystem::remove_all("/tmp/other/match2");
}

TEST_F(OtherDeviceUpdateManagerTest,
       onActivationChangedMsgActiveWithTargetMismatchOmitsComponentName)
{
    std::vector<sdbusplus::message::object_path> targets{
        sdbusplus::message::object_path(
            "/xyz/openbmc_project/software/other/not_matching_target")};
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      targets);
    const std::string objPath = "/xyz/openbmc_project/software/other/entry6";
    const std::string uuid = "UUID_ENTRY6";

    auto tracked = std::make_unique<OtherDeviceUpdateActivation>();
    tracked->uuid = uuid;
    tracked->activationState = Server::Activation::Activations::Ready;
    tracked->requestedActivation =
        Server::Activation::RequestedActivations::None;
    otherDeviceUpdateManager.otherDevices[objPath] = std::move(tracked);
    otherDeviceUpdateManager.uuidMappings[uuid] = {"1.0", "CompEntry6"};

    updateManager.deviceUpdaterMap.emplace(0x1, nullptr);
    updateManager.totalNumComponentUpdates = 2;
    updateManager.compUpdateCompletedCount = 0;
    updateManager.otherDeviceComponents[uuid] = true;
    updateManager.listCompNames.clear();

    pldm::dbus::PropertyMap properties;
    properties.emplace(
        "Activation",
        std::string("xyz.openbmc_project.Software.Activation.Activations."
                    "Active"));

    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test", objPath.c_str(),
                                      "org.test.Interface", "Method");
    msg.append(std::string(Server::Activation::interface), properties);
    sealAndRewind(msg);

    otherDeviceUpdateManager.onActivationChangedMsg(msg);
    EXPECT_TRUE(updateManager.otherDeviceCompleted.contains(uuid));
    EXPECT_TRUE(updateManager.listCompNames.empty());
}

TEST_F(OtherDeviceUpdateManagerTest,
       onActivationChangedMsgFailedMarksOtherDeviceFailure)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    const std::string objPath = "/xyz/openbmc_project/software/other/entry7";
    const std::string uuid = "UUID_ENTRY7";

    auto tracked = std::make_unique<OtherDeviceUpdateActivation>();
    tracked->uuid = uuid;
    tracked->activationState = Server::Activation::Activations::Ready;
    tracked->requestedActivation =
        Server::Activation::RequestedActivations::None;
    otherDeviceUpdateManager.otherDevices[objPath] = std::move(tracked);

    updateManager.deviceUpdaterMap.emplace(0x2, nullptr);
    updateManager.totalNumComponentUpdates = 2;
    updateManager.compUpdateCompletedCount = 0;
    updateManager.otherDeviceComponents[uuid] = false;

    pldm::dbus::PropertyMap properties;
    properties.emplace(
        "Activation",
        std::string("xyz.openbmc_project.Software.Activation.Activations."
                    "Failed"));

    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test", objPath.c_str(),
                                      "org.test.Interface", "Method");
    msg.append(std::string(Server::Activation::interface), properties);
    sealAndRewind(msg);

    otherDeviceUpdateManager.onActivationChangedMsg(msg);
    EXPECT_TRUE(updateManager.otherDeviceCompleted.contains(uuid));
    EXPECT_FALSE(updateManager.otherDeviceCompleted[uuid]);
}

TEST_F(OtherDeviceUpdateManagerTest,
       interfaceAddedIgnoresUuidInterfaceWithoutUuidProperty)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    otherDeviceUpdateManager.startWatchingInterfaceAddition();
    ASSERT_NE(otherDeviceUpdateManager.interfaceAddedMatch, nullptr);

    const std::string objPath =
        "/xyz/openbmc_project/software/other/no_uuid_property";
    pldm::dbus::PropertyMap uuidProperties;
    uuidProperties.emplace("NotUUID", std::string("ignored"));
    pldm::dbus::InterfaceMap interfaces;
    interfaces.emplace("xyz.openbmc_project.Common.UUID", uuidProperties);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test",
                                      "/xyz/openbmc_project/software/other",
                                      "org.test.Interface", "Method");
    msg.append(sdbusplus::message::object_path(objPath), interfaces);
    sealAndRewind(msg);

    otherDeviceUpdateManager.interfaceAdded(msg);
    EXPECT_FALSE(otherDeviceUpdateManager.otherDevices.contains(objPath));
}

TEST_F(OtherDeviceUpdateManagerTest,
       extractOtherDevicePkgsSingleDeadComponentIsSkippedNotFailed)
{
    MockdBusHandler dbusHandler;
    const std::string objPath = "/xyz/openbmc_project/software/other/skip_dead";
    const std::string uuid = "11223344556677889900AABBCCDDEEFF";
    const std::string sku = "0X01020304";

    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(std::vector<std::string>{objPath}));
    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(testing::_, testing::_, testing::_))
        .WillRepeatedly([&](const char* objectPath, const char* property,
                            const char*) -> pldm::utils::PropertyValue {
            if (std::string(objectPath) == objPath &&
                std::string(property) == "UUID")
            {
                return uuid;
            }
            if (std::string(objectPath) == objPath &&
                std::string(property) == "SKU")
            {
                return sku;
            }
            if (std::string(objectPath) == objPath &&
                std::string(property) == "Path")
            {
                return std::string("/tmp/other/skip_dead/token.bin");
            }
            throw std::runtime_error("unexpected property request");
        });

    OtherDeviceUpdateManager otherDeviceUpdateManager(
        busMock, &updateManager, updatePolicyTargets, dbusHandler);
    std::filesystem::create_directories("/tmp/other/skip_dead");

    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {0},
         "VersionDead",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                                0x99, 0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE,
                                0xFF}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("APSKU",
                           std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04})}},
         {0}}};
    ComponentImageInfos compImageInfos{
        {10, deadComponent, 0xFFFFFFFF, 0, 0, 0, 4, "DeadVersion"}};
    std::istringstream package("ABCD1234");

    auto result = otherDeviceUpdateManager.extractOtherDevicePkgs(
        fwDeviceIDRecords, compImageInfos, package);

    EXPECT_EQ(result, 0);
    EXPECT_TRUE(otherDeviceUpdateManager.isImageFileProcessed.empty());
    std::filesystem::remove_all("/tmp/other/skip_dead");
}

TEST_F(OtherDeviceUpdateManagerTest,
       onActivationChangedThrowsForNonStringActivationValue)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    const std::string objPath =
        "/xyz/openbmc_project/software/other/non_string_activation";
    auto tracked = std::make_unique<OtherDeviceUpdateActivation>();
    tracked->activationState = Server::Activation::Activations::Ready;
    tracked->requestedActivation =
        Server::Activation::RequestedActivations::None;
    otherDeviceUpdateManager.otherDevices[objPath] = std::move(tracked);

    pldm::dbus::PropertyMap properties;
    properties.emplace("Activation", true);

    EXPECT_THROW(
        { otherDeviceUpdateManager.onActivationChanged(objPath, properties); },
        std::bad_variant_access);
}

TEST_F(OtherDeviceUpdateManagerTest,
       onActivationChangedThrowsForNonStringRequestedActivationValue)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    const std::string objPath =
        "/xyz/openbmc_project/software/other/non_string_requested";
    auto tracked = std::make_unique<OtherDeviceUpdateActivation>();
    tracked->activationState = Server::Activation::Activations::Ready;
    tracked->requestedActivation =
        Server::Activation::RequestedActivations::None;
    otherDeviceUpdateManager.otherDevices[objPath] = std::move(tracked);

    pldm::dbus::PropertyMap properties;
    properties.emplace(
        "Activation",
        std::string("xyz.openbmc_project.Software.Activation.Activations."
                    "Active"));
    properties.emplace("RequestedActivation", true);

    EXPECT_THROW(
        { otherDeviceUpdateManager.onActivationChanged(objPath, properties); },
        std::bad_variant_access);
}

TEST_F(OtherDeviceUpdateManagerTest,
       fetchDescriptorsFromPackageIgnoresNonApskuVendorDescriptor)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);

    FirmwareDeviceIDRecord fwDeviceIDRecord = {
        1,
        {0x00},
        "VersionString2",
        {{PLDM_FWUP_UUID,
          std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                               0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6, 0x75}},
         {PLDM_FWUP_VENDOR_DEFINED,
          std::make_tuple("BOARDID",
                          std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04})}},
        {}};

    auto result =
        otherDeviceUpdateManager.fetchDescriptorsFromPackage(fwDeviceIDRecord);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->first, "162023C93EC5411595F448701D49D675");
    EXPECT_TRUE(result->second.empty());
}

TEST_F(OtherDeviceUpdateManagerTest, startTimerReturnsWhenWatchNotActive)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    const std::string uuid = "UUID_TIMER_IDLE";
    otherDeviceUpdateManager.isImageFileProcessed[uuid] = false;
    otherDeviceUpdateManager.uuidMappings[uuid] = {"1.0", "CompTimerIdle"};

    otherDeviceUpdateManager.startTimer(0);
    EXPECT_GE(sd_event_run(event.get(), 1000), 0);

    EXPECT_EQ(otherDeviceUpdateManager.interfaceAddedMatch, nullptr);
    EXPECT_FALSE(updateManager.otherDeviceCompleted.contains(uuid));
}

TEST_F(OtherDeviceUpdateManagerTest,
       extractOtherDevicePkgsReturnsZeroWhenMultiComponentTransferFails)
{
    MockdBusHandler dbusHandler;
    const std::string objPath = "/xyz/openbmc_project/software/other/multi";
    const std::string uuid = "76910DFA1E4C11ED861D0242AC120002";
    const std::string sku = "0X01020304";

    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(std::vector<std::string>{objPath}));
    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(testing::_, testing::_, testing::_))
        .WillRepeatedly([&](const char* objectPath, const char* property,
                            const char*) -> pldm::utils::PropertyValue {
            if (std::string(objectPath) == objPath &&
                std::string(property) == "UUID")
            {
                return uuid;
            }
            if (std::string(objectPath) == objPath &&
                std::string(property) == "SKU")
            {
                return sku;
            }
            if (std::string(objectPath) == objPath &&
                std::string(property) == "Path")
            {
                return std::string("/tmp/other/multi/token.bin");
            }
            throw std::runtime_error("unexpected property request");
        });

    OtherDeviceUpdateManager otherDeviceUpdateManager(
        busMock, &updateManager, updatePolicyTargets, dbusHandler);
    std::filesystem::create_directories("/tmp/other/multi");

    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {0, 1},
         "VersionMulti",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x76, 0x91, 0x0D, 0xFA, 0x1E, 0x4C, 0x11, 0xED,
                                0x86, 0x1D, 0x02, 0x42, 0xAC, 0x12, 0x00,
                                0x02}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("APSKU",
                           std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04})}},
         {0, 1}}};
    ComponentImageInfos compImageInfos{
        {10, 201, 0xFFFFFFFF, 0, 0, 64, 32, "TruncatedVersion"},
        {10, 200, 0xFFFFFFFF, 0, 0, 0, 4, "LiveVersion"}};
    std::istringstream package("ABCD1234");

    auto result = otherDeviceUpdateManager.extractOtherDevicePkgs(
        fwDeviceIDRecords, compImageInfos, package);

    EXPECT_EQ(result, 0);
    EXPECT_TRUE(otherDeviceUpdateManager.uuidMappings.contains(uuid));
    EXPECT_FALSE(otherDeviceUpdateManager.isImageFileProcessed.contains(uuid));
    std::filesystem::remove_all("/tmp/other/multi");
}

TEST_F(OtherDeviceUpdateManagerTest,
       extractOtherDevicePkgsReturnsOneWhenMultiComponentTransferSucceeds)
{
    MockdBusHandler dbusHandler;
    const std::string objPath =
        "/xyz/openbmc_project/software/other/multi_success";
    const std::string uuid = "76910DFA1E4C11ED861D0242AC120002";
    const std::string sku = "0X01020304";

    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(std::vector<std::string>{objPath}));
    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(testing::_, testing::_, testing::_))
        .WillRepeatedly([&](const char* objectPath, const char* property,
                            const char*) -> pldm::utils::PropertyValue {
            if (std::string(objectPath) == objPath &&
                std::string(property) == "UUID")
            {
                return uuid;
            }
            if (std::string(objectPath) == objPath &&
                std::string(property) == "SKU")
            {
                return sku;
            }
            if (std::string(objectPath) == objPath &&
                std::string(property) == "Path")
            {
                return std::string("/tmp/other/multi_success/token.bin");
            }
            throw std::runtime_error("unexpected property request");
        });

    OtherDeviceUpdateManager otherDeviceUpdateManager(
        busMock, &updateManager, updatePolicyTargets, dbusHandler);
    std::filesystem::create_directories("/tmp/other/multi_success");

    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {0, 1},
         "VersionMultiSuccess",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x76, 0x91, 0x0D, 0xFA, 0x1E, 0x4C, 0x11, 0xED,
                                0x86, 0x1D, 0x02, 0x42, 0xAC, 0x12, 0x00,
                                0x02}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("APSKU",
                           std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04})}},
         {0, 1}}};
    ComponentImageInfos compImageInfos{
        {10, 300, 0xFFFFFFFF, 0, 0, 0, 4, "VersionA"},
        {10, 301, 0xFFFFFFFF, 0, 0, 4, 4, "VersionB"}};
    std::istringstream package("ABCDEFGH");

    auto result = otherDeviceUpdateManager.extractOtherDevicePkgs(
        fwDeviceIDRecords, compImageInfos, package);

    EXPECT_EQ(result, 1);
    EXPECT_TRUE(otherDeviceUpdateManager.isImageFileProcessed.contains(uuid));
    EXPECT_FALSE(otherDeviceUpdateManager.isImageFileProcessed[uuid]);
    std::filesystem::remove_all("/tmp/other/multi_success");
}

TEST_F(OtherDeviceUpdateManagerTest,
       onActivationChangedMsgActiveWithMatchingTargetIncludesComponentName)
{
    std::vector<sdbusplus::message::object_path> targets{
        sdbusplus::message::object_path(
            "/xyz/openbmc_project/software/other/entry_match_suffix")};
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      targets);
    const std::string objPath =
        "/xyz/openbmc_project/software/other/entry_match";
    const std::string uuid = "UUID_ENTRY_MATCH";

    auto tracked = std::make_unique<OtherDeviceUpdateActivation>();
    tracked->uuid = uuid;
    tracked->activationState = Server::Activation::Activations::Ready;
    tracked->requestedActivation =
        Server::Activation::RequestedActivations::None;
    otherDeviceUpdateManager.otherDevices[objPath] = std::move(tracked);
    otherDeviceUpdateManager.uuidMappings[uuid] = {"1.0", "CompEntryMatch"};

    updateManager.deviceUpdaterMap.emplace(0x1, nullptr);
    updateManager.totalNumComponentUpdates = 2;
    updateManager.compUpdateCompletedCount = 0;
    updateManager.otherDeviceComponents[uuid] = true;
    updateManager.listCompNames.clear();

    pldm::dbus::PropertyMap properties;
    properties.emplace(
        "Activation",
        std::string("xyz.openbmc_project.Software.Activation.Activations."
                    "Active"));
    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test", objPath.c_str(),
                                      "org.test.Interface", "Method");
    msg.append(std::string(Server::Activation::interface), properties);
    sealAndRewind(msg);

    otherDeviceUpdateManager.onActivationChangedMsg(msg);
    EXPECT_TRUE(updateManager.otherDeviceCompleted.contains(uuid));
    EXPECT_TRUE(updateManager.otherDeviceCompleted[uuid]);
    EXPECT_NE(updateManager.listCompNames.find("CompEntryMatch"),
              std::string::npos);
}

TEST_F(OtherDeviceUpdateManagerTest,
       onActivationChangedMsgActivatingDoesNotMarkCompletion)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    const std::string objPath =
        "/xyz/openbmc_project/software/other/entry_activating";
    const std::string uuid = "UUID_ENTRY_ACTIVATING";

    auto tracked = std::make_unique<OtherDeviceUpdateActivation>();
    tracked->uuid = uuid;
    tracked->activationState = Server::Activation::Activations::Ready;
    tracked->requestedActivation =
        Server::Activation::RequestedActivations::None;
    otherDeviceUpdateManager.otherDevices[objPath] = std::move(tracked);

    pldm::dbus::PropertyMap properties;
    properties.emplace(
        "Activation",
        std::string("xyz.openbmc_project.Software.Activation.Activations."
                    "Activating"));
    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test", objPath.c_str(),
                                      "org.test.Interface", "Method");
    msg.append(std::string(Server::Activation::interface), properties);
    sealAndRewind(msg);

    otherDeviceUpdateManager.onActivationChangedMsg(msg);
    EXPECT_FALSE(updateManager.otherDeviceCompleted.contains(uuid));
}

TEST_F(OtherDeviceUpdateManagerTest, interfaceAddedThrowsForNonStringUuidValue)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    otherDeviceUpdateManager.startWatchingInterfaceAddition();
    ASSERT_NE(otherDeviceUpdateManager.interfaceAddedMatch, nullptr);

    pldm::dbus::PropertyMap uuidProperties;
    uuidProperties.emplace("UUID", true);
    pldm::dbus::InterfaceMap interfaces;
    interfaces.emplace("xyz.openbmc_project.Common.UUID", uuidProperties);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test",
                                      "/xyz/openbmc_project/software/other",
                                      "org.test.Interface", "Method");
    msg.append(sdbusplus::message::object_path(
                   "/xyz/openbmc_project/software/other/invalid_uuid_type"),
               interfaces);
    sealAndRewind(msg);

    EXPECT_THROW(
        { otherDeviceUpdateManager.interfaceAdded(msg); },
        std::bad_variant_access);
}

TEST_F(OtherDeviceUpdateManagerTest,
       interfaceAddedKeepsWatchWhenPendingImageExists)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    otherDeviceUpdateManager.startWatchingInterfaceAddition();
    ASSERT_NE(otherDeviceUpdateManager.interfaceAddedMatch, nullptr);
    otherDeviceUpdateManager.isImageFileProcessed["UUID_PENDING"] = false;

    pldm::dbus::PropertyMap versionProperties;
    versionProperties.emplace("Version", std::string("1.0"));
    pldm::dbus::InterfaceMap interfaces;
    interfaces.emplace("xyz.openbmc_project.Software.Version",
                       versionProperties);

    auto rawBus = sdbusplus::bus::new_default();
    auto msg = rawBus.new_method_call("org.test",
                                      "/xyz/openbmc_project/software/other",
                                      "org.test.Interface", "Method");
    msg.append(sdbusplus::message::object_path(
                   "/xyz/openbmc_project/software/other/no_uuid_intf"),
               interfaces);
    sealAndRewind(msg);

    otherDeviceUpdateManager.interfaceAdded(msg);
    EXPECT_NE(otherDeviceUpdateManager.interfaceAddedMatch, nullptr);
}

TEST_F(OtherDeviceUpdateManagerTest,
       fetchDescriptorsFromPackageHandlesEmptyUuidBytes)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);

    FirmwareDeviceIDRecord fwDeviceIDRecord = {
        1,
        {0x00},
        "VersionString2",
        {{PLDM_FWUP_UUID, std::vector<uint8_t>{}}},
        {}};

    auto result =
        otherDeviceUpdateManager.fetchDescriptorsFromPackage(fwDeviceIDRecord);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->first.empty());
}

TEST_F(OtherDeviceUpdateManagerTest,
       fetchDescriptorsFromPackageThrowsOnInvalidVendorDescriptorVariant)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);

    FirmwareDeviceIDRecord fwDeviceIDRecord = {
        1,
        {0x00},
        "VersionString2",
        {{PLDM_FWUP_VENDOR_DEFINED, std::vector<uint8_t>{0x01, 0x02, 0x03}}},
        {}};

    EXPECT_THROW(
        {
            otherDeviceUpdateManager.fetchDescriptorsFromPackage(
                fwDeviceIDRecord);
        },
        std::bad_variant_access);
}

TEST_F(OtherDeviceUpdateManagerTest,
       extractOtherDevicePkgsReturnsZeroWhenSingleTransferFails)
{
    MockdBusHandler dbusHandler;
    const std::string objPath =
        "/xyz/openbmc_project/software/other/single_fail";
    const std::string uuid = "76910DFA1E4C11ED861D0242AC120002";
    const std::string sku = "0X01020304";

    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(std::vector<std::string>{objPath}));
    EXPECT_CALL(dbusHandler,
                getDbusPropertyVariant(testing::_, testing::_, testing::_))
        .WillRepeatedly([&](const char* objectPath, const char* property,
                            const char*) -> pldm::utils::PropertyValue {
            if (std::string(objectPath) == objPath &&
                std::string(property) == "UUID")
            {
                return uuid;
            }
            if (std::string(objectPath) == objPath &&
                std::string(property) == "SKU")
            {
                return sku;
            }
            if (std::string(objectPath) == objPath &&
                std::string(property) == "Path")
            {
                return std::string("/tmp/other/single_fail/token.bin");
            }
            throw std::runtime_error("unexpected property request");
        });

    OtherDeviceUpdateManager otherDeviceUpdateManager(
        busMock, &updateManager, updatePolicyTargets, dbusHandler);
    std::filesystem::create_directories("/tmp/other/single_fail");

    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {0},
         "VersionSingleFail",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x76, 0x91, 0x0D, 0xFA, 0x1E, 0x4C, 0x11, 0xED,
                                0x86, 0x1D, 0x02, 0x42, 0xAC, 0x12, 0x00,
                                0x02}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("APSKU",
                           std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04})}},
         {0}}};
    ComponentImageInfos compImageInfos{
        {10, 100, 0xFFFFFFFF, 0, 0, 32, 64, "VersionString2"}};
    std::istringstream package("ABCD");

    auto result = otherDeviceUpdateManager.extractOtherDevicePkgs(
        fwDeviceIDRecords, compImageInfos, package);

    EXPECT_EQ(result, 0);
    std::filesystem::remove_all("/tmp/other/single_fail");
}

TEST_F(OtherDeviceUpdateManagerTest,
       startTimerWithProcessedImageSkipsFailureCompletion)
{
    OtherDeviceUpdateManager otherDeviceUpdateManager(busMock, &updateManager,
                                                      updatePolicyTargets);
    otherDeviceUpdateManager.startWatchingInterfaceAddition();
    ASSERT_NE(otherDeviceUpdateManager.interfaceAddedMatch, nullptr);

    const std::string uuid = "UUID_TIMER_PROCESSED";
    otherDeviceUpdateManager.isImageFileProcessed[uuid] = true;
    otherDeviceUpdateManager.uuidMappings[uuid] = {"4.0", "CompTimerProcessed"};

    otherDeviceUpdateManager.startTimer(0);
    EXPECT_GE(sd_event_run(event.get(), 1000), 0);

    EXPECT_EQ(otherDeviceUpdateManager.interfaceAddedMatch, nullptr);
    EXPECT_FALSE(updateManager.otherDeviceCompleted.contains(uuid));
}
