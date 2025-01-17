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
#include "fw-update/device_updater.hpp"
#include "fw-update/package_parser.hpp"
#include "fw-update/update_manager.hpp"

namespace software = sdbusplus::xyz::openbmc_project::Software::server;

namespace testing
{

software::Activation::Activations updateManagerActivatePackageResult =
    software::Activation::Activations::Active;
software::Activation::Activations resultPerformSecurityChecksOnComplete =
    software::Activation::Activations::NotReady;
bool securityChecksStatus = true;

class FakeUpdateManager
{
  public:
    bool fwDebug = true;
    bool isStageOnlyUpdate = false;

    software::Activation::Activations activatePackage()
    {
        return updateManagerActivatePackageResult;
    }

    void clearActivationInfo()
    {
        return;
    }

    void resetActivationBlocksTransition()
    {
        return;
    }

    void clearFirmwareUpdatePackage()
    {
        return;
    }
    void clearStagedPackage()
    {
        return;
    }
    int processPackage(
        [[maybe_unused]] const std::filesystem::path& packageFilePath)
    {
        return 0;
    }
    void restoreStagedPackageActivationObjects()
    {
        return;
    }
    void closePackage()
    {
        return;
    }
    void performSecurityChecksAsync(
        std::function<void(bool)> onComplete,
        [[maybe_unused]] std::function<void(const std::string& errorMsg)>
            onError)
    {
        onComplete = this->performSecurityChecksOnComplete;
        onComplete(securityChecksStatus);
    }
    std::string stagedObjPath;
    std::filesystem::path stagedfwPackageFilePath;

    std::function<void(bool)> performSecurityChecksOnComplete =
        [](bool result) {
            if (result)
            {
                resultPerformSecurityChecksOnComplete =
                    software::Activation::Activations::Active;
            }
            else
            {
                resultPerformSecurityChecksOnComplete =
                    software::Activation::Activations::Failed;
            }
        };
};
} // namespace testing

#define UpdateManager testing::FakeUpdateManager