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

#include "debug_token.hpp"

#include "libpldm/firmware_update.h"

#include "../activation.hpp"
#include "../dbusutil.hpp"
#include "../update_manager.hpp"
#include "common/types.hpp"
#include "common/utils.hpp"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <phosphor-logging/lg2.hpp>
#include <xyz/openbmc_project/Common/FilePath/server.hpp>
#include <xyz/openbmc_project/Common/UUID/server.hpp>
#include <xyz/openbmc_project/Common/error.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <tuple>

namespace pldm
{
namespace fw_update
{
namespace MatchRules = sdbusplus::bus::match::rules;

bool DebugToken::activate()
{
    bool activationStatus = true;

    pldm::utils::DBusMapping dbusMapping{tokenPath,
                                         Server::Activation::interface,
                                         "RequestedActivation", "string"};
    lg2::info("Activating : OBJPATH={OBJPATH}", "OBJPATH", tokenPath);
    try
    {
        pldm::utils::DBusHandler().setDbusProperty(
            dbusMapping, std::string(Server::Activation::interface) +
                             ".RequestedActivations.Active");
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "Failed to set resource RequestedActivation: OBJPATH={OBJPATH}",
            "OBJPATH", tokenPath, "ERROR", e);
        createLogEntry(transferFailed,
                       std::filesystem::path(tokenPath).filename(),
                       tokenVersion, transferFailedResolution);
        activationStatus = false;
    }
    return activationStatus;
}

void DebugToken::onActivationChangedMsg(sdbusplus::message::message& msg)
{
    using Interface = std::string;
    Interface interface;
    pldm::dbus::PropertyMap properties;
    Server::Activation::Activations activationState =
        Server::Activation::Activations::NotReady;
    std::optional<std::string> activationString;
    std::string objPath = msg.get_path();

    if (objPath == tokenPath)
    {
        msg.read(interface, properties);
        auto prop = properties.find("Activation");
        if (prop != properties.end())
        {
            activationString = std::get<std::string>(prop->second);
        }
        if (activationString.has_value())
        {
            activationState = Server::Activation::convertActivationsFromString(
                *activationString);
        }
        if (activationState == Server::Activation::Activations::Active ||
            activationState == Server::Activation::Activations::Failed)
        {
            startUpdate();
            tokenStatus = true;
        }
    }
}

void DebugToken::updateDebugToken(
    const FirmwareDeviceIDRecords& fwDeviceIDRecords,
    const ComponentImageInfos& componentImageInfos, std::istream& package)
{
    bool installToken = false;
    for (size_t index = 0; index < fwDeviceIDRecords.size(); ++index)
    {
        const auto& fwDeviceIDRecord = fwDeviceIDRecords[index];
        const auto& deviceIDDescriptors =
            std::get<Descriptors>(fwDeviceIDRecord);
        for (auto& it : deviceIDDescriptors) // For each Descriptors
        {
            if (it.first == PLDM_FWUP_UUID) // Check UUID
            {
                std::ostringstream tempStream;
                for (int byte : std::get<0>(it.second))
                {
                    tempStream << std::setfill('0') << std::setw(2) << std::hex
                               << byte;
                }

                std::string uuid = tempStream.str(); // Extract UUID
                using namespace std;
                transform(uuid.begin(), uuid.end(), uuid.begin(), ::toupper);
                if (uuid != InstallTokenUUID)
                {
                    continue; // no matching uuid skip to next uuid
                }
                const auto& applicableCompVec =
                    std::get<ApplicableComponents>(fwDeviceIDRecord);
                if (applicableCompVec.size() == 0)
                {
                    lg2::error("Invalid applicable components");
                    continue;
                }
                const auto& componentImageInfo =
                    componentImageInfos[applicableCompVec[0]];
                if (std::get<static_cast<size_t>(
                        ComponentImageInfoPos::CompIdentifierPos)>(
                        componentImageInfo) != deadComponent)
                {
                    continue;
                }
                const auto& version = std::get<static_cast<size_t>(
                    ComponentImageInfoPos::CompVersionPos)>(componentImageInfo);
                std::string filepath = "";
                std::string objPath;
                try
                {
                    // get File PATH and object path
                    std::tie(filepath, objPath) = getFilePath(uuid);
                }
                catch (const sdbusplus::exception::SdBusError& e)
                {
                    lg2::error("failed to get filepath.", "ERROR", e);
                    continue;
                }
                lg2::info("Got filepath for install token. FILEPATH={FILEPATH}",
                          "FILEPATH", filepath);
                if (filepath == "")
                {
                    continue;
                }
                package.seekg(
                    std::get<5>(componentImageInfo)); // SEEK to image offset
                std::vector<uint8_t> buffer(std::get<6>(componentImageInfo));
                package.read(reinterpret_cast<char*>(buffer.data()),
                             buffer.size());

                filepath += "/" + boost::uuids::to_string(
                                      boost::uuids::random_generator()())
                                      .substr(0, 8);
                lg2::info(
                    "Extracting to filepath: VERSION={VERSION}, FILEPATH={FILEPATH}",
                    "VERSION", version, "FILEPATH", filepath);
                std::ofstream outfile(filepath, std::ofstream::binary);
                outfile.write(reinterpret_cast<const char*>(&buffer[0]),
                              buffer.size() *
                                  sizeof(uint8_t)); // Write to image offset
                outfile.close();
                tokenPath = objPath;
                installToken = true;
                tokenVersion = version;
            }
        }
    }
    if (!installToken)
    {
        try
        {
            auto [filepath, objPath] = getFilePath(EraseTokenUUID);
            tokenPath = objPath;
            tokenVersion = "0.0"; // erase token doesn't have any version
        }
        catch (const sdbusplus::exception::SdBusError& e)
        {
            lg2::error("failed to get filepath.", "ERROR", e);
            createLogEntry(transferFailed, "HGX_FW_Debug_Token_Erase", "0.0",
                           transferFailedResolution);
            startUpdate();
            return;
        }
    }
    activationMatches.emplace_back(
        bus,
        MatchRules::propertiesChanged(tokenPath, Server::Activation::interface),
        std::bind(&DebugToken::onActivationChangedMsg, this,
                  std::placeholders::_1));

    activationMatches.emplace_back(
        bus,
        MatchRules::propertiesChanged(tokenPath,
                                      Server::ActivationProgress::interface),
        std::bind(&DebugToken::onActivationChangedMsg, this,
                  std::placeholders::_1));
    setVersion();
    if (!activate())
    {
        lg2::error("Activation failed for debug token");
        startUpdate();
        return;
    }
    startTimer(debugTokenTimeout);
    return;
}

std::pair<std::string, std::string>
    DebugToken::getFilePath(const std::string& uuid)
{
    std::vector<std::string> paths;
    getValidPaths(paths);
    auto dbusHandler = pldm::utils::DBusHandler();
    for (auto& obj : paths)
    {
        auto u = dbusHandler.getDbusProperty<std::string>(
            obj.c_str(), "UUID",
            sdbusplus::xyz::openbmc_project::Common::server::UUID::interface);
        if (u != "")
        {
            transform(u.begin(), u.end(), u.begin(), ::toupper);
            if (u == uuid)
            {
                auto p = dbusHandler.getDbusProperty<std::string>(
                    obj.c_str(), "Path",
                    sdbusplus::xyz::openbmc_project::Common::server::FilePath::
                        interface);
                if (p != "")
                {
                    return {std::filesystem::path(p).parent_path(), obj};
                }
            }
        }
    }
    return {};
}

void DebugToken::getValidPaths(std::vector<std::string>& paths)
{
    try
    {
        auto& bus = pldm::utils::DBusHandler::getBus();

        auto method = bus.new_method_call(
            pldm::utils::mapperService, pldm::utils::mapperPath,
            pldm::utils::mapperInterface, "GetSubTreePaths");
        method.append("/xyz/openbmc_project/software");
        method.append(0); // Depth 0 to search all
        method.append(
            std::vector<std::string>({sdbusplus::xyz::openbmc_project::Common::
                                          server::UUID::interface}));
        auto reply = bus.call(method);
        reply.read(paths);
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "Failed to get software D-Bus objects implementing UUID interface, ERROR={ERROR}",
            "ERROR", e);
    }
}

void DebugToken::startTimer(auto timerExpiryTime)
{
    timer = std::make_unique<sdbusplus::Timer>([this]() {
        if (!tokenStatus)
        {
            activationMatches.clear();
            auto componentName = std::filesystem::path(tokenPath).filename();
            if (componentName == "HGX_FW_Debug_Token_Erase")
            {
                auto eraseMessage = "Operation timed out.";
                auto eraseResolution =
                    "No action required. If there are other"
                    " component failures in task, retry the firmware update"
                    " operation and if issue still persists reset the baseboard.";
                createLogEntry(debugTokenEraseFailed, componentName,
                               eraseMessage, eraseResolution);
            }
            else
            {
                createLogEntry(transferFailed, componentName, tokenVersion,
                               transferFailedResolution);
            }
            lg2::error("Activation Timer expired for install debug token");
            startUpdate();
        }
    });
    lg2::info("Starting Timer to allow install or erase debug token");
    timer->start(std::chrono::seconds(timerExpiryTime), false);
}

void DebugToken::startUpdate()
{
    updateManager->startPLDMUpdate();
    auto nonPLDMState = updateManager->startNonPLDMUpdate();
    if (nonPLDMState == software::Activation::Activations::Failed ||
        nonPLDMState == software::Activation::Activations::Active)
    {
        updateManager->setActivationStatus(nonPLDMState);
    }
}

void DebugToken::setVersion()
{
    pldm::utils::DBusMapping dbusMapping{
        tokenPath, "xyz.openbmc_project.Software.ExtendedVersion",
        "ExtendedVersion", "string"};
    try
    {
        pldm::utils::DBusHandler().setDbusProperty(dbusMapping, tokenVersion);
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        lg2::error("Failed to set extended version.", "ERROR", e);
    }
}

} // namespace fw_update
} // namespace pldm