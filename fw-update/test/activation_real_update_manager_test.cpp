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
#include "common/utils.hpp"
#include "fw-update/oem-nvidia/debug_token.hpp"
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

#include <sdbusplus/test/sdbus_mock.hpp>

#include <filesystem>

#include <gtest/gtest.h>

using namespace pldm;
using namespace pldm::fw_update;
using namespace std::chrono;

static int processPackageStream(UpdateManager& updateManager,
                                const std::filesystem::path& packagePath)
{
    if (!updateManager.updater)
    {
        return -1;
    }

    updateManager.updater->clearImageStream();
    int imageFd = open(packagePath.c_str(), O_RDONLY);
    if (imageFd < 0)
    {
        return -1;
    }

    if (!updateManager.updater->mmapFile.map(imageFd, true))
    {
        return -1;
    }

    updateManager.updater->mmapStream = std::make_unique<pldm::MmapStream>(
        updateManager.updater->mmapFile.data(),
        updateManager.updater->mmapFile.size());
    if (!updateManager.updater->mmapStream->good())
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

class ActivationRealUpdateManagerTest : public testing::Test
{
  protected:
    ActivationRealUpdateManagerTest() :
        busMock(sdbusplus::get_mocked_new(&sdbusMock)),
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                      componentInfoMap, componentNameMap, true, nullptr)
    {}

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

TEST_F(ActivationRealUpdateManagerTest, ActivationCtorWithRealUpdateManager)
{
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};

    Activation activation(busMock, objPath, ActivationIntf::Activations::Ready,
                          &updateManager);
    EXPECT_EQ(activation.activation(), ActivationIntf::Activations::Ready);
}

TEST_F(ActivationRealUpdateManagerTest, DeleteWithRealUpdateManager)
{
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};

    Delete deleteImpl(busMock, objPath, &updateManager);
    EXPECT_NO_THROW({ deleteImpl.delete_(); });
}

TEST_F(ActivationRealUpdateManagerTest,
       RequestedActivationFromInvalidStateTransitionsToFailed)
{
    const std::string objPath{"/xyz/openbmc_project/software/test_request"};
    Activation activation(busMock, objPath,
                          ActivationIntf::Activations::Invalid, &updateManager);
    auto reqState = activation.requestedActivation(
        ActivationIntf::RequestedActivations::Active);
    EXPECT_EQ(reqState, ActivationIntf::RequestedActivations::Active);
    EXPECT_EQ(activation.activation(), ActivationIntf::Activations::Failed);
}

TEST_F(ActivationRealUpdateManagerTest,
       ActivationActivatingPathWithPreparedManagerDoesNotCrash)
{
    const std::string objPath{
        "/xyz/openbmc_project/software/test_activation_real_activating"};

    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    updateManager.objPath = objPath;

    if (!updateManager.otherDeviceUpdateManager)
    {
        std::vector<sdbusplus::message::object_path> targets;
        updateManager.otherDeviceUpdateManager =
            std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                       targets);
    }

#ifdef OEM_NVIDIA
    if (!updateManager.debugToken)
    {
        updateManager.debugToken =
            std::make_unique<DebugToken>(busMock, &updateManager);
    }
#endif

    Activation activation(busMock, objPath, ActivationIntf::Activations::Ready,
                          &updateManager);
    EXPECT_NO_THROW({
        auto state =
            activation.activation(ActivationIntf::Activations::Activating);
        EXPECT_EQ(state, ActivationIntf::Activations::Activating);
    });
}

TEST_F(ActivationRealUpdateManagerTest,
       ActivationActivatingWithIncorrectSignatureTransitionsToFailed)
{
    const std::string objPath{
        "/xyz/openbmc_project/software/test_activation_real_bad_signature"};

    ASSERT_EQ(
        processPackageStream(updateManager, "./test_pkg_v3_incorrectly_signed"),
        0);
    ASSERT_TRUE(
        mapPackageToUpdater(updateManager, "./test_pkg_v3_incorrectly_signed"));
    updateManager.objPath = objPath;

    if (!updateManager.otherDeviceUpdateManager)
    {
        std::vector<sdbusplus::message::object_path> targets;
        updateManager.otherDeviceUpdateManager =
            std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                       targets);
    }

#ifdef OEM_NVIDIA
    if (!updateManager.debugToken)
    {
        updateManager.debugToken =
            std::make_unique<DebugToken>(busMock, &updateManager);
    }
#endif

    Activation activation(busMock, objPath, ActivationIntf::Activations::Ready,
                          &updateManager);
    auto state = activation.activation(ActivationIntf::Activations::Activating);
    EXPECT_EQ(state, ActivationIntf::Activations::Activating);

    waitEventExpiry(milliseconds(1200));
    EXPECT_EQ(activation.activation(), ActivationIntf::Activations::Failed);
}

TEST_F(ActivationRealUpdateManagerTest,
       RequestedActivationFromReadyStateLeavesReadyState)
{
    const std::string objPath{
        "/xyz/openbmc_project/software/test_request_ready_real_manager"};

    ASSERT_EQ(processPackageStream(updateManager, "./test_pkg"), 0);
    updateManager.objPath = objPath;

    if (!updateManager.otherDeviceUpdateManager)
    {
        std::vector<sdbusplus::message::object_path> targets;
        updateManager.otherDeviceUpdateManager =
            std::make_unique<OtherDeviceUpdateManager>(busMock, &updateManager,
                                                       targets);
    }

#ifdef OEM_NVIDIA
    if (!updateManager.debugToken)
    {
        updateManager.debugToken =
            std::make_unique<DebugToken>(busMock, &updateManager);
    }
#endif

    Activation activation(busMock, objPath, ActivationIntf::Activations::Ready,
                          &updateManager);

    auto requested = activation.requestedActivation(
        ActivationIntf::RequestedActivations::Active);

    EXPECT_EQ(requested, ActivationIntf::RequestedActivations::Active);
    EXPECT_NE(activation.activation(), ActivationIntf::Activations::Ready);
    EXPECT_NE(activation.activation(), ActivationIntf::Activations::Invalid);
}
