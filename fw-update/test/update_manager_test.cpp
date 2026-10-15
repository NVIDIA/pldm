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
#include "common/test/mocked_utils.hpp"
#include "common/utils.hpp"
#include "fw-update/error_handling.hpp"
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
#include <xyz/openbmc_project/Common/error.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <set>
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

    updateManager.updater->clearImageData();
    int imageFd = open(packagePath.c_str(), O_RDONLY);
    if (imageFd < 0)
    {
        return false;
    }

    return updateManager.updater->mmapFile.map(imageFd, true);
}

static int processPackageStream(
    UpdateManager& updateManager, const std::filesystem::path& packagePath,
    std::vector<sdbusplus::object_path> targets = {},
    bool preUpdateValidation = false)
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
                    targets);
        }

        auto task = updateManager.processPackageData(
            updateManager.updater->getImageData(),
            updateManager.updater->getImageSize(), std::move(targets),
            preUpdateValidation);
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

    void SetUp() override
    {
        pldm::utils::RecordingAsioConnectionProvider::getAsioConnection()
            ->reset();
    }

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
    TestInstanceIdDb instanceIdDb;
    sdbusplus::bus_t busMock;
    std::optional<sdbusplus::bus_t> savedStaticBus{};
    bool staticBusSwapped = false;
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

TEST_F(UpdateManagerTest, markComponentUpdateCompleted)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    EXPECT_NO_THROW({ updateManager.markComponentUpdateCompleted(); });
}

TEST_F(UpdateManagerTest,
       markComponentUpdateCompletedIncompleteKeepsTimerRunning)
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

    updateManager.markComponentUpdateCompleted();

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

TEST_F(UpdateManagerTest, onResponseSendCompleteIgnoresUnknownEid)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    EXPECT_NO_THROW({ updateManager.onResponseSendComplete(0x77, true); });
}

TEST_F(UpdateManagerTest, onResponseSendCompleteDelegatesToTrackedDeviceUpdater)
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
    ASSERT_TRUE(updateManager.deviceUpdaterMap.contains(eid));

    EXPECT_NO_THROW({
        updateManager.onResponseSendComplete(eid, true);
        updateManager.onResponseSendComplete(eid, false);
    });
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

    EXPECT_ANY_THROW({
        auto task = updateManager.processPackageData(nullptr, 10, {});
        stdexec::sync_wait(std::move(task));
    });
}

TEST_F(UpdateManagerTest, processStream_sizeTooSmall)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    std::vector<uint8_t> pkg{'a', 'b', 'c'};

    EXPECT_ANY_THROW({
        auto task = updateManager.processPackageData(pkg.data(), 1, {});
        stdexec::sync_wait(std::move(task));
    });
}

TEST_F(UpdateManagerTest, processStream_nonMmapInvalidHeaderPath)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    std::vector<uint8_t> bytes(sizeof(pldm_package_header_information) + 8, 0);

    EXPECT_ANY_THROW({
        auto task =
            updateManager.processPackageData(bytes.data(), bytes.size(), {});
        stdexec::sync_wait(std::move(task));
    });
}

TEST_F(UpdateManagerTest, processStreamDefer_noMatchingDevices)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    mapPackageToUpdater(updateManager, "./test_pkg");
    auto* package = updateManager.updater->getImageData();
    auto packageSize = updateManager.updater->getImageSize();
    updateManager.setRequestedApplyTime(
        sdbusplus::xyz::openbmc_project::Software::server::ApplyTime::
            RequestedApplyTimes::Immediate);

    auto& connection =
        pldm::utils::RecordingAsioConnectionProvider::getAsioConnection();
    connection->reset();

    EXPECT_NO_THROW(updateManager.processPackageDataDefer(
        package, packageSize, false, {}, false));
    ASSERT_EQ(connection->loggingCalls.size(), 1U);
    EXPECT_EQ(connection->loggingCalls.front().messageId,
              resourceErrorDetected);
}

TEST_F(UpdateManagerTest,
       processStreamDeferExecutesDeferredPathWithRefreshCallback)
{
    DescriptorMap localDescriptorMap;
    localDescriptorMap.emplace(1, Descriptors{});

    UpdateManager updateManager(
        event, reqHandler, instanceIdDb, localDescriptorMap, componentInfoMap,
        componentNameMap, true,
        [](mctp_eid_t, bool, bool) -> exec::task<int> { co_return 0; });

    mapPackageToUpdater(updateManager, "./test_pkg");
    auto* package = updateManager.updater->getImageData();
    auto packageSize = updateManager.updater->getImageSize();
    updateManager.setRequestedApplyTime(
        sdbusplus::xyz::openbmc_project::Software::server::ApplyTime::
            RequestedApplyTimes::Immediate);

    EXPECT_NO_THROW(updateManager.processPackageDataDefer(
        package, packageSize, false, {}, false));
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

TEST_F(UpdateManagerTest, commandFailureReportsMappedNvidiaMessageAlone)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    ASSERT_NE(updateManager.parser, nullptr);
    const auto& record = updateManager.parser->getFwDeviceIDRecords().front();

    auto& connection =
        pldm::utils::RecordingAsioConnectionProvider::getAsioConnection();
    connection->reset();
    updateManager.createMessageRegistry(1, record, 0, applyFailed, "",
                                        PLDM_GET_STATUS, COMMAND_TIMEOUT);

#ifdef OEM_NVIDIA
    ASSERT_EQ(connection->loggingCalls.size(), 1U);
    EXPECT_EQ(connection->loggingCalls[0].messageId,
              "NvidiaUpdate.1.3.PLDMCommandTimedOut");
#else
    ASSERT_EQ(connection->loggingCalls.size(), 2U);
    EXPECT_EQ(connection->loggingCalls[0].messageId, applyFailed);
    EXPECT_EQ(connection->loggingCalls[1].messageId, resourceErrorDetected);
#endif
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

#ifdef OEM_NVIDIA
TEST_F(UpdateManagerTest, packageValidationHandlersUseSpecificMessages)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    ASSERT_NE(updateManager.parser, nullptr);
    const auto packageVersion = updateManager.parser->pkgVersion;
    updateManager.objPath = "/xyz/openbmc_project/software/package_errors";
    updateManager.activation = std::make_unique<Activation>(
        busMock, updateManager.objPath,
        software::Activation::Activations::Ready, &updateManager);

    auto& connection =
        pldm::utils::RecordingAsioConnectionProvider::getAsioConnection();

    connection->reset();
    updateManager.handleInvalidPackageError();
    ASSERT_EQ(connection->loggingCalls.size(), 1U);
    EXPECT_EQ(connection->loggingCalls[0].messageId,
              "NvidiaUpdate.1.3.FirmwarePackageHeaderUnavailable");
    EXPECT_FALSE(connection->loggingCalls[0].additionalData.contains(
        "REDFISH_MESSAGE_ARGS"));

    connection->reset();
    updateManager.handleInvalidPackageHeaderError();
    ASSERT_EQ(connection->loggingCalls.size(), 1U);
    EXPECT_EQ(connection->loggingCalls[0].messageId,
              "NvidiaUpdate.1.3.FirmwarePackageHeaderInvalid");
    EXPECT_FALSE(connection->loggingCalls[0].additionalData.contains(
        "REDFISH_MESSAGE_ARGS"));

    connection->reset();
    updateManager.handleHeaderChecksumError(0x12AB34CD, 0x56EF7890);
    ASSERT_EQ(connection->loggingCalls.size(), 1U);
    EXPECT_EQ(connection->loggingCalls[0].messageId,
              "NvidiaUpdate.1.3.PackageHeaderChecksumMismatch");
    EXPECT_EQ(
        connection->loggingCalls[0].additionalData["REDFISH_MESSAGE_ARGS"],
        "0x12AB34CD,0x56EF7890");

    connection->reset();
    updateManager.handlePayloadChecksumError(0xA1B2C3D4, 0x5E6F7081);
    ASSERT_EQ(connection->loggingCalls.size(), 1U);
    EXPECT_EQ(connection->loggingCalls[0].messageId,
              "NvidiaUpdate.1.3.PackagePayloadChecksumMismatch");
    EXPECT_EQ(
        connection->loggingCalls[0].additionalData["REDFISH_MESSAGE_ARGS"],
        packageVersion + ",0xA1B2C3D4,0x5E6F7081");

    connection->reset();
    updateManager.handleInvalidPackageMetadataError();
    ASSERT_EQ(connection->loggingCalls.size(), 1U);
    EXPECT_EQ(connection->loggingCalls[0].messageId,
              "NvidiaUpdate.1.3.FirmwarePackageMetadataInvalid");
    EXPECT_FALSE(connection->loggingCalls[0].additionalData.contains(
        "REDFISH_MESSAGE_ARGS"));
}

TEST_F(UpdateManagerTest, processPackageDataReportsHeaderChecksumMismatch)
{
    // Package header from PackageParser.InvalidPkgBadChecksum: the trailing
    // checksum does not match the header contents.
    const std::vector<uint8_t> fwPkgHdr{
        0xF0, 0x18, 0x87, 0x8C, 0xCB, 0x7D, 0x49, 0x43, 0x98, 0x00, 0xA0, 0x2F,
        0x05, 0x9A, 0xCA, 0x02, 0x01, 0x8B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x19, 0x0C, 0xE5, 0x07, 0x00, 0x08, 0x00, 0x01, 0x0E,
        0x56, 0x65, 0x72, 0x73, 0x69, 0x6F, 0x6E, 0x53, 0x74, 0x72, 0x69, 0x6E,
        0x67, 0x31, 0x01, 0x2E, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0E,
        0x00, 0x00, 0x01, 0x56, 0x65, 0x72, 0x73, 0x69, 0x6F, 0x6E, 0x53, 0x74,
        0x72, 0x69, 0x6E, 0x67, 0x32, 0x02, 0x00, 0x10, 0x00, 0x16, 0x20, 0x23,
        0xC9, 0x3E, 0xC5, 0x41, 0x15, 0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
        0x75, 0x01, 0x00, 0x0A, 0x00, 0x64, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
        0x00, 0x00, 0x00, 0x8B, 0x00, 0x00, 0x00, 0x1B, 0x00, 0x00, 0x00, 0x01,
        0x0E, 0x56, 0x65, 0x72, 0x73, 0x69, 0x6F, 0x6E, 0x53, 0x74, 0x72, 0x69,
        0x6E, 0x67, 0x33, 0x4F, 0x96, 0xAE, 0x57};
    constexpr size_t pkgSize = 166;

    PackageHeaderDiagnostic diagnostic;
    ASSERT_EQ(parsePkgHeader(fwPkgHdr.data(), fwPkgHdr.size(), &diagnostic),
              nullptr);
    ASSERT_EQ(diagnostic.error, PackageHeaderError::checksumMismatch);

    std::vector<uint8_t> package(fwPkgHdr);
    package.resize(pkgSize, 0xA5);
    const auto packagePath =
        std::filesystem::temp_directory_path() / "pldm_bad_checksum_pkg.bin";
    {
        std::ofstream out(packagePath, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(package.data()),
                  static_cast<std::streamsize>(package.size()));
        ASSERT_TRUE(out);
        out.close();
        ASSERT_TRUE(out);
    }

    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath = "/xyz/openbmc_project/software/bad_checksum";
    updateManager.activation = std::make_unique<Activation>(
        busMock, updateManager.objPath,
        software::Activation::Activations::Ready, &updateManager);

    auto& connection =
        pldm::utils::RecordingAsioConnectionProvider::getAsioConnection();
    connection->reset();
    EXPECT_EQ(processPackageStream(updateManager, packagePath), -1);
    EXPECT_EQ(updateManager.parser, nullptr);
    std::filesystem::remove(packagePath);

    ASSERT_EQ(connection->loggingCalls.size(), 1U);
    EXPECT_EQ(connection->loggingCalls[0].messageId,
              "NvidiaUpdate.1.3.PackageHeaderChecksumMismatch");
    EXPECT_EQ(
        connection->loggingCalls[0].additionalData["REDFISH_MESSAGE_ARGS"],
        std::format("0x{:08X},0x{:08X}", diagnostic.expectedChecksum,
                    diagnostic.calculatedChecksum));
}
#endif

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
#ifdef OEM_NVIDIA
    auto& connection =
        pldm::utils::RecordingAsioConnectionProvider::getAsioConnection();
    connection->reset();
#endif
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
#ifdef OEM_NVIDIA
    ASSERT_EQ(connection->loggingCalls.size(), 1U);
    EXPECT_EQ(connection->loggingCalls[0].messageId,
              "NvidiaUpdate.1.3.PackageSignatureIntegrityFailed");
    EXPECT_EQ(
        connection->loggingCalls[0].additionalData["REDFISH_MESSAGE_ARGS"],
        updateManager.parser->pkgVersion);
#endif
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
#ifdef OEM_NVIDIA
    auto& connection =
        pldm::utils::RecordingAsioConnectionProvider::getAsioConnection();
    connection->reset();
#endif
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
#ifdef OEM_NVIDIA
    ASSERT_EQ(connection->loggingCalls.size(), 1U);
    EXPECT_EQ(connection->loggingCalls[0].messageId,
              "NvidiaUpdate.1.3.PackageSignatureVerificationFailed");
    EXPECT_EQ(
        connection->loggingCalls[0].additionalData["REDFISH_MESSAGE_ARGS"],
        updateManager.parser->pkgVersion);
#endif
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
#if defined(OEM_NVIDIA) && defined(PLDM_PACKAGE_VERIFICATION_MUST_BE_SIGNED)
    auto& connection =
        pldm::utils::RecordingAsioConnectionProvider::getAsioConnection();
    connection->reset();
#endif
    updateManager.verifyPackageAsync(
        [&](bool) { onCompleteCalled = true; },
        [&](const std::string&) { onErrorCalled = true; });

    waitEventExpiry(milliseconds(100));
    EXPECT_TRUE(onCompleteCalled || onErrorCalled);
#if defined(OEM_NVIDIA) && defined(PLDM_PACKAGE_VERIFICATION_MUST_BE_SIGNED)
    ASSERT_EQ(connection->loggingCalls.size(), 1U);
    EXPECT_EQ(connection->loggingCalls[0].messageId,
              "NvidiaUpdate.1.3.PackageSignatureRequired");
    EXPECT_EQ(
        connection->loggingCalls[0].additionalData["REDFISH_MESSAGE_ARGS"],
        updateManager.parser->pkgVersion);
#endif
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
#ifdef OEM_NVIDIA
    auto& connection =
        pldm::utils::RecordingAsioConnectionProvider::getAsioConnection();
    connection->reset();
#endif
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
#ifdef OEM_NVIDIA
    ASSERT_EQ(connection->loggingCalls.size(), 1U);
    EXPECT_EQ(connection->loggingCalls[0].messageId,
              "NvidiaUpdate.1.3.SignatureHeaderMalformed");
    EXPECT_EQ(
        connection->loggingCalls[0].additionalData["REDFISH_MESSAGE_ARGS"],
        updateManager.parser->pkgVersion + ",signature header size is invalid");
#endif
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
       packageIntegrityCheckAsyncUnsupportedSignatureVersionFails)
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
    bool result = true;
#ifdef OEM_NVIDIA
    auto& connection =
        pldm::utils::RecordingAsioConnectionProvider::getAsioConnection();
    connection->reset();
#endif
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
#ifdef OEM_NVIDIA
    EXPECT_EQ(connection->loggingCalls.size(), 1U);
    EXPECT_EQ(connection->loggingCalls.front().messageId,
              "NvidiaUpdate.1.3.SignatureHeaderVersionUnsupported");
    EXPECT_EQ(
        connection->loggingCalls.front().additionalData["REDFISH_MESSAGE_ARGS"],
        updateManager.parser->pkgVersion + ",255");
#endif
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
#ifdef OEM_NVIDIA
    auto& connection =
        pldm::utils::RecordingAsioConnectionProvider::getAsioConnection();
    connection->reset();
#endif
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
#ifdef OEM_NVIDIA
    ASSERT_EQ(connection->loggingCalls.size(), 1U);
    EXPECT_EQ(connection->loggingCalls[0].messageId,
              "NvidiaUpdate.1.3.SignatureHeaderMalformed");
    EXPECT_EQ(
        connection->loggingCalls[0].additionalData["REDFISH_MESSAGE_ARGS"],
        updateManager.parser->pkgVersion + ",signature type is not supported");
#endif
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
    std::vector<sdbusplus::object_path> targets;
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

TEST_F(UpdateManagerTest, markComponentUpdateCompletedStopsTimer)
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

    updateManager.markComponentUpdateCompleted();
    EXPECT_EQ(updateManager.compUpdateCompletedCount, 1);
    EXPECT_EQ(updateManager.progressTimer, nullptr);
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
    updateManager.activationProgress =
        std::make_unique<ActivationProgress>(busMock, updateManager.objPath);
    updateManager.deviceUpdaterMap.emplace(0x1, nullptr);
    updateManager.deviceUpdateCompletionMap.emplace(0x1, true);
    updateManager.otherDeviceComponents.emplace("UUID_A", true);
    updateManager.otherDeviceCompleted.emplace("UUID_A", true);
    updateManager.listCompNames = "CompA";
    updateManager.activationBlocksTransition = nullptr;

    EXPECT_NO_THROW({ updateManager.updatePackageCompletion(); });
    EXPECT_EQ(updateManager.activation->activation(),
              software::Activation::Activations::Active);
    EXPECT_EQ(updateManager.activationProgress->progress(), 100);
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

TEST_F(UpdateManagerTest,
       updatePackageCompletionSetsFailedWhenExplicitTargetUnavailable)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath =
        "/xyz/openbmc_project/software/pkg_target_unavailable";
    updateManager.activation = std::make_unique<Activation>(
        busMock, updateManager.objPath,
        software::Activation::Activations::Ready, &updateManager);
    updateManager.deviceUpdaterMap.emplace(0x1, nullptr);
    updateManager.deviceUpdateCompletionMap.emplace(0x1, true);
    updateManager.unavailableTargetEids.emplace(0x2);
    updateManager.activationBlocksTransition = nullptr;

    EXPECT_NO_THROW({ updateManager.updatePackageCompletion(); });
    EXPECT_EQ(updateManager.activation->activation(),
              software::Activation::Activations::Failed);
}

TEST_F(
    UpdateManagerTest,
    updatePackageCompletionLeavesActivationUnchangedWhilePldmUpdateStillRunning)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    mapPackageToUpdater(updateManager, "./test_pkg");

    updateManager.objPath =
        "/xyz/openbmc_project/software/pkg_pldm_still_activating";
    updateManager.activation = std::make_unique<Activation>(
        busMock, updateManager.objPath,
        software::Activation::Activations::Ready, &updateManager);
    updateManager.deviceUpdaterMap.emplace(0x1, nullptr);
    updateManager.deviceUpdaterMap.emplace(0x2, nullptr);
    updateManager.deviceUpdateCompletionMap.emplace(0x1, true);
    updateManager.otherDeviceComponents.emplace("UUID_A", true);
    updateManager.otherDeviceCompleted.emplace("UUID_A", true);
    updateManager.listCompNames = "CompA";

    EXPECT_NO_THROW({ updateManager.updatePackageCompletion(); });
    EXPECT_EQ(updateManager.activation->activation(),
              software::Activation::Activations::Ready);
}

TEST_F(
    UpdateManagerTest,
    updatePackageCompletionLeavesActivationUnchangedWhileOtherDeviceUpdateStillRunning)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    mapPackageToUpdater(updateManager, "./test_pkg");

    updateManager.objPath =
        "/xyz/openbmc_project/software/pkg_other_device_still_activating";
    updateManager.activation = std::make_unique<Activation>(
        busMock, updateManager.objPath,
        software::Activation::Activations::Ready, &updateManager);
    updateManager.deviceUpdaterMap.emplace(0x1, nullptr);
    updateManager.deviceUpdateCompletionMap.emplace(0x1, true);
    updateManager.otherDeviceComponents.emplace("UUID_B", true);
    updateManager.otherDeviceComponents.emplace("UUID_C", true);
    updateManager.otherDeviceCompleted.emplace("UUID_B", true);
    updateManager.listCompNames = "CompB";

    EXPECT_NO_THROW({ updateManager.updatePackageCompletion(); });
    EXPECT_EQ(updateManager.activation->activation(),
              software::Activation::Activations::Ready);
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
    std::vector<sdbusplus::object_path> targets;
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                   targets);
    updateManager.debugToken =
        std::make_unique<DebugToken>(busMock, &updateManager);
    updateManager.objPath = "/xyz/openbmc_project/software/non_pldm_none";
    updateManager.parser = std::make_unique<PackageParser>(
        static_cast<PackageHeaderSize>(1), std::string{"1.0.0"},
        static_cast<ComponentBitmapBitLength>(8), 1);
    updateManager.createProgressUpdateTimer();

    auto& connection =
        pldm::utils::RecordingAsioConnectionProvider::getAsioConnection();
    connection->reset();

    auto state = updateManager.startNonPLDMUpdate();
    EXPECT_EQ(state, Server::Activation::Activations::Failed);
    ASSERT_EQ(connection->loggingCalls.size(), 1U);
    EXPECT_EQ(connection->loggingCalls[0].messageId,
              "NvidiaUpdate.1.3.FirmwarePackageNoMatchingDevices");
    EXPECT_EQ(
        connection->loggingCalls[0].additionalData["REDFISH_MESSAGE_ARGS"],
        "1.0.0");
}

TEST_F(UpdateManagerTest, requestedTargetsUseSpecificApplicabilityMessages)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    ASSERT_NE(updateManager.parser, nullptr);

    auto& connection =
        pldm::utils::RecordingAsioConnectionProvider::getAsioConnection();

    updateManager.requestedTargets = {{"TargetUnavailable", 9}};
    connection->reset();
    updateManager.logUnupdatedTargets({});
    ASSERT_EQ(connection->loggingCalls.size(), 1U);
    EXPECT_EQ(connection->loggingCalls[0].messageId,
              "NvidiaUpdate.1.3.RequestedTargetUnavailableForUpdate");
    EXPECT_EQ(
        connection->loggingCalls[0].additionalData["REDFISH_MESSAGE_ARGS"],
        "TargetUnavailable");

    descriptorMap.emplace(9, Descriptors{});
    updateManager.requestedTargets = {{"TargetNotInPackage", 9}};
    connection->reset();
    updateManager.logUnupdatedTargets({});
    ASSERT_EQ(connection->loggingCalls.size(), 1U);
    EXPECT_EQ(connection->loggingCalls[0].messageId,
              "NvidiaUpdate.1.3.RequestedTargetNotSupportedByPackage");
    EXPECT_EQ(
        connection->loggingCalls[0].additionalData["REDFISH_MESSAGE_ARGS"],
        "TargetNotInPackage," + updateManager.parser->pkgVersion);
}

TEST_F(UpdateManagerTest, updateComponentUsesComponentOnlyMessages)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    ASSERT_NE(updateManager.parser, nullptr);
    const auto& record = updateManager.parser->getFwDeviceIDRecords().front();
    const auto& imageInfos = updateManager.parser->getComponentImageInfos();
    const auto imageIndex = std::get<ApplicableComponents>(record).front();
    const auto& image = imageInfos[imageIndex];
    const auto identifier =
        std::get<static_cast<size_t>(ComponentImageInfoPos::CompIdentifierPos)>(
            image);
    const auto comparisonStamp = std::get<static_cast<size_t>(
        ComponentImageInfoPos::CompComparisonStampPos)>(image);
    std::ostringstream stamp;
    stamp << "0x" << std::uppercase << std::hex << std::setw(8)
          << std::setfill('0') << comparisonStamp;
    componentNameMap = {{7, {{identifier, "ComponentA"}}}};

    auto& connection =
        pldm::utils::RecordingAsioConnectionProvider::getAsioConnection();
    struct TestCase
    {
        uint8_t responseCode;
        std::string messageId;
        std::string arguments;
    };
    const auto commonArguments =
        "ComponentA," + updateManager.parser->pkgVersion;
    const std::array cases{
        TestCase{PLDM_CCRC_COMP_COMPARISON_STAMP_IDENTICAL,
                 "NvidiaUpdate.1.3.ComponentImageIdentical",
                 commonArguments + "," + stamp.str()},
        TestCase{PLDM_CCRC_COMP_COMPARISON_STAMP_LOWER,
                 "NvidiaUpdate.1.3.ComponentDowngradeRejected",
                 commonArguments + "," + stamp.str()},
    };

    for (const auto& test : cases)
    {
        SCOPED_TRACE(test.messageId + test.arguments);
        connection->reset();
        updateManager.createUpdateComponentResponseMessage(7, record, 0,
                                                           test.responseCode);
        EXPECT_EQ(connection->loggingCalls.size(), 1U);
        EXPECT_EQ(connection->loggingCalls.front().messageId, test.messageId);
        EXPECT_EQ(connection->loggingCalls.front()
                      .additionalData["REDFISH_MESSAGE_ARGS"],
                  test.arguments);
    }

    for (const uint8_t responseCode : std::array<uint8_t, 7>{
             PLDM_CCRC_VENDOR_COMP_RESP_CODE_RANGE_MIN,
             PLDM_CCRC_INCOMPLETE_COMP_IMAGE_SET, PLDM_CCRC_COMP_NOT_SUPPORTED,
             PLDM_CCRC_COMP_INFO_NO_MATCH, PLDM_CCRC_COMP_VER_STR_IDENTICAL,
             PLDM_CCRC_COMP_VER_STR_LOWER,
             PLDM_CCRC_VENDOR_COMP_RESP_CODE_RANGE_MAX})
    {
        connection->reset();
        updateManager.createUpdateComponentResponseMessage(7, record, 0,
                                                           responseCode);
        EXPECT_TRUE(connection->loggingCalls.empty());
    }
}

TEST_F(UpdateManagerTest, transferFailuresUseComponentOnlyMessages)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    ASSERT_NE(updateManager.parser, nullptr);
    const auto& record = updateManager.parser->getFwDeviceIDRecords().front();
    const auto& imageInfos = updateManager.parser->getComponentImageInfos();
    const auto imageIndex = std::get<ApplicableComponents>(record).front();
    const auto identifier =
        std::get<static_cast<size_t>(ComponentImageInfoPos::CompIdentifierPos)>(
            imageInfos[imageIndex]);
    componentNameMap = {{7, {{identifier, "ComponentA"}}}};

    auto& connection =
        pldm::utils::RecordingAsioConnectionProvider::getAsioConnection();

    connection->reset();
    updateManager.createFirmwareDownloadTimeoutMessage(7, record, 0, 1024,
                                                       4096);
    EXPECT_EQ(connection->loggingCalls.size(), 1U);
    EXPECT_EQ(connection->loggingCalls.front().messageId,
              "NvidiaUpdate.1.3.FirmwareDownloadTimedOut");
    EXPECT_EQ(
        connection->loggingCalls.front().additionalData["REDFISH_MESSAGE_ARGS"],
        "ComponentA,1024,4096");

    struct TestCase
    {
        uint8_t result;
        std::string messageId;
    };
    const std::array cases{
        TestCase{writeProtectEnabled,
                 "NvidiaUpdate.1.3.FirmwareUpdateBlockedByWriteProtection"},
        TestCase{PLDM_FWUP_TIME_OUT,
                 "NvidiaUpdate.1.3.FirmwareTransferCompletionTimedOut"},
    };
    for (const auto& test : cases)
    {
        SCOPED_TRACE(test.messageId);
        connection->reset();
        updateManager.createMessageRegistry(7, record, 0, transferFailed, "",
                                            PLDM_TRANSFER_COMPLETE,
                                            test.result);
        ASSERT_EQ(connection->loggingCalls.size(), 1U);
        EXPECT_EQ(connection->loggingCalls[0].messageId, test.messageId);
        EXPECT_EQ(
            connection->loggingCalls[0].additionalData["REDFISH_MESSAGE_ARGS"],
            "ComponentA");
    }
}

TEST_F(UpdateManagerTest, verifyFailuresUseComponentPackageMessages)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    ASSERT_NE(updateManager.parser, nullptr);
    const auto& record = updateManager.parser->getFwDeviceIDRecords().front();
    const auto& imageInfos = updateManager.parser->getComponentImageInfos();
    const auto imageIndex = std::get<ApplicableComponents>(record).front();
    const auto& image = imageInfos[imageIndex];
    const auto identifier =
        std::get<static_cast<size_t>(ComponentImageInfoPos::CompIdentifierPos)>(
            image);
    const auto& version =
        std::get<static_cast<size_t>(ComponentImageInfoPos::CompVersionPos)>(
            image);
    componentNameMap = {{7, {{identifier, "ComponentA"}}}};

    struct TestCase
    {
        uint8_t result;
        std::string messageId;
        std::string reason;
    };
    const std::array cases{
        TestCase{PLDM_FWUP_TIME_OUT,
                 "NvidiaUpdate.1.3.FirmwareVerificationCompletionTimedOut", ""},
        TestCase{metadataAuthFailure,
                 "NvidiaUpdate.1.3.ComponentMetadataAuthenticationFailed", ""},
        TestCase{secVersionCheckFailure,
                 "NvidiaUpdate.1.3.ComponentImageSecurityVersionRejected", ""},
        TestCase{secKeysReovked,
                 "NvidiaUpdate.1.3.ComponentImageSigningKeyRevoked", ""},
        TestCase{imageAuthFailure,
                 "NvidiaUpdate.1.3.ComponentImageAuthenticationFailed", ""},
        TestCase{skuMismatch, "NvidiaUpdate.1.3.ComponentImageSkuMismatch", ""},
        TestCase{
            firmwarePackageSizeFailure,
            "NvidiaUpdate.1.3.ComponentImageVerificationFailed",
            "image size check failed (VerifyComplete: OEM firmwarePackageSizeFailure (0x98))"},
        TestCase{
            PLDM_FWUP_VERIFY_ERROR_VERIFICATION_FAILURE,
            "NvidiaUpdate.1.3.ComponentImageVerificationFailed",
            "generic verification failure (VerifyComplete: PLDM_FWUP_VERIFY_ERROR_VERIFICATION_FAILURE (0x01))"},
        TestCase{
            PLDM_FWUP_VERIFY_FAILED_FD_SECURITY_CHECKS,
            "NvidiaUpdate.1.3.ComponentImageVerificationFailed",
            "did not pass the device's security checks (VerifyComplete: PLDM_FWUP_VERIFY_FAILED_FD_SECURITY_CHECKS (0x03))"},
        TestCase{
            PLDM_FWUP_VERIFY_ERROR_IMAGE_INCOMPLETE,
            "NvidiaUpdate.1.3.ComponentImageVerificationFailed",
            "image transfer was incomplete (VerifyComplete: PLDM_FWUP_VERIFY_ERROR_IMAGE_INCOMPLETE (0x04))"},
        TestCase{
            PLDM_FWUP_VERIFY_FAILURE_SECURITY_REVISION_ERROR,
            "NvidiaUpdate.1.3.ComponentImageVerificationFailed",
            "security version does not meet the device's requirement (VerifyComplete: PLDM_FWUP_VERIFY_FAILURE_SECURITY_REVISION_ERROR (0x10))"},
        TestCase{
            PLDM_FWUP_GENERIC_ERROR,
            "NvidiaUpdate.1.3.ComponentImageVerificationFailed",
            "generic verification failure (VerifyComplete: PLDM_FWUP_GENERIC_ERROR (0x0A))"},
    };

    const auto commonArguments =
        "ComponentA," + version + "," + updateManager.parser->pkgVersion;
    auto& connection =
        pldm::utils::RecordingAsioConnectionProvider::getAsioConnection();
    for (const auto& test : cases)
    {
        SCOPED_TRACE(test.messageId + test.reason);
        connection->reset();
        updateManager.createMessageRegistry(7, record, 0, verificationFailed,
                                            "", PLDM_VERIFY_COMPLETE,
                                            test.result);
        ASSERT_EQ(connection->loggingCalls.size(), 1U);
        EXPECT_EQ(connection->loggingCalls[0].messageId, test.messageId);
        const auto expectedArguments =
            test.result == PLDM_FWUP_TIME_OUT
                ? "ComponentA"
                : commonArguments +
                      (test.reason.empty() ? "" : "," + test.reason);
        EXPECT_EQ(
            connection->loggingCalls[0].additionalData["REDFISH_MESSAGE_ARGS"],
            expectedArguments);
    }
}

TEST_F(UpdateManagerTest, resultsWithoutNvidiaMessageKeepLegacyEntries)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    ASSERT_NE(updateManager.parser, nullptr);
    const auto& record = updateManager.parser->getFwDeviceIDRecords().front();

    auto& connection =
        pldm::utils::RecordingAsioConnectionProvider::getAsioConnection();
    connection->reset();
    updateManager.createMessageRegistry(7, record, 0, applyFailed, "",
                                        PLDM_APPLY_COMPLETE,
                                        PLDM_FWUP_APPLY_FAILURE_MEMORY_ISSUE);
    ASSERT_EQ(connection->loggingCalls.size(), 2U);
    EXPECT_EQ(connection->loggingCalls[0].messageId, applyFailed);
    EXPECT_EQ(connection->loggingCalls[1].messageId, resourceErrorDetected);
}

TEST_F(UpdateManagerTest, applyAndGetStatusTimeoutsUseSpecificMessages)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    ASSERT_NE(updateManager.parser, nullptr);
    const auto& record = updateManager.parser->getFwDeviceIDRecords().front();
    const auto& imageInfos = updateManager.parser->getComponentImageInfos();
    const auto imageIndex = std::get<ApplicableComponents>(record).front();
    const auto identifier =
        std::get<static_cast<size_t>(ComponentImageInfoPos::CompIdentifierPos)>(
            imageInfos[imageIndex]);
    componentNameMap = {{7, {{identifier, "ComponentA"}}}};

    struct TestCase
    {
        pldm_firmware_update_commands command;
        uint8_t result;
        std::string messageId;
        std::string arguments;
    };
    const std::array cases{
        TestCase{PLDM_APPLY_COMPLETE, PLDM_FWUP_TIME_OUT,
                 "NvidiaUpdate.1.3.ApplyCompleteTimedOut", "ComponentA"},
        TestCase{PLDM_GET_STATUS, COMMAND_TIMEOUT,
                 "NvidiaUpdate.1.3.PLDMCommandTimedOut",
                 "GetStatus,ComponentA"},
    };

    auto& connection =
        pldm::utils::RecordingAsioConnectionProvider::getAsioConnection();
    for (const auto& test : cases)
    {
        SCOPED_TRACE(test.messageId);
        connection->reset();
        updateManager.createMessageRegistry(7, record, 0, applyFailed, "",
                                            test.command, test.result);
        ASSERT_EQ(connection->loggingCalls.size(), 1U);
        EXPECT_EQ(connection->loggingCalls[0].messageId, test.messageId);
        EXPECT_EQ(
            connection->loggingCalls[0].additionalData["REDFISH_MESSAGE_ARGS"],
            test.arguments);
    }
}
#endif

TEST_F(UpdateManagerTest,
       startNonPLDMUpdateReturnsActivatingWhenAsyncActivationIsUsed)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    std::vector<sdbusplus::object_path> targets;
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
    EXPECT_EQ(state, Server::Activation::Activations::Activating);
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
    std::vector<std::tuple<mctp_eid_t, bool, bool>> refreshCalls{};

    UpdateManager updateManager(
        event, reqHandler, instanceIdDb, localDescriptorMap, componentInfoMap,
        componentNameMap, true,
        [&refreshCalls](mctp_eid_t eid, bool isTarget,
                        bool preUpdateValidation) -> exec::task<int> {
            refreshCalls.emplace_back(eid, isTarget, preUpdateValidation);
            co_return 0;
        });

    EXPECT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    ASSERT_FALSE(refreshCalls.empty());
    EXPECT_TRUE(std::ranges::all_of(refreshCalls, [](const auto& call) {
        return !std::get<2>(call);
    }));
}

TEST_F(UpdateManagerTest, targetedRequestIgnoresPreUpdateValidationOption)
{
    const std::vector<uint8_t> packageUuid{
        0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
        0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6, 0x75};
    DescriptorMap localDescriptorMap{
        {1, {{PLDM_FWUP_UUID, packageUuid}}},
        {2,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
                                0x0F}}}}};
    ComponentNameMap localComponentNames{
        {1, {{100, "TargetComponent"}}},
        {2, {{200, "NonTargetComponent"}}},
    };
    std::vector<std::tuple<mctp_eid_t, bool, bool>> refreshCalls{};

    UpdateManager updateManager(
        event, reqHandler, instanceIdDb, localDescriptorMap, componentInfoMap,
        localComponentNames, true,
        [&refreshCalls](mctp_eid_t eid, bool isTarget,
                        bool preUpdateValidation) -> exec::task<int> {
            refreshCalls.emplace_back(eid, isTarget, preUpdateValidation);
            co_return 0;
        });
    std::vector<sdbusplus::object_path> targets{sdbusplus::object_path(
        "/xyz/openbmc_project/software/TargetComponent")};

    // Feed the package through the updater's mmap exactly as
    // Update::startUpdate does, then hand the mapped pointer straight on.
    ASSERT_TRUE(::mapPackageToUpdater(updateManager, "./test_pkg"));
    const auto* package = updateManager.updater->getImageData();
    const size_t packageSize = updateManager.updater->getImageSize();
    // OnReset: the test verifies the refresh scope only; Immediate would
    // auto-activate the package, which needs production-only state.
    updateManager.setRequestedApplyTime(
        sdbusplus::xyz::openbmc_project::Software::server::ApplyTime::
            RequestedApplyTimes::OnReset);

    // Pre-update validation is whole-system only: a non-empty Targets makes
    // processStreamDefer ignore the option entirely, so no endpoint - not
    // even the requested target - is validated and the gate never runs.
    EXPECT_NO_THROW(updateManager.processPackageDataDefer(
        package, packageSize, false, targets, true));
    EXPECT_FALSE(updateManager.preUpdateValidation);
    // Drop the activation object before pumping the loop: the test verifies
    // the refresh scope only, and a present activation would auto-start the
    // update at the end of processPackageData(), which needs production-only
    // state (debug token, device updaters driving real requests).
    updateManager.activation.reset();
    updateManager.activationProgress.reset();
    EXPECT_GE(sd_event_run(event.get(), 500000), 0);
    EXPECT_GE(sd_event_run(event.get(), 500000), 0);
    EXPECT_THAT(refreshCalls, testing::UnorderedElementsAre(
                                  std::make_tuple(1, true, false),
                                  std::make_tuple(2, false, false)));
}

TEST_F(UpdateManagerTest, wholePreUpdateValidationIgnoredWithoutDeviceScope)
{
    // Develop derives the validation scope from the pre-update refresh set
    // (live descriptorMap union the static-config seed). With neither a
    // discovered endpoint nor a reachable static MCTP config (no mapper in
    // the test environment), the scope is empty: the option is ignored with
    // a journal note and the gate never rejects the request — the
    // develop-branch equivalent of the release "empty fw_update_config.json
    // device scope" ignore.
    DescriptorMap emptyDescriptorMap{};
    UpdateManager updateManager(event, reqHandler, instanceIdDb,
                                emptyDescriptorMap, componentInfoMap,
                                componentNameMap, true, nullptr);
    // processStreamDefer would have recorded the session flag; set it here
    // since the helper drives processPackageData() directly, and verify the
    // empty-scope downgrade clears it.
    updateManager.preUpdateValidation = true;

    EXPECT_EQ(processPackageStream(updateManager, "./test_pkg", {}, true), 0);
    EXPECT_FALSE(updateManager.preUpdateValidation);
    // The gate did not run/reject: the parsed package survives.
    EXPECT_NE(updateManager.parser, nullptr);
}

// EIDs 1 and 2 reach the same device. Runs a whole-system PreUpdateValidation
// update whose refresh of EID 2 drops it from descriptorMap and returns
// refreshRc; returns processPackageStream's result (0: package survived the
// gate, -1: gate rejected the request).
static int runGateWithSecondPathRefresh(
    sdeventplus::Event& event,
    requester::Handler<requester::Request>& reqHandler,
    pldm::InstanceIdDb& instanceIdDb, const ComponentInfoMap& componentInfoMap,
    ComponentNameMap& componentNameMap, int refreshRc)
{
    const std::vector<uint8_t> packageUuid{
        0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
        0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6, 0x75};
    DescriptorMap localDescriptorMap{
        {1, {{PLDM_FWUP_UUID, packageUuid}}},
        {2, {{PLDM_FWUP_UUID, packageUuid}}},
    };
    UpdateManager updateManager(
        event, reqHandler, instanceIdDb, localDescriptorMap, componentInfoMap,
        componentNameMap, true,
        [&localDescriptorMap,
         refreshRc](mctp_eid_t eid, bool /*isTarget*/,
                    bool /*preUpdateValidation*/) -> exec::task<int> {
            if (eid == 2)
            {
                localDescriptorMap.erase(eid);
                co_return refreshRc;
            }
            co_return PLDM_SUCCESS;
        });
    updateManager.objPath = "/xyz/openbmc_project/software/testupdate";
    updateManager.setRequestedApplyTime(
        sdbusplus::xyz::openbmc_project::Software::server::ApplyTime::
            RequestedApplyTimes::OnReset);
    updateManager.preUpdateValidation = true;
    return processPackageStream(updateManager, "./test_pkg", {}, true);
}

TEST_F(UpdateManagerTest,
       wholePreUpdateValidationSkipsDeduplicatedDuplicatePathEndpoint)
{
    // The refresh deduplicates EID 2 (another path is the selected fastest
    // path): it succeeds but drops the EID from descriptorMap. That duplicate
    // is not an unreachable device, so the gate passes.
    EXPECT_EQ(runGateWithSecondPathRefresh(event, reqHandler, instanceIdDb,
                                           componentInfoMap, componentNameMap,
                                           PLDM_SUCCESS),
              0);
}

TEST_F(UpdateManagerTest, wholePreUpdateValidationRejectsFailedRefreshEndpoint)
{
    // Control: EID 2 failing its refresh is unreachable, so the gate rejects
    // the request and drops the package.
    EXPECT_EQ(runGateWithSecondPathRefresh(event, reqHandler, instanceIdDb,
                                           componentInfoMap, componentNameMap,
                                           PLDM_ERROR),
              -1);
}

// Runs a whole-system PreUpdateValidation update with EID 1 discovered and
// healthy (named HGX_GPU_SMA_0) against the given configured device names;
// returns processPackageStream's result (0: package survived the gate, -1:
// gate rejected the request).
static int runGateWithConfiguredTargets(
    sdeventplus::Event& event,
    requester::Handler<requester::Request>& reqHandler,
    pldm::InstanceIdDb& instanceIdDb, const ComponentInfoMap& componentInfoMap,
    ComponentNameMap& componentNameMap,
    const std::set<std::string>& configuredTargets)
{
    const std::vector<uint8_t> packageUuid{
        0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
        0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6, 0x75};
    DescriptorMap localDescriptorMap{{1, {{PLDM_FWUP_UUID, packageUuid}}}};
    UpdateManager updateManager(
        event, reqHandler, instanceIdDb, localDescriptorMap, componentInfoMap,
        componentNameMap, true,
        [](mctp_eid_t, bool, bool) -> exec::task<int> {
            co_return PLDM_SUCCESS;
        },
        emptyExpectedComponentIds, {},
        [](mctp_eid_t eid) {
            return eid == 1 ? std::string("HGX_GPU_SMA_0") : std::string{};
        },
        [configuredTargets]() { return configuredTargets; });
    updateManager.objPath = "/xyz/openbmc_project/software/testupdate";
    updateManager.setRequestedApplyTime(
        sdbusplus::xyz::openbmc_project::Software::server::ApplyTime::
            RequestedApplyTimes::OnReset);
    updateManager.preUpdateValidation = true;
    return processPackageStream(updateManager, "./test_pkg", {}, true);
}

TEST_F(UpdateManagerTest, wholePreUpdateValidationPassesWhenConfiguredAnswer)
{
    // Every configured firmware device answers through a live endpoint.
    EXPECT_EQ(runGateWithConfiguredTargets(event, reqHandler, instanceIdDb,
                                           componentInfoMap, componentNameMap,
                                           {"HGX_GPU_SMA_0"}),
              0);
}

TEST_F(UpdateManagerTest,
       wholePreUpdateValidationRejectsVanishedConfiguredDevice)
{
    // HGX_GPU_0 is configured but has no endpoint at all (it left MCTP in
    // recovery mode, so it is neither discovered nor statically seeded). It
    // must still count as unreachable and reject the request.
    EXPECT_EQ(runGateWithConfiguredTargets(event, reqHandler, instanceIdDb,
                                           componentInfoMap, componentNameMap,
                                           {"HGX_GPU_SMA_0", "HGX_GPU_0"}),
              -1);
}

// EID 14 (HGX_CPU_0) is reachable and refreshed, but its configured_by
// association was not republished after a re-add, so the configured_by name
// lookup does not resolve it. Returns processPackageStream's result (0:
// package survived the gate, -1: gate rejected the request).
static int runGateWithUnresolvedStaticEndpoint(
    sdeventplus::Event& event,
    requester::Handler<requester::Request>& reqHandler,
    pldm::InstanceIdDb& instanceIdDb, const ComponentInfoMap& componentInfoMap,
    ComponentNameMap& componentNameMap, StaticEidTargetsCallback staticTargets)
{
    const std::vector<uint8_t> packageUuid{
        0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
        0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6, 0x75};
    DescriptorMap localDescriptorMap{
        {1, {{PLDM_FWUP_UUID, packageUuid}}},
        {14, {{PLDM_FWUP_UUID, packageUuid}}},
    };
    UpdateManager updateManager(
        event, reqHandler, instanceIdDb, localDescriptorMap, componentInfoMap,
        componentNameMap, true,
        [](mctp_eid_t, bool, bool) -> exec::task<int> {
            co_return PLDM_SUCCESS;
        },
        emptyExpectedComponentIds, {},
        [](mctp_eid_t eid) {
            return eid == 1 ? std::string("HGX_GPU_SMA_0") : std::string{};
        },
        []() { return std::set<std::string>{"HGX_GPU_SMA_0", "HGX_CPU_0"}; },
        std::move(staticTargets));
    updateManager.objPath = "/xyz/openbmc_project/software/testupdate";
    updateManager.setRequestedApplyTime(
        sdbusplus::xyz::openbmc_project::Software::server::ApplyTime::
            RequestedApplyTimes::OnReset);
    updateManager.preUpdateValidation = true;
    return processPackageStream(updateManager, "./test_pkg", {}, true);
}

TEST_F(UpdateManagerTest,
       wholePreUpdateValidationResolvesStaticEndpointWithoutConfiguredBy)
{
    // The static transport configuration names EID 14, so the reachable
    // endpoint answers for HGX_CPU_0 and the gate passes.
    EXPECT_EQ(runGateWithUnresolvedStaticEndpoint(
                  event, reqHandler, instanceIdDb, componentInfoMap,
                  componentNameMap,
                  []() {
                      return std::map<mctp_eid_t, std::set<std::string>>{
                          {14, {"HGX_CPU_0"}}};
                  }),
              0);
}

TEST_F(UpdateManagerTest,
       wholePreUpdateValidationUnresolvedEndpointWithoutStaticNameRejects)
{
    // Without a name source for EID 14, HGX_CPU_0 has no answering endpoint.
    EXPECT_EQ(runGateWithUnresolvedStaticEndpoint(
                  event, reqHandler, instanceIdDb, componentInfoMap,
                  componentNameMap, {}),
              -1);
}

TEST_F(UpdateManagerTest,
       wholePreUpdateValidationUsesProvidedPlatformEidFallbackScope)
{
    const std::vector<uint8_t> packageUuid{
        0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
        0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6, 0x75};
    DescriptorMap localDescriptorMap{{7, {{PLDM_FWUP_UUID, packageUuid}}}};
    UpdateManager updateManager(event, reqHandler, instanceIdDb,
                                localDescriptorMap, componentInfoMap,
                                componentNameMap, true, nullptr);
    // The gate dereferences the parser (guaranteed by processStream), so
    // parse a real package before overriding the records under test.
    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    ASSERT_NE(updateManager.parser, nullptr);
    updateManager.fwDeviceIDRecords = {{1, {0}, "Version", Descriptors{}, {}}};
    // The gate's reject path publishes the final activation status, which
    // registers an Activation object at objPath; give it a valid path since
    // this test calls the gate without going through processPackageDataDefer().
    updateManager.objPath = "/xyz/openbmc_project/software/testupdate";
    const std::vector<mctp_eid_t> fallbackConfigEids{7};

    EXPECT_TRUE(updateManager.runPreUpdateValidationGate(
        fallbackConfigEids, DeviceUpdaterInfos{{7, 0}}));

    std::get<ApplicableComponents>(updateManager.fwDeviceIDRecords.front())
        .clear();
    EXPECT_FALSE(updateManager.runPreUpdateValidationGate(
        fallbackConfigEids, DeviceUpdaterInfos{{7, 0}}));

    EXPECT_FALSE(
        updateManager.runPreUpdateValidationGate(fallbackConfigEids, {}));
}

TEST_F(UpdateManagerTest,
       wholePreUpdateValidationCoverageIsCheckedForEveryDeviceOnSharedEid)
{
    const std::vector<uint8_t> packageUuid{
        0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
        0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6, 0x75};
    DescriptorMap localDescriptorMap{
        {9, {{PLDM_FWUP_UUID, packageUuid}}},
    };
    ComponentNameMap emptyComponentNames{};
    // The Manager owns the expected-component map in production and the
    // UpdateManager holds a reference to it; mirror that here so the
    // expectation can be varied between gate runs.
    ExpectedComponentIdsByEid expectation{};
    UpdateManager updateManager(
        event, reqHandler, instanceIdDb, localDescriptorMap, componentInfoMap,
        emptyComponentNames, true, nullptr, expectation);
    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    ASSERT_NE(updateManager.parser, nullptr);
    ASSERT_EQ(updateManager.fwDeviceIDRecords.size(), 1U);
    // The failing-coverage call below rejects the request and publishes the
    // final activation status; that needs a valid object path (normally set
    // by processStreamDefer, which this test bypasses).
    updateManager.objPath = "/xyz/openbmc_project/software/testupdate";

    const auto& applicableComponents =
        std::get<ApplicableComponents>(updateManager.fwDeviceIDRecords.front());
    ASSERT_FALSE(applicableComponents.empty());
    const auto& componentImageInfos =
        updateManager.parser->getComponentImageInfos();
    std::set<CompIdentifier> packagedIds{};
    for (const auto componentIndex : applicableComponents)
    {
        packagedIds.insert(std::get<static_cast<size_t>(
                               ComponentImageInfoPos::CompIdentifierPos)>(
            componentImageInfos[componentIndex]));
    }
    const auto coveredId = *packagedIds.begin();
    CompIdentifier uncoveredId = 0;
    while (packagedIds.contains(uncoveredId))
    {
        ASSERT_NE(uncoveredId, std::numeric_limits<CompIdentifier>::max());
        ++uncoveredId;
    }

    // Control: every configured component covered by the package.
    expectation = {{9, {{"CoveredComponent", {coveredId}}}}};
    EXPECT_TRUE(updateManager.runPreUpdateValidationGate(
        {9}, DeviceUpdaterInfos{{9, 0}}));

    // A second configured component on the same EID with no image in the
    // package must fail coverage even though the EID itself is covered.
    expectation = {{9,
                    {{"CoveredComponent", {coveredId}},
                     {"UncoveredComponent", {uncoveredId}}}}};
    EXPECT_FALSE(updateManager.runPreUpdateValidationGate(
        {9}, DeviceUpdaterInfos{{9, 0}}));
}

TEST_F(UpdateManagerTest, getComponentTargetListMergesComponentsForSameEid)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    ComponentNameMap names{
        {1, {{10, "CompA"}, {11, "CompB"}}},
    };
    std::vector<sdbusplus::object_path> targets{
        sdbusplus::object_path("/xyz/openbmc_project/software/CompA"),
        sdbusplus::object_path("/xyz/openbmc_project/software/CompB")};

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

TEST_F(UpdateManagerTest, getActivationMethod_DcPowerCycle)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    bitfield16_t dcOnly{};
    dcOnly.value = 0x10;

    EXPECT_EQ(updateManager.getActivationMethod(dcOnly), "DC power cycle");
}

TEST_F(UpdateManagerTest, getComponentTargetListSkipsNonSoftwareTargetPaths)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    ComponentNameMap names{
        {1, {{10, "CompA"}}},
    };
    std::vector<sdbusplus::object_path> targets{
        sdbusplus::object_path("/xyz/openbmc_project/software/CompA"),
        sdbusplus::object_path(
            "/xyz/openbmc_project/inventory/system/chassis/chassis0")};

    auto compTargetList = updateManager.getComponentTargetList(names, targets);

    ASSERT_TRUE(compTargetList.contains(1));
    ASSERT_EQ(compTargetList.at(1).size(), 1u);
    EXPECT_EQ(compTargetList.at(1).front(), 10);
}

TEST_F(UpdateManagerTest, associatePkgToDevicesWithoutTargetFiltering)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    std::vector<uint8_t> uuid{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                              0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6, 0x75};
    Descriptors descriptors{{PLDM_FWUP_UUID, uuid}};
    DescriptorMap matchingDescriptorMap{{9, descriptors}};
    FirmwareDeviceIDRecords inRecords{
        {DeviceUpdateOptionFlags{},
         ApplicableComponents{0, 1},
         "VersionString",
         descriptors,
         {}}};
    ComponentImageInfos compImageInfos{{10, 100, 0xFFFFFFFF, 0, 0, 0, 4, "V1"},
                                       {10, 101, 0xFFFFFFFF, 0, 0, 4, 4, "V2"}};
    FirmwareDeviceIDRecords outRecords;
    TotalComponentUpdates totalNumComponentUpdates = 0;

    auto deviceUpdaterInfos = updateManager.associatePkgToDevices(
        inRecords, matchingDescriptorMap, compImageInfos, {}, {}, outRecords,
        totalNumComponentUpdates);

    ASSERT_EQ(deviceUpdaterInfos.size(), 1u);
    ASSERT_EQ(outRecords.size(), 1u);
    EXPECT_EQ(totalNumComponentUpdates, 2u);
    EXPECT_EQ(std::get<ApplicableComponents>(outRecords.front()).size(), 2u);
}

TEST_F(UpdateManagerTest, associatePkgToDevicesFiltersRequestedComponents)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    std::vector<uint8_t> uuid{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                              0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6, 0x75};
    Descriptors descriptors{{PLDM_FWUP_UUID, uuid}};
    DescriptorMap matchingDescriptorMap{{9, descriptors}};
    FirmwareDeviceIDRecords inRecords{
        {DeviceUpdateOptionFlags{},
         ApplicableComponents{0, 1},
         "VersionString",
         descriptors,
         {}}};
    ComponentImageInfos compImageInfos{{10, 100, 0xFFFFFFFF, 0, 0, 0, 4, "V1"},
                                       {10, 101, 0xFFFFFFFF, 0, 0, 4, 4, "V2"}};
    ComponentTargetList compTargetList{{9, {101}}};
    std::vector<sdbusplus::object_path> objectPaths{
        sdbusplus::object_path("/xyz/openbmc_project/software/CompB")};
    FirmwareDeviceIDRecords outRecords;
    TotalComponentUpdates totalNumComponentUpdates = 0;

    auto deviceUpdaterInfos = updateManager.associatePkgToDevices(
        inRecords, matchingDescriptorMap, compImageInfos, compTargetList,
        objectPaths, outRecords, totalNumComponentUpdates);

    ASSERT_EQ(deviceUpdaterInfos.size(), 1u);
    ASSERT_EQ(outRecords.size(), 1u);
    EXPECT_EQ(totalNumComponentUpdates, 1u);
    ASSERT_EQ(std::get<ApplicableComponents>(outRecords.front()).size(), 1u);
    EXPECT_EQ(std::get<ApplicableComponents>(outRecords.front()).front(), 1u);
}

TEST_F(UpdateManagerTest, associatePkgToDevicesSkipsEmptyFilteredComponentLists)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    std::vector<uint8_t> uuid{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                              0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6, 0x75};
    Descriptors descriptors{{PLDM_FWUP_UUID, uuid}};
    DescriptorMap matchingDescriptorMap{{9, descriptors}};
    FirmwareDeviceIDRecords inRecords{
        {DeviceUpdateOptionFlags{},
         ApplicableComponents{0, 1},
         "VersionString",
         descriptors,
         {}}};
    ComponentImageInfos compImageInfos{{10, 100, 0xFFFFFFFF, 0, 0, 0, 4, "V1"},
                                       {10, 101, 0xFFFFFFFF, 0, 0, 4, 4, "V2"}};
    ComponentTargetList compTargetList{{9, {999}}};
    std::vector<sdbusplus::object_path> objectPaths{sdbusplus::object_path(
        "/xyz/openbmc_project/software/NoMatchingComponent")};
    FirmwareDeviceIDRecords outRecords;
    TotalComponentUpdates totalNumComponentUpdates = 0;

    auto deviceUpdaterInfos = updateManager.associatePkgToDevices(
        inRecords, matchingDescriptorMap, compImageInfos, compTargetList,
        objectPaths, outRecords, totalNumComponentUpdates);

    EXPECT_TRUE(deviceUpdaterInfos.empty());
    EXPECT_TRUE(outRecords.empty());
    EXPECT_EQ(totalNumComponentUpdates, 0u);
}

TEST_F(UpdateManagerTest, recordUnavailableTargetEidsTracksRemovedTargets)
{
    std::vector<uint8_t> uuid{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                              0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6, 0x75};
    Descriptors descriptors{{PLDM_FWUP_UUID, uuid}};
    DescriptorMap liveDescriptorMap{{9, descriptors}};

    UpdateManager updateManager(event, reqHandler, instanceIdDb,
                                liveDescriptorMap, componentInfoMap,
                                componentNameMap, true, nullptr);

    ComponentTargetList compTargetList{{9, {100}}, {10, {100}}};

    updateManager.recordUnavailableTargetEids(compTargetList);

    EXPECT_FALSE(updateManager.unavailableTargetEids.contains(9));
    EXPECT_TRUE(updateManager.unavailableTargetEids.contains(10));
}

TEST_F(UpdateManagerTest, updateDeviceCompletionAppendsToExistingComponentList)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.listCompNames = "Existing";

    updateManager.updateDeviceCompletion(9, true, {"CompA", "CompB"});

    EXPECT_EQ(updateManager.listCompNames, "Existing CompA CompB");
}

TEST_F(UpdateManagerTest, startNonPLDMUpdateNoDevicesUsesExistingProgressObject)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);

    std::vector<sdbusplus::object_path> targets;
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

TEST_F(UpdateManagerTest,
       startNonPLDMUpdateReturnsActivatingWhenTrackedOtherDevicesExist)
{
    MockdBusHandler dbusHandler;
    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(std::vector<std::string>{}));
    // TODO(async-writes): the RequestedActivation property write triggered
    // by startNonPLDMUpdate -> OtherDeviceUpdateManager::activate() now
    // goes through setDBusPropertyAsync (free function) and is not
    // observable through MockdBusHandler::setDbusProperty. The assertion
    // on `state == Activating` below still validates the public contract.

    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath = "/xyz/openbmc_project/software/non_pldm_activating";

    std::vector<sdbusplus::object_path> targets;
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                   targets, dbusHandler);

    auto tracked = std::make_unique<OtherDeviceUpdateActivation>();
    tracked->uuid = "UUID_NON_PLDM_ACTIVATING";
    tracked->activationState = Server::Activation::Activations::Ready;
    tracked->requestedActivation =
        Server::Activation::RequestedActivations::None;
    updateManager.otherDeviceUpdateManager->otherDevices["/xyz/openbmc_project/"
                                                         "software/other/"
                                                         "tracked"] =
        std::move(tracked);
    updateManager.otherDeviceUpdateManager
        ->uuidMappings["UUID_NON_PLDM_ACTIVATING"] = {"9.9", "OtherComp"};
    updateManager.otherDeviceUpdateManager
        ->isImageFileProcessed["UUID_NON_PLDM_ACTIVATING"] = false;

    auto state = updateManager.startNonPLDMUpdate();

    EXPECT_EQ(state, Server::Activation::Activations::Activating);
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

TEST_F(UpdateManagerTest,
       computeEffectiveTimeoutSec_returnsFirmwareUpdateTimeWhenNoOtherMgr)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    // No otherDeviceUpdateManager constructed -> falls back to the
    // FIRMWARE_UPDATE_TIME default (in seconds).
    const uint64_t result = updateManager.computeEffectiveTimeoutSec();
    EXPECT_GE(result, 60u);
}

TEST_F(UpdateManagerTest,
       computeEffectiveTimeoutSec_emptyOtherDevices_matchesNoMgrCase)
{
    // A single UpdateManager is reused across the two configurations because
    // its constructor registers /xyz/openbmc_project/software/pldm on the
    // global D-Bus; constructing two simultaneously would collide.
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    const uint64_t withoutMgr = updateManager.computeEffectiveTimeoutSec();

    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(
            busMock, &updateManager, std::vector<sdbusplus::object_path>{});
    const uint64_t withMgr = updateManager.computeEffectiveTimeoutSec();

    // Empty otherDevices -> max == 0 -> result is the default in both
    // cases. The two paths must agree.
    EXPECT_EQ(withMgr, withoutMgr);
    EXPECT_GT(withMgr, 0u);
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
            busMock, &updateManager, std::vector<sdbusplus::object_path>{});
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

TEST_F(UpdateManagerTest, processStreamDeferReusesExistingActivationObjects)
{
    // package must be declared before updateManager, so it is destroyed
    // after: processPackageDataDefer() captures package.data() in a
    // deferred callback owned by updateManager, so the buffer must outlive
    // updateManager's Defer handler even though this test never pumps the
    // event loop to actually invoke it.
    std::ifstream packageFile("./test_pkg", std::ios::binary | std::ios::ate);
    ASSERT_TRUE(packageFile.good());
    auto packageSize = static_cast<size_t>(packageFile.tellg());
    packageFile.seekg(0, std::ios::beg);
    std::vector<uint8_t> package(packageSize);
    packageFile.read(reinterpret_cast<char*>(package.data()),
                     static_cast<std::streamsize>(packageSize));

    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath =
        "/xyz/openbmc_project/software/existing_activation_object";
    updateManager.activation = std::make_unique<Activation>(
        busMock, updateManager.objPath,
        software::Activation::Activations::Ready, &updateManager);
    updateManager.activationProgress =
        std::make_unique<ActivationProgress>(busMock, updateManager.objPath);

    auto* activation = updateManager.activation.get();
    auto* activationProgress = updateManager.activationProgress.get();
    updateManager.setRequestedApplyTime(
        sdbusplus::xyz::openbmc_project::Software::server::ApplyTime::
            RequestedApplyTimes::Immediate);

    auto returnedPath = updateManager.processPackageDataDefer(
        package.data(), packageSize, false, {});

    EXPECT_EQ(returnedPath, updateManager.objPath);
    EXPECT_EQ(updateManager.activation.get(), activation);
    EXPECT_EQ(updateManager.activationProgress.get(), activationProgress);
}

TEST_F(UpdateManagerTest,
       createProgressUpdateTimerLeavesTimerRunningBeforeBoundary)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath =
        "/xyz/openbmc_project/software/progress_before_timeout";
    updateManager.activationProgress =
        std::make_unique<ActivationProgress>(busMock, updateManager.objPath);
    updateManager.totalInterval = 4;
    updateManager.createProgressUpdateTimer();
    ASSERT_NE(updateManager.progressTimer, nullptr);

    updateManager.progressTimer->start(std::chrono::seconds(0), true);
    EXPECT_GE(sd_event_run(event.get(), 500000), 0);

    EXPECT_EQ(updateManager.updateInterval, 1U);
    EXPECT_NE(updateManager.updateInterval, updateManager.totalInterval - 1);
    EXPECT_NE(updateManager.progressTimer, nullptr);
    updateManager.progressTimer->stop();
}

TEST_F(UpdateManagerTest,
       activatePackageReturnsActivatingWhenNonPLDMUpdateContinues)
{
    MockdBusHandler dbusHandler;
    EXPECT_CALL(dbusHandler,
                getSubTreePaths(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(std::vector<std::string>{}));
    // TODO(async-writes): same as
    // startNonPLDMUpdateReturnsActivatingWhenTrackedOtherDevicesExist —
    // the RequestedActivation write inside activatePackage's non-PLDM
    // branch is now dispatched via setDBusPropertyAsync.

    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    updateManager.objPath =
        "/xyz/openbmc_project/software/activate_non_pldm_in_progress";

    std::vector<sdbusplus::object_path> targets;
    updateManager.otherDeviceUpdateManager =
        std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                   targets, dbusHandler);

    auto tracked = std::make_unique<OtherDeviceUpdateActivation>();
    tracked->uuid = "UUID_NON_PLDM_ACTIVATE";
    tracked->activationState = Server::Activation::Activations::Ready;
    tracked->requestedActivation =
        Server::Activation::RequestedActivations::None;
    updateManager.otherDeviceUpdateManager->otherDevices["/xyz/openbmc_project/"
                                                         "software/other/"
                                                         "activate"] =
        std::move(tracked);
    updateManager.otherDeviceUpdateManager
        ->uuidMappings["UUID_NON_PLDM_ACTIVATE"] = {"1.0", "CompA"};
    updateManager.otherDeviceUpdateManager
        ->isImageFileProcessed["UUID_NON_PLDM_ACTIVATE"] = false;

    auto state = updateManager.activatePackage();

    EXPECT_EQ(state, Server::Activation::Activations::Activating);
    EXPECT_NE(updateManager.progressTimer, nullptr);
    EXPECT_NE(updateManager.activationBlocksTransition, nullptr);
    updateManager.progressTimer->stop();
}

TEST_F(UpdateManagerTest,
       logUnupdatedTargetsKeepsDirectTargetBehindProxyReported)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    descriptorMap[61] = {};
    updateManager.requestedTargets = {{"CX_0", 61}};
    updateManager.requestedDownstreamTargets = {{"XCVR_0", {61, 0}},
                                                {"XCVR_1", {62, 1}}};
    updateManager.downstreamJobByRecordOffset[7] =
        DownstreamUpdateJob{0, DownstreamUpdateAddressMode::SingleDevice, {0}};

    updateManager.logUnupdatedTargets({{61, 7}});

    EXPECT_TRUE(updateManager.requestedTargets.empty());
    EXPECT_TRUE(updateManager.requestedDownstreamTargets.empty());

    updateManager.requestedTargets = {{"CX_0", 61}};
    updateManager.requestedDownstreamTargets = {{"XCVR_0", {61, 0}}};
    updateManager.logUnupdatedTargets({{61, 8}});
    EXPECT_TRUE(updateManager.requestedTargets.empty());
    EXPECT_TRUE(updateManager.requestedDownstreamTargets.empty());
}

namespace
{

/** @brief Smallest UpdateManagerBase, to reach the default message hooks
 *         that updater test doubles inherit without overriding. */
class BaseOnlyUpdateManager : public UpdateManagerBase
{
  public:
    using UpdateManagerBase::UpdateManagerBase;

    void updateDeviceCompletion(mctp_eid_t, bool,
                                std::vector<std::string>) override
    {}
    void markComponentUpdateCompleted() override {}
    software::Activation::Activations activatePackage() override
    {
        return software::Activation::Activations::Active;
    }
    void resetActivationState() override {}
};

} // namespace

#ifdef OEM_NVIDIA
TEST_F(UpdateManagerTest, componentMessagesSkipUnreportableComponents)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    auto& connection =
        pldm::utils::RecordingAsioConnectionProvider::getAsioConnection();

    // Without a parsed package there is no component to describe.
    FirmwareDeviceIDRecord emptyRecord{};
    connection->reset();
    updateManager.createUpdateComponentResponseMessage(
        1, emptyRecord, 0, PLDM_CCRC_COMP_COMPARISON_STAMP_IDENTICAL);
    updateManager.createNvidiaCommandResponseMessage(
        1, emptyRecord, 0, PLDM_GET_STATUS, COMMAND_TIMEOUT);
    updateManager.createFirmwareDownloadTimeoutMessage(1, emptyRecord, 0, 0, 0);
    EXPECT_TRUE(connection->loggingCalls.empty());

    // A component index past the record's applicable components is dropped.
    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    const auto& record = updateManager.parser->getFwDeviceIDRecords().front();
    const size_t badIndex = std::get<ApplicableComponents>(record).size();
    connection->reset();
    updateManager.createUpdateComponentResponseMessage(
        1, record, badIndex, PLDM_CCRC_COMP_COMPARISON_STAMP_IDENTICAL);
    updateManager.createNvidiaCommandResponseMessage(
        1, record, badIndex, PLDM_GET_STATUS, COMMAND_TIMEOUT);
    updateManager.createFirmwareDownloadTimeoutMessage(1, record, badIndex, 0,
                                                       0);
    EXPECT_TRUE(connection->loggingCalls.empty());

    // The UpdateManagerBase defaults used by test doubles stay silent.
    BaseOnlyUpdateManager baseOnlyManager(event, reqHandler, instanceIdDb);
    UpdateManagerBase& base = baseOnlyManager;
    EXPECT_NO_THROW({
        base.createUpdateComponentResponseMessage(
            1, record, 0, PLDM_CCRC_COMP_COMPARISON_STAMP_IDENTICAL);
        base.createNvidiaCommandResponseMessage(1, record, 0, PLDM_GET_STATUS,
                                                COMMAND_TIMEOUT);
        base.createFirmwareDownloadTimeoutMessage(1, record, 0, 0, 0);
    });
    EXPECT_TRUE(connection->loggingCalls.empty());
}
#endif

TEST_F(UpdateManagerTest, packageIntegrityCheckAsyncReportsEmptyPublicKey)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    processPackageStream(updateManager, "./test_pkg_v3_signed");
    ASSERT_NE(updateManager.parser, nullptr);
    const auto calcPkgSize = updateManager.parser->calculatePackageSize();

    // In test_pkg_v3_signed the public-key size sits 16 bytes into the
    // signature header and the signature size follows the key. Zero the key
    // size and move the (still valid) signature size up behind it so the
    // header parses with an empty public key.
    constexpr std::streamoff publicKeySizeOffset = 16;
    constexpr std::array<char, 4> emptyKeyThenSignatureSize{
        0x00, 0x00, 0x00, 0x68};

    auto brokenPath = std::filesystem::temp_directory_path() /
                      "pldm_pkg_v3_empty_public_key.bin";
    std::filesystem::copy_file(
        "./test_pkg_v3_signed", brokenPath,
        std::filesystem::copy_options::overwrite_existing);
    {
        std::fstream io(brokenPath,
                        std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(io.good());
        io.seekp(static_cast<std::streamoff>(calcPkgSize) +
                 publicKeySizeOffset);
        io.write(emptyKeyThenSignatureSize.data(),
                 emptyKeyThenSignatureSize.size());
    }
    mapPackageToUpdater(updateManager, brokenPath.string());

    auto& connection =
        pldm::utils::RecordingAsioConnectionProvider::getAsioConnection();
    connection->reset();
    bool onCompleteCalled = false;
    bool onErrorCalled = false;
    std::string errorMessage;
    updateManager.packageIntegrityCheckAsync(
        [&](bool) { onCompleteCalled = true; },
        [&](const std::string& message) {
            onErrorCalled = true;
            errorMessage = message;
        });

    std::filesystem::remove(brokenPath);
    EXPECT_FALSE(onCompleteCalled);
    ASSERT_TRUE(onErrorCalled);
    EXPECT_NE(errorMessage.find("Public key data is empty"), std::string::npos);
#ifdef OEM_NVIDIA
    ASSERT_EQ(connection->loggingCalls.size(), 1U);
    EXPECT_EQ(connection->loggingCalls[0].messageId,
              "NvidiaUpdate.1.3.PackageSignatureIntegrityFailed");
    EXPECT_EQ(
        connection->loggingCalls[0].additionalData["REDFISH_MESSAGE_ARGS"],
        updateManager.parser->pkgVersion);
#endif
}

TEST_F(UpdateManagerTest, updatePackageCompletionReportsNoMatchingDevices)
{
    UpdateManager updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                                componentInfoMap, componentNameMap, true,
                                nullptr);
    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    mapPackageToUpdater(updateManager, "./test_pkg");
    ASSERT_NE(updateManager.otherDeviceUpdateManager, nullptr);

    updateManager.objPath = "/xyz/openbmc_project/software/pkg_no_match";
    updateManager.activation = std::make_unique<Activation>(
        busMock, updateManager.objPath,
        software::Activation::Activations::Ready, &updateManager);
    updateManager.activationBlocksTransition = nullptr;

    // A targeted request that no PLDM device and no Item Updater accepted.
    auto& otherManager = *updateManager.otherDeviceUpdateManager;
    otherManager.targets.emplace_back(
        "/xyz/openbmc_project/software/unmatched_target");
    otherManager.isImageFileProcessed.emplace("UUID_UNMATCHED", true);
    otherManager.updateStarted = false;

    auto& connection =
        pldm::utils::RecordingAsioConnectionProvider::getAsioConnection();
    connection->reset();
    EXPECT_NO_THROW({ updateManager.updatePackageCompletion(); });
    EXPECT_EQ(updateManager.activation->activation(),
              software::Activation::Activations::Failed);
#if defined(OEM_NVIDIA) && defined(NON_PLDM)
    const bool reported = std::any_of(
        connection->loggingCalls.begin(), connection->loggingCalls.end(),
        [](const auto& call) {
            return call.messageId ==
                   "NvidiaUpdate.1.3.FirmwarePackageNoMatchingDevices";
        });
    EXPECT_TRUE(reported);
#endif
}
