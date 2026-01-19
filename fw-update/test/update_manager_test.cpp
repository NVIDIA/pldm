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
#include "common/mmap_stream.hpp"
#include "common/utils.hpp"
#include "fw-update/package_parser.hpp"
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "fw-update/update_manager.hpp"
#undef private
#include "fw-update/activation.hpp"
#include "requester/test/mock_request.hpp"
#include "test/test_instance_id.hpp"

#include <fcntl.h>
#include <systemd/sd-event.h>
#include <unistd.h>

#include <sdbusplus/bus.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include <gtest/gtest.h>

using namespace pldm;
using namespace pldm::fw_update;
using namespace std::chrono;

static bool mapPackageToUpdater(UpdateManager& updateManager,
                                const std::filesystem::path& packagePath)
{
    if (!updateManager.updater)
    {
        return false;
    }

    updateManager.updater->clearImageStream();
    int imageFd = open(packagePath.c_str(), O_RDONLY);
    if (imageFd < 0)
    {
        return false;
    }

    if (!updateManager.updater->mmapFile.map(imageFd, true))
    {
        return false;
    }

    updateManager.updater->mmapStream = std::make_unique<pldm::MmapStream>(
        updateManager.updater->mmapFile.data(),
        updateManager.updater->mmapFile.size());
    return updateManager.updater->mmapStream->good();
}

static int processPackageStream(UpdateManager& updateManager,
                                const std::filesystem::path& packagePath)
{
    if (!mapPackageToUpdater(updateManager, packagePath))
    {
        return -1;
    }

    try
    {
        if (!updateManager.otherDeviceUpdateManager)
        {
            updateManager.otherDeviceUpdateManager =
                std::make_unique<OtherDeviceUpdateManager>(
                    pldm::utils::DBusHandler::getBus(), &updateManager,
                    std::vector<sdbusplus::message::object_path>{});
        }

        auto task =
            updateManager.processStream(*updateManager.updater->mmapStream,
                                        updateManager.updater->mmapFile.size());
        auto rc = stdexec::sync_wait(std::move(task));
        if (!rc.has_value() || !updateManager.parser)
        {
            return -1;
        }
        return 0;
    }
    catch (const std::exception&)
    {
        return -1;
    }
}

class UpdateManagerTest : public testing::Test
{
  protected:
    UpdateManagerTest() :
        busMock(sdbusplus::get_mocked_new(&sdbusMock)),
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100))
    {}

    void waitEventExpiry(milliseconds timeout)
    {
        while (1)
        {
            auto sleepTime = duration_cast<microseconds>(timeout);
            if (!sd_event_run(event.get(), sleepTime.count()))
            {
                break;
            }
        }
    }

    void mapPackageToUpdater(UpdateManager& updateManager,
                             const std::string& packagePath)
    {
        ASSERT_TRUE(::mapPackageToUpdater(updateManager, packagePath));
    }

    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    TestInstanceIdDb instanceIdDb;
    sdbusplus::bus::bus busMock;
    sdeventplus::Event event;
    requester::Handler<requester::Request> reqHandler;
    DescriptorMap descriptorMap;
    ComponentInfoMap componentInfoMap;
    ComponentNameMap componentNameMap;
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

TEST_F(UpdateManagerTest, getActivationMethod_Automatic)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    const std::string activationMethodResult = "Automatic";

    bitfield16_t compActivationModification{0x1};

    std::string result =
        updateManager.getActivationMethod(compActivationModification);

    EXPECT_EQ(result, activationMethodResult);
}

TEST_F(UpdateManagerTest, getActivationMethod_SelfContained)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    const std::string activationMethodResult = "Self-Contained";

    bitfield16_t compActivationModification{0x2};

    std::string result =
        updateManager.getActivationMethod(compActivationModification);

    EXPECT_EQ(result, activationMethodResult);
}

TEST_F(UpdateManagerTest, getActivationMethod_AutomaticOrSelfContained)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    const std::string activationMethodResult = "Automatic or Self-Contained";

    bitfield16_t compActivationModification{0x3};

    std::string result =
        updateManager.getActivationMethod(compActivationModification);

    EXPECT_EQ(result, activationMethodResult);
}

TEST_F(UpdateManagerTest, getActivationMethod_MediumSpecificReset)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    const std::string activationMethodResult = "Medium-specific reset";

    bitfield16_t compActivationModification{0x4};

    std::string result =
        updateManager.getActivationMethod(compActivationModification);

    EXPECT_EQ(result, activationMethodResult);
}

TEST_F(UpdateManagerTest, getActivationMethod_SystemReboot)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    const std::string activationMethodResult = "System reboot";

    bitfield16_t compActivationModification{0x8};

    std::string result =
        updateManager.getActivationMethod(compActivationModification);

    EXPECT_EQ(result, activationMethodResult);
}

TEST_F(UpdateManagerTest, getActivationMethod_AcPowerCycle)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    const std::string activationMethodResult = "AC power cycle";

    bitfield16_t compActivationModification{0x20};

    std::string result =
        updateManager.getActivationMethod(compActivationModification);

    EXPECT_EQ(result, activationMethodResult);
}

TEST_F(UpdateManagerTest, getActivationMethod_DcOrAcPowerCycle)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    const std::string activationMethodResult =
        "DC power cycle or AC power cycle";

    bitfield16_t compActivationModification{0x30};

    std::string result =
        updateManager.getActivationMethod(compActivationModification);

    EXPECT_EQ(result, activationMethodResult);
}

TEST_F(UpdateManagerTest, clearFirmwareUpdatePackage)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    EXPECT_NO_THROW({ updateManager.clearFirmwareUpdatePackage(); });
}

TEST_F(UpdateManagerTest, updateDeviceCompletion)
{
    mctp_eid_t eid = 0;
    bool status = true;

    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    std::vector<ComponentName> successCompNames = {
        "TestComponentName1", "TestComponentName2", "TestComponentName3"};

    EXPECT_NO_THROW({
        updateManager.updateDeviceCompletion(eid, status, successCompNames);
    });
}

TEST_F(UpdateManagerTest, updateDeviceCompletion_withStatusEqualsFalse)
{
    mctp_eid_t eid = 0;
    bool status = false;

    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    EXPECT_NO_THROW({ updateManager.updateDeviceCompletion(eid, status); });
}

TEST_F(UpdateManagerTest, updateDeviceCompletion_withoutSuccessCompNames)
{
    mctp_eid_t eid = 0;
    bool status = true;

    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    EXPECT_NO_THROW({ updateManager.updateDeviceCompletion(eid, status); });
}

TEST_F(UpdateManagerTest, updateActivationProgress)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    EXPECT_NO_THROW({ updateManager.updateActivationProgress(); });
}

TEST_F(UpdateManagerTest, updateActivationProgressIncompleteKeepsTimerRunning)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath =
        "/xyz/openbmc_project/software/update_progress_incomplete";
    updateManager.activationProgress =
        std::make_unique<ActivationProgress>(busMock, updateManager.objPath);
    updateManager.totalNumComponentUpdates = 3;
    updateManager.compUpdateCompletedCount = 1;
    updateManager.createProgressUpdateTimer();
    ASSERT_NE(updateManager.progressTimer, nullptr);

    updateManager.updateActivationProgress();

    EXPECT_EQ(updateManager.compUpdateCompletedCount, 2U);
    EXPECT_NE(updateManager.progressTimer, nullptr);
}

TEST_F(UpdateManagerTest, clearActivationInfo)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    EXPECT_NO_THROW({ updateManager.clearActivationInfo(); });
}

TEST_F(UpdateManagerTest, activatePackage_throw_exception)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    EXPECT_THROW(updateManager.activatePackage(),
                 sdbusplus::exception::SdBusError);
}

TEST_F(UpdateManagerTest, processPackage_empty_descriptorMap)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    processPackageStream(updateManager, "./test_pkg");
}

TEST_F(UpdateManagerTest, processPackage_no_matching_devices_found)
{
    mctp_eid_t eid = 0;

    const DescriptorMap descriptorMap2{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("ECSKU",
                           std::vector<uint8_t>{0x49, 0x35, 0x36, 0x81})}}}};

    ComponentInfoMap componentInfoMap;
    ComponentNameMap componentNameMap;
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap2,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    processPackageStream(updateManager, "./test_pkg");
}

TEST_F(UpdateManagerTest, processPackage_new)
{
    requester::Handler<requester::Request> reqHandler2(
        nullptr, event, instanceIdDb, false, seconds(1), 2, milliseconds(100));

    mctp_eid_t eid = 0x01;

    const DescriptorMap descriptorMap2{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6,
                                0x75}}}}};

    UpdateManager updateManager(event, reqHandler2, instanceIdDb,
                                descriptorMap2, componentInfoMap,
                                componentNameMap, true, nullptr);

    processPackageStream(updateManager, "./test_pkg");
}

TEST_F(UpdateManagerTest, handleRequest_empty_descriptorMap)
{
    uint8_t expectedResult = 0x15;

    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    mctp_eid_t eid = 0;

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_request_firmware_data_req)>
        reqFwDataReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x02, 0x00, 0x00};

    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    auto result =
        updateManager.handleRequest(eid, PLDM_REQUEST_FIRMWARE_DATA, requestMsg,
                                    sizeof(pldm_request_firmware_data_req));

    EXPECT_EQ(result[2], expectedResult);
}

TEST_F(UpdateManagerTest, handleRequest_request_fw_data)
{
    uint8_t expectedResult = 0x15;
    mctp_eid_t eid = 0;
    ComponentInfoMap componentInfoMap2{
        {eid,
         {{std::make_pair(10, 100),
           std::make_tuple(static_cast<uint8_t>(1), "comp1Version",
                           static_cast<uint16_t>(0))}}}};

    const DescriptorMap descriptorMap2{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6,
                                0x75}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x01, 0x02})}}}};

    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap2,
                                componentInfoMap2, componentNameMap, true,
                                nullptr);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_request_firmware_data_req)>
        reqFwDataReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x02, 0x00, 0x00};

    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());
    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    ASSERT_FALSE(updateManager.deviceUpdaterMap.empty());

    auto result =
        updateManager.handleRequest(eid, PLDM_REQUEST_FIRMWARE_DATA, requestMsg,
                                    sizeof(pldm_request_firmware_data_req));

    EXPECT_EQ(result[2], expectedResult);
}

TEST_F(UpdateManagerTest, handleRequest_transfer_complete)
{
    uint8_t expectedResult = 0x16;
    mctp_eid_t eid = 0;
    ComponentInfoMap componentInfoMap2{
        {eid,
         {{std::make_pair(10, 100),
           std::make_tuple(static_cast<uint8_t>(1), "comp1Version",
                           static_cast<uint16_t>(0))}}}};

    const DescriptorMap descriptorMap2{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6,
                                0x75}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x01, 0x02})}}}};

    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap2,
                                componentInfoMap2, componentNameMap, true,
                                nullptr);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_request_firmware_data_req)>
        reqFwDataReq{0x8A, 0x05, 0x16, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x02, 0x00, 0x00};

    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    ASSERT_FALSE(updateManager.deviceUpdaterMap.empty());

    auto result =
        updateManager.handleRequest(eid, PLDM_TRANSFER_COMPLETE, requestMsg,
                                    sizeof(pldm_request_firmware_data_req));

    EXPECT_EQ(result[2], expectedResult);
}

TEST_F(UpdateManagerTest, handleRequest_verify_complete)
{
    uint8_t expectedResult = 0x17;
    mctp_eid_t eid = 0;
    ComponentInfoMap componentInfoMap2{
        {eid,
         {{std::make_pair(10, 100),
           std::make_tuple(static_cast<uint8_t>(1), "comp1Version",
                           static_cast<uint16_t>(0))}}}};

    const DescriptorMap descriptorMap2{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6,
                                0x75}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x01, 0x02})}}}};

    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap2,
                                componentInfoMap2, componentNameMap, true,
                                nullptr);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_request_firmware_data_req)>
        reqFwDataReq{0x8A, 0x05, 0x17, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x02, 0x00, 0x00};

    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    ASSERT_FALSE(updateManager.deviceUpdaterMap.empty());

    auto result =
        updateManager.handleRequest(eid, PLDM_VERIFY_COMPLETE, requestMsg,
                                    sizeof(pldm_request_firmware_data_req));

    EXPECT_EQ(result[2], expectedResult);
}

TEST_F(UpdateManagerTest, handleRequest_apply_complete)
{
    uint8_t expectedResult = 0x18;
    mctp_eid_t eid = 0;
    ComponentInfoMap componentInfoMap2{
        {eid,
         {{std::make_pair(10, 100),
           std::make_tuple(static_cast<uint8_t>(1), "comp1Version",
                           static_cast<uint16_t>(0))}}}};

    const DescriptorMap descriptorMap2{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6,
                                0x75}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x01, 0x02})}}}};

    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap2,
                                componentInfoMap2, componentNameMap, true,
                                nullptr);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_request_firmware_data_req)>
        reqFwDataReq{0x8A, 0x05, 0x18, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x02, 0x00, 0x00};

    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    ASSERT_FALSE(updateManager.deviceUpdaterMap.empty());

    auto result =
        updateManager.handleRequest(eid, PLDM_APPLY_COMPLETE, requestMsg,
                                    sizeof(pldm_request_firmware_data_req));

    EXPECT_EQ(result[2], expectedResult);
}

TEST_F(UpdateManagerTest, handleRequest_not_supported_command)
{
    uint8_t expectedResult = 0x15;
    mctp_eid_t eid = 0;
    ComponentInfoMap componentInfoMap2{
        {eid,
         {{std::make_pair(10, 100),
           std::make_tuple(static_cast<uint8_t>(1), "comp1Version",
                           static_cast<uint16_t>(0))}}}};

    const DescriptorMap descriptorMap2{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6,
                                0x75}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x01, 0x02})}}}};

    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap2,
                                componentInfoMap2, componentNameMap, true,
                                nullptr);

    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) +
                                      sizeof(pldm_request_firmware_data_req)>
        reqFwDataReq{0x8A, 0x05, 0x15, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0x02, 0x00, 0x00};

    auto requestMsg = reinterpret_cast<const pldm_msg*>(reqFwDataReq.data());

    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    ASSERT_FALSE(updateManager.deviceUpdaterMap.empty());

    auto result = updateManager.handleRequest(
        eid, PLDM_QUERY_DEVICE_IDENTIFIERS, requestMsg,
        sizeof(pldm_request_firmware_data_req));

    EXPECT_EQ(result[2], expectedResult);
    EXPECT_EQ(result[sizeof(pldm_msg_hdr)], PLDM_ERROR_INVALID_DATA);
}

TEST_F(UpdateManagerTest, setActivationStatus)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    const Server::Activation::Activations activationState =
        Server::Activation::Activations::Active;

    processPackageStream(updateManager, "./test_pkg");

    EXPECT_NO_THROW({ updateManager.setActivationStatus(activationState); });
}

TEST_F(UpdateManagerTest, updateOtherDeviceComponents)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    std::unordered_map<std::string, bool> otherDeviceMap = {
        {"device1", true}, {"device2", false}, {"device3", true}};

    processPackageStream(updateManager, "./test_pkg");
    EXPECT_NO_THROW({
        updateManager.updateOtherDeviceComponents(otherDeviceMap);
    });
}

TEST_F(UpdateManagerTest,
       updateOtherDeviceComponentsWithActivationObjectPrepared)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath =
        "/xyz/openbmc_project/software/other_device_components_with_activation";
    auto activation = std::make_unique<RecordingActivation>(
        busMock, updateManager.objPath, &updateManager);
    auto* activationRaw = activation.get();
    updateManager.activation = std::move(activation);

    std::unordered_map<std::string, bool> otherDeviceMap{{"UUID_A", true}};

    EXPECT_NO_THROW({
        updateManager.updateOtherDeviceComponents(otherDeviceMap);
    });
    EXPECT_EQ(updateManager.otherDeviceComponents.size(), 1U);
    EXPECT_EQ(activationRaw->lastValue,
              software::Activation::Activations::Activating);
}

TEST_F(UpdateManagerTest, resetActivationBlocksTransition)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    EXPECT_NO_THROW({ updateManager.resetActivationBlocksTransition(); });
}

TEST_F(UpdateManagerTest, getComponentName)
{
    eid eid1 = 0;
    const std::string componentName{"Component1"};

    const std::string activeCompVersion1{"Comp1v2.0"};
    const std::string activeCompVersion2{"Comp2v3.0"};
    constexpr uint16_t compClassification1 = 10;
    constexpr uint16_t compIdentifier1 = 100;
    constexpr uint8_t compClassificationIndex1 = 20;
    constexpr uint16_t compClassification2 = 16;
    constexpr uint16_t compIdentifier2 = 301;
    constexpr uint8_t compClassificationIndex2 = 30;
    ComponentInfoMap componentInfoMap2{
        {eid1,
         {{std::make_pair(compClassification1, compIdentifier1),
           std::make_tuple(compClassificationIndex1, activeCompVersion1,
                           static_cast<uint16_t>(0))},
          {std::make_pair(compClassification2, compIdentifier2),
           std::make_tuple(compClassificationIndex2, activeCompVersion2,
                           static_cast<uint16_t>(0))}}}};

    const DescriptorMap descriptorMap2{
        {eid1,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6,
                                0x75}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x01, 0x02})}}}};

    ComponentNameMap componentNameMap2{
        {eid1, {{compIdentifier1, componentName}}}};

    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap2,
                                componentInfoMap2, componentNameMap2, true,
                                nullptr);

    FirmwareDeviceIDRecord fwDeviceIDRecord = {
        1,
        {0x00},
        "VersionString2",
        {{PLDM_FWUP_UUID,
          std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                               0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                               0x75}}},
        {}};

    size_t componentIndex = 0;
    processPackageStream(updateManager, "./test_pkg");

    std::string componentNameResult =
        updateManager.getComponentName(eid1, fwDeviceIDRecord, componentIndex);
    EXPECT_EQ(componentNameResult, componentName);
}

TEST_F(UpdateManagerTest, getComponentName_DoesNotFindComponent)
{
    eid eid1 = 0;
    const std::string componentName{"Component1"};

    const std::string activeCompVersion1{"Comp1v2.0"};
    const std::string activeCompVersion2{"Comp2v3.0"};
    constexpr uint16_t compClassification1 = 10;
    constexpr uint16_t compIdentifier1 = 200;
    constexpr uint8_t compClassificationIndex1 = 20;
    constexpr uint16_t compClassification2 = 16;
    constexpr uint16_t compIdentifier2 = 301;
    constexpr uint8_t compClassificationIndex2 = 30;
    ComponentInfoMap componentInfoMap2{
        {eid1,
         {{std::make_pair(compClassification1, compIdentifier1),
           std::make_tuple(compClassificationIndex1, activeCompVersion1,
                           static_cast<uint16_t>(0))},
          {std::make_pair(compClassification2, compIdentifier2),
           std::make_tuple(compClassificationIndex2, activeCompVersion2,
                           static_cast<uint16_t>(0))}}}};

    const DescriptorMap descriptorMap2{
        {eid1,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6,
                                0x75}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x01, 0x02})}}}};

    ComponentNameMap componentNameMap2{
        {eid1, {{compIdentifier1, componentName}}}};

    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap2,
                                componentInfoMap2, componentNameMap2, true,
                                nullptr);

    FirmwareDeviceIDRecord fwDeviceIDRecord = {
        1,
        {0x00},
        "VersionString2",
        {{PLDM_FWUP_UUID,
          std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                               0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                               0x75}}},
        {}};

    size_t componentIndex = 0;
    processPackageStream(updateManager, "./test_pkg");

    std::string componentNameResult =
        updateManager.getComponentName(eid1, fwDeviceIDRecord, componentIndex);
    EXPECT_EQ(componentNameResult, "");
}

TEST_F(UpdateManagerTest, getComponentName_ForEmptyComponentNameMap)
{
    eid eid1 = 0;
    const std::string componentName{"Component1"};

    const std::string activeCompVersion1{"Comp1v2.0"};
    const std::string activeCompVersion2{"Comp2v3.0"};
    constexpr uint16_t compClassification1 = 10;
    constexpr uint16_t compIdentifier1 = 200;
    constexpr uint8_t compClassificationIndex1 = 20;
    constexpr uint16_t compClassification2 = 16;
    constexpr uint16_t compIdentifier2 = 301;
    constexpr uint8_t compClassificationIndex2 = 30;
    ComponentInfoMap componentInfoMap2{
        {eid1,
         {{std::make_pair(compClassification1, compIdentifier1),
           std::make_tuple(compClassificationIndex1, activeCompVersion1,
                           static_cast<uint16_t>(0))},
          {std::make_pair(compClassification2, compIdentifier2),
           std::make_tuple(compClassificationIndex2, activeCompVersion2,
                           static_cast<uint16_t>(0))}}}};

    const DescriptorMap descriptorMap2{
        {eid1,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6,
                                0x75}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x01, 0x02})}}}};

    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap2,
                                componentInfoMap2, componentNameMap, true,
                                nullptr);

    FirmwareDeviceIDRecord fwDeviceIDRecord = {
        1,
        {0x00},
        "VersionString2",
        {{PLDM_FWUP_UUID,
          std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                               0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                               0x75}}},
        {}};

    size_t componentIndex = 0;
    processPackageStream(updateManager, "./test_pkg");

    EXPECT_NO_THROW({
        updateManager.getComponentName(eid1, fwDeviceIDRecord, componentIndex);
    });
}

TEST_F(UpdateManagerTest, processPackage_Package_v3_truncated)
{
    requester::Handler<requester::Request> reqHandler2(
        nullptr, event, instanceIdDb, false, seconds(1), 2, milliseconds(100));
    mctp_eid_t eid = 0x01;

    const DescriptorMap descriptorMap2{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6,
                                0x75}}}}};

    UpdateManager updateManager(event, reqHandler2, instanceIdDb,
                                descriptorMap2, componentInfoMap,
                                componentNameMap, true, nullptr);

    int result =
        processPackageStream(updateManager, "./test_pkg_v3_signed_truncated");

    EXPECT_EQ(result, -1);
}

TEST_F(UpdateManagerTest, processPackage_missingFileReturnsFailure)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    EXPECT_EQ(processPackageStream(updateManager, "./does_not_exist.pkg"), -1);
}

TEST_F(UpdateManagerTest, processPackage_emptyFileReturnsFailure)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    auto emptyPath =
        std::filesystem::temp_directory_path() / "pldm_fw_empty_package.bin";
    {
        std::ofstream out(emptyPath, std::ios::binary);
    }
    EXPECT_EQ(processPackageStream(updateManager, emptyPath), -1);
    std::filesystem::remove(emptyPath);
}

TEST_F(UpdateManagerTest, processStream_invalidStreamState)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    std::stringstream badStream;
    badStream.setstate(std::ios::failbit);

    EXPECT_ANY_THROW({
        auto task = updateManager.processStream(badStream, 10, {});
        stdexec::sync_wait(std::move(task));
    });
}

TEST_F(UpdateManagerTest, processStream_sizeTooSmall)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    std::stringstream pkg("abc");

    EXPECT_ANY_THROW({
        auto task = updateManager.processStream(pkg, 1, {});
        stdexec::sync_wait(std::move(task));
    });
}

TEST_F(UpdateManagerTest, processStream_nonMmapInvalidHeaderPath)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    std::string bytes(sizeof(pldm_package_header_information) + 8, '\0');
    std::stringstream pkg(bytes);

    EXPECT_ANY_THROW({
        auto task = updateManager.processStream(pkg, bytes.size(), {});
        stdexec::sync_wait(std::move(task));
    });
}

TEST_F(UpdateManagerTest, processStreamDefer_noMatchingDevices)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    std::ifstream package("./test_pkg", std::ios::binary | std::ios::ate);
    ASSERT_TRUE(package.good());
    auto packageSize = static_cast<uintmax_t>(package.tellg());
    package.seekg(0, std::ios::beg);
    updateManager.setRequestedApplyTime(
        sdbusplus::xyz::openbmc_project::Software::server::ApplyTime::
            RequestedApplyTimes::Immediate);

    EXPECT_NO_THROW(
        updateManager.processStreamDefer(package, packageSize, false, {}));
}

TEST_F(UpdateManagerTest,
       processStreamDeferExecutesDeferredPathWithRefreshCallback)
{
    DescriptorMap localDescriptorMap;
    localDescriptorMap.emplace(1, Descriptors{});

    UpdateManager updateManager(
        event, reqHandler, instanceIdDb, localDescriptorMap, componentInfoMap,
        componentNameMap, true,
        [](mctp_eid_t, bool) -> exec::task<int> { co_return 0; });

    std::ifstream package("./test_pkg", std::ios::binary | std::ios::ate);
    ASSERT_TRUE(package.good());
    auto packageSize = static_cast<uintmax_t>(package.tellg());
    package.seekg(0, std::ios::beg);
    updateManager.setRequestedApplyTime(
        sdbusplus::xyz::openbmc_project::Software::server::ApplyTime::
            RequestedApplyTimes::Immediate);

    EXPECT_NO_THROW(
        updateManager.processStreamDefer(package, packageSize, false, {}));
    EXPECT_GE(sd_event_run(event.get(), 500000), 0);
    EXPECT_GE(sd_event_run(event.get(), 500000), 0);
}

TEST_F(UpdateManagerTest, createMessageRegistryPathsAreCallable)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    ASSERT_NE(updateManager.parser, nullptr);

    const auto& fwDeviceIDRecords =
        updateManager.parser->getFwDeviceIDRecords();
    ASSERT_FALSE(fwDeviceIDRecords.empty());
    const auto& record = fwDeviceIDRecords.front();
    ASSERT_FALSE(std::get<ApplicableComponents>(record).empty());

    EXPECT_NO_THROW({
        updateManager.createMessageRegistry(
            1, record, 0, "Update.1.0.TargetDetermined", "",
            static_cast<pldm_firmware_update_commands>(0), 0);
        updateManager.createMessageRegistryResourceErrors(
            1, record, 0, "Update.1.0.TransferFailed", "transfer error",
            "Retry firmware update operation");
    });
}

TEST_F(UpdateManagerTest, handleInvalidPackageErrorCreatesAndReusesActivation)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    EXPECT_THROW(updateManager.handleInvalidPackageError(),
                 sdbusplus::exception::SdBusError);
}

TEST_F(UpdateManagerTest, handlePayloadChecksumErrorCreatesAndReusesActivation)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    EXPECT_THROW(updateManager.handlePayloadChecksumError(),
                 sdbusplus::exception::SdBusError);
}

TEST_F(UpdateManagerTest,
       handleInvalidPackageHeaderErrorCreatesAndReusesActivation)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    EXPECT_THROW(updateManager.handleInvalidPackageHeaderError(),
                 sdbusplus::exception::SdBusError);
}

TEST_F(UpdateManagerTest, performSecurityChecksAsyncInvokesCallbackOrError)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    processPackageStream(updateManager, "./test_pkg_v3_signed");
    mapPackageToUpdater(updateManager, "./test_pkg_v3_signed");

    bool onCompleteCalled = false;
    bool onErrorCalled = false;
    updateManager.performSecurityChecksAsync(
        [&](bool) { onCompleteCalled = true; },
        [&](const std::string&) { onErrorCalled = true; });

    waitEventExpiry(milliseconds(1200));
    EXPECT_TRUE(onCompleteCalled || onErrorCalled);
}

TEST_F(UpdateManagerTest,
       performSecurityChecksAsyncFailurePathWithIncorrectSignature)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    processPackageStream(updateManager, "./test_pkg_v3_incorrectly_signed");
    mapPackageToUpdater(updateManager, "./test_pkg_v3_incorrectly_signed");

    bool onCompleteCalled = false;
    bool onErrorCalled = false;
    bool securityStatus = true;
    updateManager.performSecurityChecksAsync(
        [&](bool status) {
            onCompleteCalled = true;
            securityStatus = status;
        },
        [&](const std::string&) { onErrorCalled = true; });

    waitEventExpiry(milliseconds(1200));
    EXPECT_TRUE(onCompleteCalled || onErrorCalled);
    if (onCompleteCalled)
    {
        EXPECT_FALSE(securityStatus);
    }
}

TEST_F(UpdateManagerTest, packageIntegrityCheckAsyncInvokesCallback)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    processPackageStream(updateManager, "./test_pkg_v3_signed");
    mapPackageToUpdater(updateManager, "./test_pkg_v3_signed");

    bool onCompleteCalled = false;
    bool onErrorCalled = false;
    updateManager.packageIntegrityCheckAsync(
        [&](bool) { onCompleteCalled = true; },
        [&](const std::string&) { onErrorCalled = true; });

    waitEventExpiry(milliseconds(1200));
    EXPECT_TRUE(onCompleteCalled || onErrorCalled);
}

TEST_F(UpdateManagerTest,
       packageIntegrityCheckAsyncFailsWithCorruptedSignedPackage)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    processPackageStream(updateManager, "./test_pkg_v3_signed_corrupted_bytes");
    mapPackageToUpdater(updateManager, "./test_pkg_v3_signed_corrupted_bytes");

    bool onCompleteCalled = false;
    bool onErrorCalled = false;
    bool integrityStatus = true;
    updateManager.packageIntegrityCheckAsync(
        [&](bool status) {
            onCompleteCalled = true;
            integrityStatus = status;
        },
        [&](const std::string&) { onErrorCalled = true; });

    waitEventExpiry(milliseconds(1200));
    EXPECT_TRUE(onCompleteCalled || onErrorCalled);
    if (onCompleteCalled)
    {
        EXPECT_FALSE(integrityStatus);
    }
}

TEST_F(UpdateManagerTest, packageIntegrityCheckAsyncUnsignedPackagePath)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    processPackageStream(updateManager, "./test_pkg");
    mapPackageToUpdater(updateManager, "./test_pkg");

    bool onCompleteCalled = false;
    bool onErrorCalled = false;
    bool result = false;
    updateManager.packageIntegrityCheckAsync(
        [&](bool status) {
            onCompleteCalled = true;
            result = status;
        },
        [&](const std::string&) { onErrorCalled = true; });

    waitEventExpiry(milliseconds(1200));
    EXPECT_TRUE(onCompleteCalled || onErrorCalled);
    if (onCompleteCalled)
    {
        EXPECT_TRUE(result);
    }
}

TEST_F(UpdateManagerTest, verifyPackageAsyncInvokesCallback)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    processPackageStream(updateManager, "./test_pkg_v3_signed");
    mapPackageToUpdater(updateManager, "./test_pkg_v3_signed");

    bool onCompleteCalled = false;
    bool onErrorCalled = false;
    updateManager.verifyPackageAsync(
        [&](bool) { onCompleteCalled = true; },
        [&](const std::string&) { onErrorCalled = true; });

    waitEventExpiry(milliseconds(1200));
    EXPECT_TRUE(onCompleteCalled || onErrorCalled);
}

TEST_F(UpdateManagerTest, verifyPackageAsyncFailsWithIncorrectSignature)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    processPackageStream(updateManager, "./test_pkg_v3_incorrectly_signed");
    mapPackageToUpdater(updateManager, "./test_pkg_v3_incorrectly_signed");

    bool onCompleteCalled = false;
    bool onErrorCalled = false;
    bool verificationStatus = true;
    updateManager.verifyPackageAsync(
        [&](bool status) {
            onCompleteCalled = true;
            verificationStatus = status;
        },
        [&](const std::string&) { onErrorCalled = true; });

    waitEventExpiry(milliseconds(1200));
    EXPECT_TRUE(onCompleteCalled || onErrorCalled);
    if (onCompleteCalled)
    {
        EXPECT_FALSE(verificationStatus);
    }
}

TEST_F(UpdateManagerTest, verifyPackageAsyncUnsignedPackagePath)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    processPackageStream(updateManager, "./test_pkg");
    mapPackageToUpdater(updateManager, "./test_pkg");

    bool onCompleteCalled = false;
    bool onErrorCalled = false;
    updateManager.verifyPackageAsync(
        [&](bool) { onCompleteCalled = true; },
        [&](const std::string&) { onErrorCalled = true; });

    waitEventExpiry(milliseconds(100));
    EXPECT_TRUE(onCompleteCalled || onErrorCalled);
}

TEST_F(UpdateManagerTest, packageIntegrityCheckAsyncGetSignatureHeaderFailure)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    processPackageStream(updateManager, "./test_pkg_v3_signed");

    auto brokenPath =
        std::filesystem::temp_directory_path() / "pldm_pkg_v3_signed_extra.bin";
    std::filesystem::copy_file(
        "./test_pkg_v3_signed", brokenPath,
        std::filesystem::copy_options::overwrite_existing);
    {
        std::ofstream out(brokenPath, std::ios::binary | std::ios::app);
        out.put(static_cast<char>(0x00));
    }
    mapPackageToUpdater(updateManager, brokenPath.string());

    bool onCompleteCalled = false;
    bool onErrorCalled = false;
    bool result = true;
    updateManager.packageIntegrityCheckAsync(
        [&](bool status) {
            onCompleteCalled = true;
            result = status;
        },
        [&](const std::string&) { onErrorCalled = true; });

    std::filesystem::remove(brokenPath);
    EXPECT_TRUE(onCompleteCalled);
    EXPECT_FALSE(result);
    EXPECT_FALSE(onErrorCalled);
}

TEST_F(UpdateManagerTest, verifyPackageAsyncGetSignatureHeaderFailure)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    processPackageStream(updateManager, "./test_pkg_v3_signed");

    auto brokenPath = std::filesystem::temp_directory_path() /
                      "pldm_pkg_v3_signed_extra2.bin";
    std::filesystem::copy_file(
        "./test_pkg_v3_signed", brokenPath,
        std::filesystem::copy_options::overwrite_existing);
    {
        std::ofstream out(brokenPath, std::ios::binary | std::ios::app);
        out.put(static_cast<char>(0x00));
    }
    mapPackageToUpdater(updateManager, brokenPath.string());

    bool onCompleteCalled = false;
    bool onErrorCalled = false;
    bool result = true;
    updateManager.verifyPackageAsync(
        [&](bool status) {
            onCompleteCalled = true;
            result = status;
        },
        [&](const std::string&) { onErrorCalled = true; });

    std::filesystem::remove(brokenPath);
    EXPECT_TRUE(onCompleteCalled);
    EXPECT_FALSE(result);
    EXPECT_FALSE(onErrorCalled);
}

TEST_F(UpdateManagerTest,
       packageIntegrityCheckAsyncUnsupportedSignatureVersionFallsBackToPass)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    processPackageStream(updateManager, "./test_pkg_v3_signed");
    auto calcPkgSize = updateManager.parser->calculatePackageSize();

    auto brokenPath =
        std::filesystem::temp_directory_path() / "pldm_pkg_v3_bad_version.bin";
    std::filesystem::copy_file(
        "./test_pkg_v3_signed", brokenPath,
        std::filesystem::copy_options::overwrite_existing);
    {
        std::fstream io(brokenPath,
                        std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(io.good());
        io.seekp(static_cast<std::streamoff>(calcPkgSize) +
                 static_cast<std::streamoff>(pldmFwupSignatureMagicLength));
        io.put(static_cast<char>(0xFF));
    }
    mapPackageToUpdater(updateManager, brokenPath.string());

    bool onCompleteCalled = false;
    bool onErrorCalled = false;
    bool result = false;
    updateManager.packageIntegrityCheckAsync(
        [&](bool status) {
            onCompleteCalled = true;
            result = status;
        },
        [&](const std::string&) { onErrorCalled = true; });

    std::filesystem::remove(brokenPath);
    EXPECT_TRUE(onCompleteCalled);
    EXPECT_TRUE(result);
    EXPECT_FALSE(onErrorCalled);
}

TEST_F(UpdateManagerTest, verifyPackageAsyncUnsupportedSignatureVersionFails)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    processPackageStream(updateManager, "./test_pkg_v3_signed");
    auto calcPkgSize = updateManager.parser->calculatePackageSize();

    auto brokenPath =
        std::filesystem::temp_directory_path() / "pldm_pkg_v3_bad_version2.bin";
    std::filesystem::copy_file(
        "./test_pkg_v3_signed", brokenPath,
        std::filesystem::copy_options::overwrite_existing);
    {
        std::fstream io(brokenPath,
                        std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(io.good());
        io.seekp(static_cast<std::streamoff>(calcPkgSize) +
                 static_cast<std::streamoff>(pldmFwupSignatureMagicLength));
        io.put(static_cast<char>(0xFF));
    }
    mapPackageToUpdater(updateManager, brokenPath.string());

    bool onCompleteCalled = false;
    bool onErrorCalled = false;
    bool result = true;
    updateManager.verifyPackageAsync(
        [&](bool status) {
            onCompleteCalled = true;
            result = status;
        },
        [&](const std::string&) { onErrorCalled = true; });

    std::filesystem::remove(brokenPath);
    EXPECT_TRUE(onCompleteCalled);
    EXPECT_FALSE(result);
    EXPECT_FALSE(onErrorCalled);
}

TEST_F(UpdateManagerTest, packageIntegrityCheckAsyncParseHeaderFailure)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    processPackageStream(updateManager, "./test_pkg_v3_signed");
    auto calcPkgSize = updateManager.parser->calculatePackageSize();
    constexpr std::streamoff signatureTypeOffset = 13;

    auto brokenPath =
        std::filesystem::temp_directory_path() / "pldm_pkg_v3_bad_sig_type.bin";
    std::filesystem::copy_file(
        "./test_pkg_v3_signed", brokenPath,
        std::filesystem::copy_options::overwrite_existing);
    {
        std::fstream io(brokenPath,
                        std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(io.good());
        io.seekp(static_cast<std::streamoff>(calcPkgSize) +
                 signatureTypeOffset);
        io.put(static_cast<char>(0x01));
    }
    mapPackageToUpdater(updateManager, brokenPath.string());

    bool onCompleteCalled = false;
    bool onErrorCalled = false;
    bool result = true;
    updateManager.packageIntegrityCheckAsync(
        [&](bool status) {
            onCompleteCalled = true;
            result = status;
        },
        [&](const std::string&) { onErrorCalled = true; });

    std::filesystem::remove(brokenPath);
    EXPECT_TRUE(onCompleteCalled);
    EXPECT_FALSE(result);
    EXPECT_FALSE(onErrorCalled);
}

TEST_F(UpdateManagerTest, verifyPackageAsyncParseHeaderFailure)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    processPackageStream(updateManager, "./test_pkg_v3_signed");
    auto calcPkgSize = updateManager.parser->calculatePackageSize();
    constexpr std::streamoff signatureTypeOffset = 13;

    auto brokenPath = std::filesystem::temp_directory_path() /
                      "pldm_pkg_v3_bad_sig_type2.bin";
    std::filesystem::copy_file(
        "./test_pkg_v3_signed", brokenPath,
        std::filesystem::copy_options::overwrite_existing);
    {
        std::fstream io(brokenPath,
                        std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(io.good());
        io.seekp(static_cast<std::streamoff>(calcPkgSize) +
                 signatureTypeOffset);
        io.put(static_cast<char>(0x01));
    }
    mapPackageToUpdater(updateManager, brokenPath.string());

    bool onCompleteCalled = false;
    bool onErrorCalled = false;
    bool result = true;
    updateManager.verifyPackageAsync(
        [&](bool status) {
            onCompleteCalled = true;
            result = status;
        },
        [&](const std::string&) { onErrorCalled = true; });

    std::filesystem::remove(brokenPath);
    EXPECT_TRUE(onCompleteCalled);
    EXPECT_FALSE(result);
    EXPECT_FALSE(onErrorCalled);
}

TEST_F(UpdateManagerTest, startPLDMUpdateNoDevicesNoop)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    EXPECT_NO_THROW({ updateManager.startPLDMUpdate(); });
}

TEST_F(UpdateManagerTest, createProgressUpdateTimerCallbackAdvancesProgress)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath = "/xyz/openbmc_project/software/progress_test";
    updateManager.activationProgress =
        std::make_unique<ActivationProgress>(busMock, updateManager.objPath);
    updateManager.totalInterval = 2;
    updateManager.updateInterval = 0;
    updateManager.createProgressUpdateTimer();
    ASSERT_NE(updateManager.progressTimer, nullptr);

    updateManager.progressTimer->start(std::chrono::seconds(0), true);
    EXPECT_GE(sd_event_run(event.get(), 500000), 0);
    EXPECT_GE(updateManager.updateInterval, 1);
    updateManager.progressTimer->stop();
}

TEST_F(UpdateManagerTest, createProgressUpdateTimerStopsAtTimeoutBoundary)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath =
        "/xyz/openbmc_project/software/progress_timeout_boundary";
    updateManager.activationProgress =
        std::make_unique<ActivationProgress>(busMock, updateManager.objPath);
    updateManager.totalInterval = 2;
    updateManager.createProgressUpdateTimer();
    ASSERT_NE(updateManager.progressTimer, nullptr);

    updateManager.progressTimer->start(std::chrono::milliseconds(1), true);
    waitEventExpiry(std::chrono::milliseconds(20));

    EXPECT_EQ(updateManager.updateInterval, 2U);
}

TEST_F(UpdateManagerTest, startNonPLDMUpdateWithPLDMDeviceReturnsActivating)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    std::vector<sdbusplus::message::object_path> targets;
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                   targets);
    updateManager.deviceUpdaterMap.emplace(0x01, nullptr);

    auto state = updateManager.startNonPLDMUpdate();
    EXPECT_EQ(state, Server::Activation::Activations::Activating);
}

TEST_F(UpdateManagerTest, updateOtherDeviceCompletionStoresResult)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.deviceUpdaterMap.emplace(0x01, nullptr);
    updateManager.otherDeviceComponents = {{"UUID_1", true}};
    updateManager.totalNumComponentUpdates = 2;
    updateManager.compUpdateCompletedCount = 0;

    EXPECT_NO_THROW({
        updateManager.updateOtherDeviceCompletion("UUID_1", true, "CompA");
    });
    EXPECT_TRUE(updateManager.otherDeviceCompleted.contains("UUID_1"));
    EXPECT_EQ(updateManager.listCompNames, "CompA");
}

TEST_F(UpdateManagerTest, updateDeviceCompletionIgnoresDuplicateCompletion)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath = "/xyz/openbmc_project/software/progress_duplicate";
    updateManager.activationProgress =
        std::make_unique<ActivationProgress>(busMock, updateManager.objPath);
    updateManager.totalNumComponentUpdates = 2;

    updateManager.updateDeviceCompletion(0x01, false);
    updateManager.updateDeviceCompletion(0x01, true, {"CompA"});

    ASSERT_EQ(updateManager.deviceUpdateCompletionMap.size(), 1U);
    EXPECT_FALSE(updateManager.deviceUpdateCompletionMap.at(0x01));
    EXPECT_EQ(updateManager.compUpdateCompletedCount, 1U);
    EXPECT_TRUE(updateManager.listCompNames.empty());
}

TEST_F(UpdateManagerTest, clearExistingActivationNoActivationObjectNoop)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    EXPECT_NO_THROW({ updateManager.clearExistingActivation(); });
}

TEST_F(UpdateManagerTest, updateActivationProgressCompletesAndStopsTimer)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath = "/xyz/openbmc_project/software/progress_done";
    updateManager.activationProgress =
        std::make_unique<ActivationProgress>(busMock, updateManager.objPath);
    updateManager.totalNumComponentUpdates = 1;
    updateManager.compUpdateCompletedCount = 0;
    updateManager.createProgressUpdateTimer();
    ASSERT_NE(updateManager.progressTimer, nullptr);

    updateManager.updateActivationProgress();
    EXPECT_EQ(updateManager.compUpdateCompletedCount, 1);
    EXPECT_EQ(updateManager.progressTimer, nullptr);
    EXPECT_EQ(updateManager.activationProgress->progress(), 100);
}

TEST_F(UpdateManagerTest, updatePackageCompletionSetsActiveOnAllSuccess)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    mapPackageToUpdater(updateManager, "./test_pkg");

    updateManager.objPath = "/xyz/openbmc_project/software/pkg_success";
    updateManager.activation = std::make_unique<Activation>(
        busMock, updateManager.objPath,
        software::Activation::Activations::Ready, &updateManager);
    updateManager.deviceUpdaterMap.emplace(0x1, nullptr);
    updateManager.deviceUpdateCompletionMap.emplace(0x1, true);
    updateManager.otherDeviceComponents.emplace("UUID_A", true);
    updateManager.otherDeviceCompleted.emplace("UUID_A", true);
    updateManager.listCompNames = "CompA";
    updateManager.activationBlocksTransition = nullptr;

    EXPECT_NO_THROW({ updateManager.updatePackageCompletion(); });
    EXPECT_EQ(updateManager.activation->activation(),
              software::Activation::Activations::Active);
}

TEST_F(UpdateManagerTest, updatePackageCompletionSetsFailedWhenAnyFailure)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    mapPackageToUpdater(updateManager, "./test_pkg");

    updateManager.objPath = "/xyz/openbmc_project/software/pkg_failed";
    updateManager.activation = std::make_unique<Activation>(
        busMock, updateManager.objPath,
        software::Activation::Activations::Ready, &updateManager);
    updateManager.deviceUpdaterMap.emplace(0x1, nullptr);
    updateManager.deviceUpdateCompletionMap.emplace(0x1, true);
    updateManager.otherDeviceComponents.emplace("UUID_B", true);
    updateManager.otherDeviceCompleted.emplace("UUID_B", false);
    updateManager.listCompNames = "CompB";
    updateManager.activationBlocksTransition = nullptr;

    EXPECT_NO_THROW({ updateManager.updatePackageCompletion(); });
    EXPECT_EQ(updateManager.activation->activation(),
              software::Activation::Activations::Failed);
}

TEST_F(UpdateManagerTest, startPLDMUpdateWithMappedDeviceExecutesLoop)
{
    mctp_eid_t eid = 0;
    ComponentInfoMap componentInfoMap2{
        {eid,
         {{std::make_pair(10, 100),
           std::make_tuple(static_cast<uint8_t>(1), "comp1Version",
                           static_cast<uint16_t>(0))}}}};
    const DescriptorMap descriptorMap2{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0x0d}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xc9, 0x3e, 0xc5, 0x41, 0x15,
                                0x95, 0xf4, 0x48, 0x70, 0x1d, 0x49, 0xd6,
                                0x75}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x01, 0x02})}}}};

    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap2,
                                componentInfoMap2, componentNameMap, true,
                                nullptr);
    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    ASSERT_FALSE(updateManager.deviceUpdaterMap.empty());

    EXPECT_NO_THROW({ updateManager.startPLDMUpdate(); });
}

#ifdef OEM_NVIDIA
TEST_F(UpdateManagerTest, startNonPLDMUpdateReturnsFailedWhenNoDevicesOrImages)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    std::vector<sdbusplus::message::object_path> targets;
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                   targets);
    updateManager.debugToken =
        std::make_unique<DebugToken>(busMock, &updateManager);
    updateManager.objPath = "/xyz/openbmc_project/software/non_pldm_none";
    updateManager.createProgressUpdateTimer();

    auto state = updateManager.startNonPLDMUpdate();
    EXPECT_EQ(state, Server::Activation::Activations::Failed);
}
#endif

TEST_F(UpdateManagerTest,
       startNonPLDMUpdateReturnsFailedWhenActivationFailsAndNoPLDMDevices)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    std::vector<sdbusplus::message::object_path> targets;
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                   targets);

    const std::string path = "/xyz/openbmc_project/software/other/fail_entry";
    const std::string uuid = "UUID_NON_PLDM_FAIL";
    auto tracked = std::make_unique<OtherDeviceUpdateActivation>();
    tracked->uuid = uuid;
    tracked->activationState = Server::Activation::Activations::Ready;
    tracked->requestedActivation =
        Server::Activation::RequestedActivations::None;
    updateManager.otherDeviceUpdateManager->otherDevices[path] =
        std::move(tracked);
    updateManager.otherDeviceUpdateManager->uuidMappings[uuid] = {
        "1.0", "CompFail"};
    updateManager.otherDeviceUpdateManager->isImageFileProcessed[uuid] = false;

    auto state = updateManager.startNonPLDMUpdate();
    EXPECT_EQ(state, Server::Activation::Activations::Failed);
}

TEST_F(UpdateManagerTest, clearActivationInfoStopsAndResetsProgressTimer)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath = "/xyz/openbmc_project/software/clear_info";
    updateManager.createProgressUpdateTimer();
    ASSERT_NE(updateManager.progressTimer, nullptr);
    updateManager.progressTimer->start(std::chrono::seconds(1), true);

    updateManager.clearActivationInfo();
    EXPECT_EQ(updateManager.progressTimer, nullptr);
    EXPECT_TRUE(updateManager.objPath.empty());
}

TEST_F(UpdateManagerTest, setActivationStatusUpdatesExistingActivation)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath = "/xyz/openbmc_project/software/set_activation";
    updateManager.activation = std::make_unique<Activation>(
        busMock, updateManager.objPath,
        software::Activation::Activations::Ready, &updateManager);

    updateManager.setActivationStatus(
        software::Activation::Activations::Active);
    EXPECT_EQ(updateManager.activation->activation(),
              software::Activation::Activations::Active);
}

TEST_F(UpdateManagerTest, clearExistingActivationWhenActivatingClearsState)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath = "/xyz/openbmc_project/software/clear_existing";
    updateManager.activation = std::make_unique<Activation>(
        busMock, updateManager.objPath,
        software::Activation::Activations::Ready, &updateManager);
    updateManager.activation->sdbusplus::xyz::openbmc_project::Software::
        server::Activation::activation(
            software::Activation::Activations::Activating);
    updateManager.createProgressUpdateTimer();

    updateManager.clearExistingActivation();
    EXPECT_EQ(updateManager.activation, nullptr);
    EXPECT_TRUE(updateManager.objPath.empty());
}

TEST_F(UpdateManagerTest, createMessageRegistryUsesMappedAndFallbackNames)
{
    ComponentNameMap localNames;
    {
        UpdateManager updateManager1(event, reqHandler, instanceIdDb,
                                     descriptorMap, componentInfoMap,
                                     localNames, true, nullptr);
        ASSERT_EQ(processPackageStream(updateManager1, "./test_pkg"), 0);
        ASSERT_NE(updateManager1.parser, nullptr);
        const auto& record =
            updateManager1.parser->getFwDeviceIDRecords().front();
        const auto& compImageInfos =
            updateManager1.parser->getComponentImageInfos();
        const auto compIndex = std::get<ApplicableComponents>(record).front();
        const auto compIdentifier = std::get<static_cast<size_t>(
            ComponentImageInfoPos::CompIdentifierPos)>(
            compImageInfos[compIndex]);

        localNames = {{1, {{compIdentifier, "NamedComp"}}}};
        EXPECT_NO_THROW({
            updateManager1.createMessageRegistry(
                1, record, 0, "Update.1.0.TargetDetermined", "",
                static_cast<pldm_firmware_update_commands>(0), 0);
        });

        localNames = {{1, {{static_cast<CompIdentifier>(9999), "OtherComp"}}}};
        EXPECT_NO_THROW({
            updateManager1.createMessageRegistry(
                1, record, 0, "Update.1.0.TargetDetermined", "",
                static_cast<pldm_firmware_update_commands>(0), 0);
        });
    }

    ComponentNameMap emptyNames;
    {
        UpdateManager updateManager2(event, reqHandler, instanceIdDb,
                                     descriptorMap, componentInfoMap,
                                     emptyNames, true, nullptr);
        ASSERT_EQ(processPackageStream(updateManager2, "./test_pkg"), 0);
        ASSERT_NE(updateManager2.parser, nullptr);
        const auto& record2 =
            updateManager2.parser->getFwDeviceIDRecords().front();
        EXPECT_NO_THROW({
            updateManager2.createMessageRegistry(
                1, record2, 0, "Update.1.0.TargetDetermined", "",
                static_cast<pldm_firmware_update_commands>(0), 0);
        });
    }
}

TEST_F(UpdateManagerTest,
       createMessageRegistryResourceErrorsUsesMappedAndFallbackNames)
{
    ComponentNameMap localNames;
    {
        UpdateManager updateManager1(event, reqHandler, instanceIdDb,
                                     descriptorMap, componentInfoMap,
                                     localNames, true, nullptr);
        ASSERT_EQ(processPackageStream(updateManager1, "./test_pkg"), 0);
        ASSERT_NE(updateManager1.parser, nullptr);
        const auto& record =
            updateManager1.parser->getFwDeviceIDRecords().front();
        const auto& compImageInfos =
            updateManager1.parser->getComponentImageInfos();
        const auto compIndex = std::get<ApplicableComponents>(record).front();
        const auto compIdentifier = std::get<static_cast<size_t>(
            ComponentImageInfoPos::CompIdentifierPos)>(
            compImageInfos[compIndex]);

        localNames = {{1, {{compIdentifier, "NamedComp"}}}};
        EXPECT_NO_THROW({
            updateManager1.createMessageRegistryResourceErrors(
                1, record, 0, "Update.1.0.TransferFailed", "transfer error",
                "Retry firmware update operation");
        });

        localNames = {{1, {{static_cast<CompIdentifier>(8888), "OtherComp"}}}};
        EXPECT_NO_THROW({
            updateManager1.createMessageRegistryResourceErrors(
                1, record, 0, "Update.1.0.TransferFailed", "transfer error",
                "Retry firmware update operation");
        });
    }

    ComponentNameMap emptyNames;
    {
        UpdateManager updateManager2(event, reqHandler, instanceIdDb,
                                     descriptorMap, componentInfoMap,
                                     emptyNames, true, nullptr);
        ASSERT_EQ(processPackageStream(updateManager2, "./test_pkg"), 0);
        ASSERT_NE(updateManager2.parser, nullptr);
        const auto& record2 =
            updateManager2.parser->getFwDeviceIDRecords().front();
        EXPECT_NO_THROW({
            updateManager2.createMessageRegistryResourceErrors(
                1, record2, 0, "Update.1.0.TransferFailed", "transfer error",
                "Retry firmware update operation");
        });
    }
}

TEST_F(UpdateManagerTest, processPackageInvokesRefreshCallbackWhenConfigured)
{
    DescriptorMap localDescriptorMap;
    localDescriptorMap.emplace(1, Descriptors{});
    std::vector<std::pair<mctp_eid_t, bool>> refreshCalls;

    UpdateManager updateManager(
        event, reqHandler, instanceIdDb, localDescriptorMap, componentInfoMap,
        componentNameMap, true,
        [&refreshCalls](mctp_eid_t eid, bool isTarget) -> exec::task<int> {
            refreshCalls.emplace_back(eid, isTarget);
            co_return 0;
        });

    EXPECT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    EXPECT_FALSE(refreshCalls.empty());
}

TEST_F(UpdateManagerTest, getComponentTargetListMergesComponentsForSameEid)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    ComponentNameMap names{
        {1, {{10, "CompA"}, {11, "CompB"}}},
    };
    std::vector<sdbusplus::message::object_path> targets{
        sdbusplus::message::object_path("/xyz/openbmc_project/software/CompA"),
        sdbusplus::message::object_path("/xyz/openbmc_project/software/CompB")};

    auto compTargetList = updateManager.getComponentTargetList(names, targets);
    ASSERT_TRUE(compTargetList.contains(1));
    EXPECT_EQ(compTargetList[1].size(), 2);
}

TEST_F(UpdateManagerTest, updateOtherDeviceCompletionAppendsMultipleNames)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.deviceUpdaterMap.emplace(0x1, nullptr);
    updateManager.otherDeviceComponents = {{"UUID_1", true}, {"UUID_2", true}};
    updateManager.totalNumComponentUpdates = 3;
    updateManager.compUpdateCompletedCount = 0;

    updateManager.updateOtherDeviceCompletion("UUID_1", true, "CompA");
    updateManager.updateOtherDeviceCompletion("UUID_2", true, "CompB");

    EXPECT_EQ(updateManager.listCompNames, "CompA CompB");
}

TEST_F(UpdateManagerTest, handleInvalidPackageErrorWithExistingActivation)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath = "/xyz/openbmc_project/software/invalid_pkg";
    updateManager.activation = std::make_unique<Activation>(
        busMock, updateManager.objPath,
        software::Activation::Activations::Ready, &updateManager);

    EXPECT_NO_THROW({ updateManager.handleInvalidPackageError(); });
    EXPECT_EQ(updateManager.activation->activation(),
              software::Activation::Activations::Failed);
}

TEST_F(UpdateManagerTest, handleInvalidPackageErrorWithNullUpdater)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath =
        "/xyz/openbmc_project/software/invalid_pkg_no_updater";
    updateManager.updater.reset();

    EXPECT_NO_THROW({ updateManager.handleInvalidPackageError(); });
    ASSERT_NE(updateManager.activation, nullptr);
    EXPECT_EQ(updateManager.activation->activation(),
              software::Activation::Activations::Failed);
}

TEST_F(UpdateManagerTest, handlePayloadChecksumErrorWithExistingActivation)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath = "/xyz/openbmc_project/software/payload_error";
    updateManager.activation = std::make_unique<Activation>(
        busMock, updateManager.objPath,
        software::Activation::Activations::Ready, &updateManager);

    EXPECT_NO_THROW({ updateManager.handlePayloadChecksumError(); });
    EXPECT_EQ(updateManager.activation->activation(),
              software::Activation::Activations::Failed);
}

TEST_F(UpdateManagerTest, handlePayloadChecksumErrorWithNullUpdater)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath =
        "/xyz/openbmc_project/software/payload_error_no_updater";
    updateManager.updater.reset();

    EXPECT_NO_THROW({ updateManager.handlePayloadChecksumError(); });
    ASSERT_NE(updateManager.activation, nullptr);
    EXPECT_EQ(updateManager.activation->activation(),
              software::Activation::Activations::Failed);
}

TEST_F(UpdateManagerTest, processPackageReusesExistingOtherDeviceManager)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    auto* existingManager = updateManager.otherDeviceUpdateManager.get();
    ASSERT_NE(existingManager, nullptr);

    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    EXPECT_EQ(updateManager.otherDeviceUpdateManager.get(), existingManager);
}

TEST_F(UpdateManagerTest, getActivationMethodIgnoresUnknownBits)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    bitfield16_t unknownOnly{};
    unknownOnly.value = static_cast<uint16_t>(1U << 15);
    EXPECT_TRUE(updateManager.getActivationMethod(unknownOnly).empty());

    bitfield16_t mixedKnownUnknown{};
    mixedKnownUnknown.value = static_cast<uint16_t>(0x1 | (1U << 15));
    EXPECT_EQ(updateManager.getActivationMethod(mixedKnownUnknown),
              "Automatic");
}

TEST_F(UpdateManagerTest, startNonPLDMUpdateNoDevicesUsesExistingProgressObject)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    std::vector<sdbusplus::message::object_path> targets;
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                   targets);

#ifdef OEM_NVIDIA
    updateManager.debugToken =
        std::make_unique<DebugToken>(busMock, &updateManager);
    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
#endif

    updateManager.objPath = "/xyz/openbmc_project/software/non_pldm_progress";
    updateManager.activationProgress =
        std::make_unique<ActivationProgress>(busMock, updateManager.objPath);
    updateManager.createProgressUpdateTimer();
    ASSERT_NE(updateManager.progressTimer, nullptr);

    auto state = updateManager.startNonPLDMUpdate();
    EXPECT_EQ(state, Server::Activation::Activations::Failed);
    EXPECT_NE(updateManager.activationProgress, nullptr);
}

TEST_F(UpdateManagerTest, clearFirmwareUpdatePackageHandlesNullUpdater)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.updater.reset();
    EXPECT_NO_THROW({ updateManager.clearFirmwareUpdatePackage(); });
}

TEST_F(UpdateManagerTest, updateOtherDeviceCompletionIgnoresDuplicateUuid)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath = "/xyz/openbmc_project/software/dup_other_uuid";
    updateManager.activation = std::make_unique<Activation>(
        busMock, updateManager.objPath,
        software::Activation::Activations::Ready, &updateManager);
    updateManager.otherDeviceComponents = {
        {"UUID_1", true},
        {"UUID_2", true},
    };
    updateManager.totalNumComponentUpdates = 3;
    updateManager.compUpdateCompletedCount = 0;

    updateManager.updateOtherDeviceCompletion("UUID_1", true, "CompA");
    updateManager.updateOtherDeviceCompletion("UUID_1", false, "CompB");

    EXPECT_EQ(updateManager.otherDeviceCompleted.size(), 1U);
    EXPECT_EQ(updateManager.compUpdateCompletedCount, 1U);
    EXPECT_EQ(updateManager.listCompNames, "CompA");
}

TEST_F(UpdateManagerTest, clearExistingActivationWhenReadyClearsState)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath = "/xyz/openbmc_project/software/clear_ready";
    updateManager.activation = std::make_unique<Activation>(
        busMock, updateManager.objPath,
        software::Activation::Activations::Ready, &updateManager);

    updateManager.clearExistingActivation();
    EXPECT_EQ(updateManager.activation, nullptr);
    EXPECT_TRUE(updateManager.objPath.empty());
}

TEST_F(UpdateManagerTest, updatePackageCompletionWithEmptyComponentList)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath = "/xyz/openbmc_project/software/pkg_no_components";
    updateManager.activation = std::make_unique<Activation>(
        busMock, updateManager.objPath,
        software::Activation::Activations::Ready, &updateManager);
    updateManager.deviceUpdaterMap.emplace(0x1, nullptr);
    updateManager.deviceUpdateCompletionMap.emplace(0x1, true);
    updateManager.listCompNames.clear();

    EXPECT_NO_THROW({ updateManager.updatePackageCompletion(); });
    EXPECT_EQ(updateManager.activation->activation(),
              software::Activation::Activations::Active);
}

TEST_F(UpdateManagerTest, handleInvalidPackageErrorCreatesActivationWhenMissing)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath =
        "/xyz/openbmc_project/software/invalid_pkg_create_activation";

    ASSERT_EQ(updateManager.activation, nullptr);
    updateManager.handleInvalidPackageError();
    ASSERT_NE(updateManager.activation, nullptr);
    EXPECT_EQ(updateManager.activation->activation(),
              software::Activation::Activations::Failed);
}

TEST_F(UpdateManagerTest,
       handlePayloadChecksumErrorCreatesActivationWhenMissing)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath =
        "/xyz/openbmc_project/software/payload_error_create_activation";

    ASSERT_EQ(updateManager.activation, nullptr);
    updateManager.handlePayloadChecksumError();
    ASSERT_NE(updateManager.activation, nullptr);
    EXPECT_EQ(updateManager.activation->activation(),
              software::Activation::Activations::Failed);
}

TEST_F(UpdateManagerTest,
       handleInvalidPackageHeaderErrorCreatesActivationWhenMissing)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath =
        "/xyz/openbmc_project/software/header_error_create_activation";

    ASSERT_EQ(updateManager.activation, nullptr);
    updateManager.handleInvalidPackageHeaderError();
    ASSERT_NE(updateManager.activation, nullptr);
    EXPECT_EQ(updateManager.activation->activation(),
              software::Activation::Activations::Failed);
}

TEST_F(UpdateManagerTest, handleInvalidPackageHeaderErrorWithNullUpdater)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath =
        "/xyz/openbmc_project/software/header_error_no_updater";
    updateManager.updater.reset();

    EXPECT_NO_THROW({ updateManager.handleInvalidPackageHeaderError(); });
    ASSERT_NE(updateManager.activation, nullptr);
    EXPECT_EQ(updateManager.activation->activation(),
              software::Activation::Activations::Failed);
}

TEST_F(UpdateManagerTest, updateOtherDeviceComponentsWithEmptyMapNoop)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    std::unordered_map<std::string, bool> otherDeviceMap;

    EXPECT_NO_THROW({
        updateManager.updateOtherDeviceComponents(otherDeviceMap);
    });
    EXPECT_TRUE(updateManager.otherDeviceComponents.empty());
}

TEST_F(UpdateManagerTest,
       updateOtherDeviceComponentsNonEmptyWithoutActivationObject)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    std::unordered_map<std::string, bool> otherDeviceMap{{"UUID_A", true},
                                                         {"UUID_B", false}};
    updateManager.activation.reset();

    EXPECT_NO_THROW({
        updateManager.updateOtherDeviceComponents(otherDeviceMap);
    });
    EXPECT_EQ(updateManager.otherDeviceComponents.size(), 2U);
}

TEST_F(UpdateManagerTest, clearActivationInfoResetsAllTrackedMembers)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath =
        "/xyz/openbmc_project/software/clear_activation_resets_members";
    updateManager.activation = std::make_unique<Activation>(
        busMock, updateManager.objPath,
        software::Activation::Activations::Ready, &updateManager);
    updateManager.activationProgress =
        std::make_unique<ActivationProgress>(busMock, updateManager.objPath);
    updateManager.activationBlocksTransition =
        std::make_unique<ActivationBlocksTransition>(busMock,
                                                     updateManager.objPath);
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(
            busMock, &updateManager,
            std::vector<sdbusplus::message::object_path>{});
    updateManager.parser = std::make_unique<PackageParser>(
        static_cast<PackageHeaderSize>(1), std::string{},
        static_cast<ComponentBitmapBitLength>(8), 1);

    EXPECT_NO_THROW({ updateManager.clearActivationInfo(); });
    EXPECT_EQ(updateManager.activation, nullptr);
    EXPECT_EQ(updateManager.activationProgress, nullptr);
    EXPECT_EQ(updateManager.activationBlocksTransition, nullptr);
    EXPECT_EQ(updateManager.parser, nullptr);
    EXPECT_EQ(updateManager.otherDeviceUpdateManager, nullptr);
}
