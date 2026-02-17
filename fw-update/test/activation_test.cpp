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
#include "fake_update_manager.hpp"
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "fw-update/activation.hpp"
#undef private

#include <sdbusplus/bus.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldm;
using namespace pldm::fw_update;
using namespace std::chrono;

using ::testing::_;
using ::testing::HasSubstr;
using ::testing::IsNull;
using ::testing::Return;
using ::testing::StrEq;
using ::testing::Throw;

class ActivationTest : public testing::Test
{
  protected:
    ActivationTest() : updateManager() {}

    ~ActivationTest() override = default;

    void SetUp() override
    {
        testing::securityChecksStatus = true;
        testing::updateManagerActivatePackageResult =
            software::Activation::Activations::Active;
        testing::resultPerformSecurityChecksOnComplete =
            software::Activation::Activations::NotReady;
        testing::resetTestState();
    }

    UpdateManager updateManager;
};

TEST(Entry, Basic)
{
    int expectedProgress(0);
    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};

    ActivationProgress activationProgress(busMock, objPath);

    EXPECT_EQ(activationProgress.progress(), expectedProgress);
}

TEST_F(ActivationTest, DISABLED_Delete)
{
    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};

    Delete _delete(busMock, objPath, &updateManager);

    EXPECT_NO_THROW({ _delete.delete_(); });
}

TEST_F(ActivationTest, Activation_status_active)
{
    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};

    Server::Activation::Activations activationState =
        Server::Activation::Activations::Active;

    const Server::Activation::Activations stateActive =
        Server::Activation::Activations::Active;

    Activation _activation(busMock, objPath, stateActive, &updateManager);

    Server::Activation::Activations resultState =
        _activation.activation(activationState);

    EXPECT_EQ(resultState, stateActive);
}

TEST_F(ActivationTest, Activation_status_active_recreatesDeleteWhenMissing)
{
    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};

    Activation activation(busMock, objPath,
                          Server::Activation::Activations::Ready,
                          &updateManager);
    activation.deleteImpl.reset();
    ASSERT_EQ(activation.deleteImpl, nullptr);

    auto result =
        activation.activation(Server::Activation::Activations::Active);
    EXPECT_EQ(result, Server::Activation::Activations::Active);
    EXPECT_NE(activation.deleteImpl, nullptr);
}

TEST_F(ActivationTest, DISABLED_DeleteInvokesUpdateManagerClearActivationInfo)
{
    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};

    Delete deleteObj(busMock, objPath, &updateManager);
    deleteObj.delete_();

    EXPECT_TRUE(testing::clearActivationInfoCalled);
    EXPECT_EQ(testing::clearActivationInfoCallCount, 1U);
}

TEST_F(ActivationTest, DISABLED_ActivationActivatingWhenSecurityChecksFail)
{
    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const std::string objPath{
        "/xyz/openbmc_project/software/security_checks_fail"};

    Activation activation(busMock, objPath,
                          Server::Activation::Activations::Ready,
                          &updateManager);
    [[maybe_unused]] auto leakedDelete = activation.deleteImpl.release();

    testing::securityChecksStatus = false;
    auto result =
        activation.activation(Server::Activation::Activations::Activating);

    EXPECT_EQ(result, Server::Activation::Activations::Activating);
    EXPECT_EQ(testing::resultPerformSecurityChecksOnComplete,
              Server::Activation::Activations::Failed);
    EXPECT_TRUE(testing::resetActivationBlocksTransitionCalled);
    EXPECT_TRUE(testing::clearFirmwareUpdatePackageCalled);
}

TEST_F(ActivationTest, DISABLED_ActivationActivatingWhenActivatePackageFails)
{
    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const std::string objPath{
        "/xyz/openbmc_project/software/activate_package_fail"};

    Activation activation(busMock, objPath,
                          Server::Activation::Activations::Ready,
                          &updateManager);
    [[maybe_unused]] auto leakedDelete = activation.deleteImpl.release();

    testing::securityChecksStatus = true;
    testing::updateManagerActivatePackageResult =
        software::Activation::Activations::Failed;
    auto result =
        activation.activation(Server::Activation::Activations::Activating);

    EXPECT_EQ(result, Server::Activation::Activations::Activating);
    EXPECT_TRUE(testing::resetActivationBlocksTransitionCalled);
    EXPECT_TRUE(testing::clearFirmwareUpdatePackageCalled);
}

TEST_F(ActivationTest, DISABLED_ActivationActivatingWhenActivatePackageSucceeds)
{
    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const std::string objPath{
        "/xyz/openbmc_project/software/activate_package_success"};

    Activation activation(busMock, objPath,
                          Server::Activation::Activations::Ready,
                          &updateManager);
    [[maybe_unused]] auto leakedDelete = activation.deleteImpl.release();

    testing::securityChecksStatus = true;
    testing::updateManagerActivatePackageResult =
        software::Activation::Activations::Active;
    auto result =
        activation.activation(Server::Activation::Activations::Activating);

    EXPECT_EQ(result, Server::Activation::Activations::Activating);
    EXPECT_FALSE(testing::resetActivationBlocksTransitionCalled);
    EXPECT_TRUE(testing::clearFirmwareUpdatePackageCalled);
}

TEST_F(ActivationTest,
       DISABLED_ActivationActivatingWhenSecurityCheckCallbackErrors)
{
    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const std::string objPath{
        "/xyz/openbmc_project/software/security_check_callback_error"};

    Activation activation(busMock, objPath,
                          Server::Activation::Activations::Ready,
                          &updateManager);
    [[maybe_unused]] auto leakedDelete = activation.deleteImpl.release();

    testing::triggerSecurityChecksError = true;
    auto result =
        activation.activation(Server::Activation::Activations::Activating);

    EXPECT_EQ(result, Server::Activation::Activations::Activating);
    EXPECT_TRUE(testing::resetActivationBlocksTransitionCalled);
    EXPECT_TRUE(testing::clearFirmwareUpdatePackageCalled);
}

TEST_F(ActivationTest,
       DISABLED_Activation_status_activating_updateManager_returns_active)
{
    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};

    Server::Activation::Activations activationState =
        Server::Activation::Activations::Activating;

    const Server::Activation::Activations stateActive =
        Server::Activation::Activations::Active;

    Activation _activation(busMock, objPath, stateActive, &updateManager);
    [[maybe_unused]] auto leakedDelete = _activation.deleteImpl.release();

    _activation.activation(activationState);
    EXPECT_EQ(testing::resultPerformSecurityChecksOnComplete,
              Server::Activation::Activations::Active);
}

TEST_F(ActivationTest,
       DISABLED_Activation_status_activating_updateManager_returns_activating)
{
    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};

    Server::Activation::Activations activationState =
        Server::Activation::Activations::Activating;

    const Server::Activation::Activations stateActive =
        Server::Activation::Activations::Active;

    Activation _activation(busMock, objPath, stateActive, &updateManager);
    [[maybe_unused]] auto leakedDelete = _activation.deleteImpl.release();

    testing::updateManagerActivatePackageResult =
        software::Activation::Activations::Activating;

    Server::Activation::Activations resultState =
        _activation.activation(activationState);

    EXPECT_EQ(resultState, Server::Activation::Activations::Activating);
}

TEST_F(ActivationTest,
       DISABLED_Activation_status_activating_updateManager_returns_failed)
{
    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};

    Server::Activation::Activations activationState =
        Server::Activation::Activations::Activating;

    const Server::Activation::Activations stateActive =
        Server::Activation::Activations::Active;

    Activation _activation(busMock, objPath, stateActive, &updateManager);
    [[maybe_unused]] auto leakedDelete = _activation.deleteImpl.release();

    testing::securityChecksStatus = false;
    _activation.activation(activationState);
    EXPECT_EQ(testing::resultPerformSecurityChecksOnComplete,
              Server::Activation::Activations::Failed);
}

TEST_F(ActivationTest, DISABLED_RequestedActivation_status_active)
{
    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};

    Server::Activation::RequestedActivations requestActivations =
        Server::Activation::RequestedActivations::Active;

    const Server::Activation::Activations stateActive =
        Server::Activation::Activations::Active;

    Activation _activation(busMock, objPath, stateActive, &updateManager);
    [[maybe_unused]] auto leakedDelete = _activation.deleteImpl.release();

    Server::Activation::RequestedActivations resultState =
        _activation.requestedActivation(requestActivations);

    EXPECT_EQ(resultState, requestActivations);
}

TEST_F(ActivationTest, DISABLED_RequestedActivation_status_failed)
{
    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};

    Server::Activation::RequestedActivations requestActivations =
        Server::Activation::RequestedActivations::Active;

    const Server::Activation::Activations stateActive =
        Server::Activation::Activations::Failed;

    Activation _activation(busMock, objPath, stateActive, &updateManager);
    [[maybe_unused]] auto leakedDelete = _activation.deleteImpl.release();

    Server::Activation::RequestedActivations resultState =
        _activation.requestedActivation(requestActivations);

    EXPECT_EQ(resultState, requestActivations);
}

TEST_F(ActivationTest, DISABLED_RequestedActivation_status_ready)
{
    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};

    Server::Activation::RequestedActivations requestActivations =
        Server::Activation::RequestedActivations::Active;

    const Server::Activation::Activations stateActive =
        Server::Activation::Activations::Ready;

    Activation _activation(busMock, objPath, stateActive, &updateManager);
    [[maybe_unused]] auto leakedDelete = _activation.deleteImpl.release();

    Server::Activation::RequestedActivations resultState =
        _activation.requestedActivation(requestActivations);

    EXPECT_EQ(resultState, requestActivations);
}

TEST_F(ActivationTest, ActivationBlocksTransition_Constructor)
{
    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};

    EXPECT_NO_THROW({
        ActivationBlocksTransition activationBlocksTransition(busMock, objPath);
    });
}

class testexception : public std::exception
{
    virtual const char* what() const noexcept
    {
        return "Test exception happened";
    }
} testex;

TEST_F(ActivationTest,
       ActivationBlocksTransition_Constructor_enableRebootGuard_throw_exception)
{
    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};

    EXPECT_CALL(sdbusMock,
                sd_bus_message_new_method_call(_, _, _, _, _, "StartUnit"))
        .WillOnce(Throw(testex))
        .WillOnce(Return(0));

    EXPECT_NO_THROW({
        ActivationBlocksTransition activationBlocksTransition(busMock, objPath);
    });
}

TEST_F(
    ActivationTest,
    ActivationBlocksTransition_Constructor_disableRebootGuard_throw_exception)
{
    testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};

    EXPECT_CALL(sdbusMock,
                sd_bus_message_new_method_call(_, _, _, _, _, "StartUnit"))
        .WillOnce(Return(0))
        .WillOnce(Throw(testex));

    EXPECT_NO_THROW({
        ActivationBlocksTransition activationBlocksTransition(busMock, objPath);
    });
}
