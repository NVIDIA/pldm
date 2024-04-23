/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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

#include "common/types.hpp"
#include "common/utils.hpp"

#include <sdbusplus/timer.hpp>
#include <xyz/openbmc_project/Software/Activation/server.hpp>
#include <xyz/openbmc_project/Software/ActivationProgress/server.hpp>

namespace pldm
{
namespace fw_update
{

namespace MatchRules = sdbusplus::bus::match::rules;
namespace Server = sdbusplus::xyz::openbmc_project::Software::server;
class UpdateManager;
const std::string InstallTokenUUID = "76910DFA1E4C11ED861D0242AC120002";
const std::string EraseTokenUUID = "76910DFA1E4C11ED861D0242AE52A53E";
const std::string transferFailedResolution =
    "Debug Token Service is not ready, retry the firmware update operation "
    "after the management controller is ready. If the issue still persists"
    " reset the baseboard.";

/**
 * @brief Debug Token implementation in pldm for token installation and erase
 */
class DebugToken
{
  public:
    /**
     * @brief Timeout for debug token install or erase operation
     *
     */
    auto static constexpr debugTokenTimeout = std::chrono::seconds(180);

    DebugToken() = delete;
    DebugToken(const DebugToken&) = delete;
    DebugToken(DebugToken&&) = delete;
    DebugToken& operator=(const DebugToken&) = delete;
    DebugToken& operator=(DebugToken&&) = delete;
    ~DebugToken() = default;

    /**
     * @brief Debug token object for install or erase token
     *
     * @param[in] bus - sdbusplus referance
     * @param[in] updateManager - update manager reference
     */
    explicit DebugToken(sdbusplus::bus::bus& bus,
                        UpdateManager* updateManager) :
        updateManager(updateManager),
        bus(bus), timer(nullptr), tokenStatus(false)
    {}

    /**
     * @brief From pldm image extracts the debug token image and copies to
     * respective location
     *
     * @param[in] fwDeviceIDRecords - Device records
     * @param[in] componentImageInfos - Image info like offset, size
     * @param[in] package - pldm image input stream
     * @return void - debug token install/erase status is ignored
     */
    void updateDebugToken(const FirmwareDeviceIDRecords& fwDeviceIDRecords,
                          const ComponentImageInfos& componentImageInfos,
                          std::istream& package);

  private:
    UpdateManager* updateManager;
    /* install or erase token path */
    std::string tokenPath;
    /* debug token install or erase version */
    std::string tokenVersion;
    /**
     * @brief Dbus object referance
     *
     */
    sdbusplus::bus::bus& bus;

    /**
     * @brief matcher rule to check for activation dbus object change
     *
     */
    std::vector<sdbusplus::bus::match_t> activationMatches;

    /**
     * @brief Timer for debug token install or erase
     *
     */
    std::unique_ptr<phosphor::Timer> timer;

    /* contains install or erase completion status */
    bool tokenStatus;

    /**
     * @brief Async call to monitor the activate change in D-Bus
     *
     * @param[in] msg - msg
     */
    void onActivationChangedMsg(sdbusplus::message::message& msg);
    /**
     * @brief Timer for debug token install or erase timeout
     *
     * @param[in] - expiry time
     */
    void startTimer(auto timerExpiryTime);

    /**
     * @brief Get file path based on UUID
     *
     * @param[in] UUID - UUID to find file path for
     * @return pair with filepath and object path, returns {} on no match
     *
     */
    std::pair<std::string, std::string> getFilePath(const std::string& uuid);

    /**
     * @brief Get valid D-Bus object paths that may contain UUIDs
     *
     * @param[in] paths - object to store the paths into
     */
    void getValidPaths(std::vector<std::string>& paths);

    /**
     * @brief Activates all other devices
     *
     * @return true if successfull
     * @return false otherwise
     */
    bool activate();

    /**
     * @brief triggers pldm and non-pldm updates
     *
     */
    void startUpdate();
    /**
     * @brief set the extended version for item updater to update
    message registry and pass this to token installer
     *
     */
    void setVersion();
};

} // namespace fw_update
} // namespace pldm