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
#include "common/dBusAsyncUtils.hpp"
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

pldm::utils::DBusHandlerInterface& DebugToken::defaultDbusHandler()
{
    static pldm::utils::DBusHandler handler;
    return handler;
}

exec::task<bool> DebugToken::activate()
{
    info("Activating : OBJPATH={OBJPATH}", "OBJPATH", tokenPath);

    // GCC 13 coroutine ICE workaround (matches the pattern documented in
    // other_device_update_manager.cpp): bind every argument to a local
    // lvalue before the co_await so the awaitable's reference-typed members
    // remain valid across the suspension.
    const std::string objectPath = tokenPath;
    const std::string interface = Server::Activation::interface;
    const std::string property = "RequestedActivation";
    std::string value = std::string(Server::Activation::interface) +
                        ".RequestedActivations.Active";
    const bool success = co_await pldm::utils::coSetDbusProperty<std::string>(
        objectPath, interface, property, std::move(value));
    if (success)
    {
        co_return true;
    }

    error("Failed to set resource RequestedActivation: OBJPATH={OBJPATH}",
          "OBJPATH", tokenPath);

    auto componentName = std::filesystem::path(tokenPath).filename();
    if (componentName == "HGX_FW_Debug_Token_Erase")
    {
        auto eraseResolution =
            "No action required. If there are other"
            " component failures in task, retry the firmware"
            " update operation and if issue still persists"
            " reset the baseboard.";
        createLogEntry(debugTokenEraseFailed, componentName,
                       "Operation timed out.", eraseResolution);
    }
    else
    {
        createLogEntry(transferFailed, componentName, tokenVersion,
                       transferFailedResolution);
    }
    if (!tokenStatus)
    {
        tokenStatus = true;
        activationMatches.clear();
        startUpdate();
    }
    co_return false;
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
            if (const auto* p = std::get_if<std::string>(&prop->second))
            {
                activationString = *p;
            }
        }
        if (activationString.has_value())
        {
            activationState = Server::Activation::convertActivationsFromString(
                *activationString);
        }
        if (activationState == Server::Activation::Activations::Active ||
            activationState == Server::Activation::Activations::Failed)
        {
            if (tokenStatus)
            {
                return;
            }
            tokenStatus = true;
            activationMatches.clear();
            startUpdate();
        }
    }
}

exec::task<void> DebugToken::updateDebugToken(
    FirmwareDeviceIDRecords fwDeviceIDRecords,
    ComponentImageInfos componentImageInfos, std::istream& package)
{
    // fwDeviceIDRecords and componentImageInfos are taken by value, so the
    // coroutine frame owns them outright — safe to access across co_await.
    // `package` is only used in the synchronous prologue below; see header
    // for the invariant. Do not add post-await reads from `package`.
    installToken = false;
    bool installTokenInPackage = false;
    for (size_t index = 0; index < fwDeviceIDRecords.size(); ++index)
    {
        const auto& fwDeviceIDRecord = fwDeviceIDRecords[index];
        const auto& deviceIDDescriptors =
            std::get<Descriptors>(fwDeviceIDRecord);
        for (auto& it : deviceIDDescriptors) // For each Descriptors
        {
            if (it.first == PLDM_FWUP_UUID)  // Check UUID
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
                    error("Invalid applicable components");
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
                // The package carries an install token for us. Remember that
                // independently of whether its object resolves, so that a
                // failed lookup is not mistaken for "no install token" below.
                installTokenInPackage = true;
                const auto& version = std::get<static_cast<size_t>(
                    ComponentImageInfoPos::CompVersionPos)>(componentImageInfo);
                std::string filepath = "";
                std::string objPath;
                std::tie(filepath, objPath) = getFilePath(uuid);
                if (filepath.empty() || objPath.empty())
                {
                    continue;
                }
                info("Got filepath for install token. FILEPATH={FILEPATH}",
                     "FILEPATH", filepath);
                package.seekg(
                    std::get<5>(componentImageInfo)); // SEEK to image offset
                std::vector<uint8_t> buffer(std::get<6>(componentImageInfo));
                package.read(reinterpret_cast<char*>(buffer.data()),
                             buffer.size());

                filepath += "/" + boost::uuids::to_string(
                                      boost::uuids::random_generator()())
                                      .substr(0, 8);
                info(
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
    if (!installToken && installTokenInPackage)
    {
        // An install token was requested but its object never resolved. Do
        // not fall through to the erase branch: that would silently erase a
        // token when the caller asked to install one.
        error("Cannot install debug token: no D-Bus object for UUID={UUID}",
              "UUID", InstallTokenUUID);
        startUpdate();
        co_return;
    }
    if (!installToken)
    {
        // Only the object path matters here; the erase branch never uses the
        // directory. getFilePath() derives it with parent_path(), which is
        // empty for a Path with no directory component, so testing it would
        // reject an erase object that resolved successfully.
        const auto objPath = getFilePath(EraseTokenUUID).second;
        if (objPath.empty())
        {
            error("Cannot erase debug token: no D-Bus object for UUID={UUID}",
                  "UUID", EraseTokenUUID);
            createLogEntry(debugTokenEraseFailed, "HGX_FW_Debug_Token_Erase",
                           "Debug token service is not ready.",
                           transferFailedResolution);
            startUpdate();
            co_return;
        }
        tokenPath = objPath;
        tokenVersion = eraseTokenVersion;
    }

    try
    {
        activationMatches.emplace_back(
            bus,
            MatchRules::propertiesChanged(tokenPath,
                                          Server::Activation::interface),
            std::bind(&DebugToken::onActivationChangedMsg, this,
                      std::placeholders::_1));
    }
    catch (const std::exception& e)
    {
        /* emplace_back function will call constructor of
        std::vector<sdbusplus::bus::match_t>, which in some cases throws
        an exception. For example, when tokenpath is empty, the generated match
        rule is invalid and causes an exception when creating match_t */
        error(
            "Failed to create match_t for interface {FAILED_MATCH} and token path {TOKENPATH} with error: {ERROR}",
            "FAILED_MATCH", Server::Activation::interface, "TOKENPATH",
            tokenPath, "ERROR", e.what());
        startUpdate();
        co_return;
    }

    try
    {
        activationMatches.emplace_back(
            bus,
            MatchRules::propertiesChanged(
                tokenPath, Server::ActivationProgress::interface),
            std::bind(&DebugToken::onActivationChangedMsg, this,
                      std::placeholders::_1));
    }
    catch (const std::exception& e)
    {
        /* Catch potential exception like the previous emplace_back call */
        error(
            "Failed to create match_t for interface {FAILED_MATCH} and token path {TOKENPATH} with error: {ERROR}",
            "FAILED_MATCH", Server::ActivationProgress::interface, "TOKENPATH",
            tokenPath, "ERROR", e.what());
        startUpdate();
        co_return;
    }

    co_await setVersion();
    const bool activated = co_await activate();
    // Only arm the activation-timeout safety net when the property-set
    // succeeded and we are genuinely waiting on a property-change signal
    // from the token service. On failure, activate() has already run the
    // synchronous failure cleanup (log entry + startUpdate()), so a timer
    // here would just hold a 60s-pinned lambda on `this` for nothing.
    if (activated)
    {
        startTimer(debugTokenTimeout);
    }
    co_return;
}

void DebugToken::startTokenUpdate(
    const FirmwareDeviceIDRecords& fwDeviceIDRecords,
    const ComponentImageInfos& componentImageInfos, std::istream& package)
{
    tokenScope.spawn(
        updateDebugToken(fwDeviceIDRecords, componentImageInfos, package),
        exec::default_task_context<void>(stdexec::inline_scheduler{}));
}

std::pair<std::string, std::string> DebugToken::getFilePath(
    const std::string& uuid)
{
    // TODO(async-reads): convert to exec::task once the caller chain
    // (updateDebugToken -> activatePackage -> sdbusplus property setter in
    // activation.cpp) supports coroutines. Today the property setter is a
    // synchronous sdbusplus handler so co_await-ing from here would require
    // bubbling exec::task through it. The blocking reads here run only
    // during the single updateDebugToken setup step, not in the steady-state
    // event loop, so the stall window is bounded.
    std::vector<std::string> paths;
    getValidPaths(paths);
    for (auto& obj : paths)
    {
        try
        {
            auto u = std::get<std::string>(dbusHandler.getDbusPropertyVariant(
                obj.c_str(), "UUID",
                sdbusplus::xyz::openbmc_project::Common::server::UUID::
                    interface));
            if (u != "")
            {
                transform(u.begin(), u.end(), u.begin(), ::toupper);
                if (u == uuid)
                {
                    try
                    {
                        auto p = std::get<std::string>(
                            dbusHandler.getDbusPropertyVariant(
                                obj.c_str(), "Path",
                                sdbusplus::xyz::openbmc_project::Common::
                                    server::FilePath::interface));
                        if (p != "")
                        {
                            return {std::filesystem::path(p).parent_path(),
                                    obj};
                        }
                    }
                    catch (const std::exception& e)
                    {
                        error(
                            "Failed to get D-Bus property 'Path': OBJ={OBJ} ERROR={ERROR}",
                            "OBJ", obj, "ERROR", e);
                    }
                }
            }
        }
        catch (const std::exception& e)
        {
            error(
                "Failed to get D-Bus property 'UUID': OBJ={OBJ} ERROR={ERROR}",
                "OBJ", obj, "ERROR", e);
        }
    }
    error("No debug token object for UUID={UUID}, searched {PATHCOUNT} path(s)",
          "UUID", uuid, "PATHCOUNT", paths.size());
    return {};
}

void DebugToken::getValidPaths(std::vector<std::string>& paths)
{
    try
    {
        paths = dbusHandler.getSubTreePaths(
            "/xyz/openbmc_project/software/other", 0,
            std::vector<std::string>{sdbusplus::xyz::openbmc_project::Common::
                                         server::UUID::interface});
    }
    catch (const std::exception& e)
    {
        error(
            "Failed to get software D-Bus objects implementing UUID interface, ERROR={ERROR}",
            "ERROR", e);
    }
}

void DebugToken::startTimer(auto timerExpiryTime)
{
    timer = std::make_unique<sdbusplus::Timer>([this]() {
        if (!tokenStatus)
        {
            tokenStatus = true;
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
            error("Activation Timer expired for install debug token");
            startUpdate();
        }
    });
    info("Starting Timer to allow install or erase debug token");
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

exec::task<void> DebugToken::setVersion()
{
    const std::string objectPath = tokenPath;
    const std::string interface =
        "xyz.openbmc_project.Software.ExtendedVersion";
    const std::string property = "ExtendedVersion";
    std::string value = tokenVersion;
    [[maybe_unused]] bool ok =
        co_await pldm::utils::coSetDbusProperty<std::string>(
            objectPath, interface, property, std::move(value));
    // Failure is intentionally non-fatal here: ExtendedVersion is used by the
    // item updater for message-registry decoration only. The legacy
    // setDBusPropertyAsync call also discarded errors.
    co_return;
}

} // namespace fw_update
} // namespace pldm
