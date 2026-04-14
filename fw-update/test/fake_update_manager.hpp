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
#pragma once

#include "libpldm/firmware_update.h"

#include "common/types.hpp"
#include "common/utils.hpp"
#define UpdateManager RealUpdateManagerForActivationTest
#include "fw-update/update_manager.hpp"
#undef UpdateManager

#include <xyz/openbmc_project/Software/Activation/server.hpp>

#include <functional>
#include <stdexcept>

namespace software = sdbusplus::xyz::openbmc_project::Software::server;

namespace testing
{

software::Activation::Activations updateManagerActivatePackageResult =
    software::Activation::Activations::Active;
software::Activation::Activations resultPerformSecurityChecksOnComplete =
    software::Activation::Activations::NotReady;
bool securityChecksStatus = true;
bool startPLDMUpdateCalled = false;
bool startNonPLDMUpdateCalled = false;
bool setActivationStatusCalled = false;
software::Activation::Activations startNonPLDMUpdateResult =
    software::Activation::Activations::Active;
software::Activation::Activations lastSetActivationStatus =
    software::Activation::Activations::NotReady;
bool triggerSecurityChecksError = false;
std::string securityChecksErrorMessage = "security checks callback error";
bool clearActivationInfoCalled = false;
bool resetActivationBlocksTransitionCalled = false;
bool clearFirmwareUpdatePackageCalled = false;
bool throwOnActivatePackage = false;
size_t clearFirmwareUpdatePackageCallCount = 0;
size_t resetActivationBlocksTransitionCallCount = 0;
size_t clearActivationInfoCallCount = 0;

void resetTestState()
{
    startPLDMUpdateCalled = false;
    startNonPLDMUpdateCalled = false;
    setActivationStatusCalled = false;
    startNonPLDMUpdateResult = software::Activation::Activations::Active;
    lastSetActivationStatus = software::Activation::Activations::NotReady;
    triggerSecurityChecksError = false;
    securityChecksErrorMessage = "security checks callback error";
    clearActivationInfoCalled = false;
    resetActivationBlocksTransitionCalled = false;
    clearFirmwareUpdatePackageCalled = false;
    throwOnActivatePackage = false;
    clearFirmwareUpdatePackageCallCount = 0;
    resetActivationBlocksTransitionCallCount = 0;
    clearActivationInfoCallCount = 0;
}

} // namespace testing

// This macro replaces UpdateManager with FakeUpdateManager
// It must be defined before activation.hpp is included
#define UpdateManager FakeUpdateManager

namespace pldm::fw_update
{

class FakeUpdateManager
{
  public:
    bool fwDebug = true;

    software::Activation::Activations activatePackage()
    {
        if (testing::throwOnActivatePackage)
        {
            throw std::runtime_error("activatePackage failure");
        }
        return testing::updateManagerActivatePackageResult;
    }

    void startPLDMUpdate()
    {
        testing::startPLDMUpdateCalled = true;
    }

    software::Activation::Activations startNonPLDMUpdate()
    {
        testing::startNonPLDMUpdateCalled = true;
        return testing::startNonPLDMUpdateResult;
    }

    void setActivationStatus(const software::Activation::Activations& state)
    {
        testing::setActivationStatusCalled = true;
        testing::lastSetActivationStatus = state;
    }

    void clearActivationInfo()
    {
        testing::clearActivationInfoCalled = true;
        testing::clearActivationInfoCallCount++;
        return;
    }

    void resetActivationBlocksTransition()
    {
        testing::resetActivationBlocksTransitionCalled = true;
        testing::resetActivationBlocksTransitionCallCount++;
        return;
    }

    void clearFirmwareUpdatePackage()
    {
        testing::clearFirmwareUpdatePackageCalled = true;
        testing::clearFirmwareUpdatePackageCallCount++;
        return;
    }
    void closePackage()
    {
        return;
    }
    void performSecurityChecksAsync(
        std::function<void(bool)> onComplete,
        std::function<void(const std::string& errorMsg)> onError)
    {
        if (testing::triggerSecurityChecksError)
        {
            onError(testing::securityChecksErrorMessage);
            return;
        }
        this->performSecurityChecksOnComplete(testing::securityChecksStatus);
        onComplete(testing::securityChecksStatus);
    }
    std::function<void(bool)> performSecurityChecksOnComplete =
        [](bool result) {
            if (result)
            {
                testing::resultPerformSecurityChecksOnComplete =
                    software::Activation::Activations::Active;
            }
            else
            {
                testing::resultPerformSecurityChecksOnComplete =
                    software::Activation::Activations::Failed;
            }
        };
};

} // namespace pldm::fw_update
