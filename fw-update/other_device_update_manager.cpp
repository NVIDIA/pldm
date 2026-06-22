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

#include "other_device_update_manager.hpp"

#include "libpldm/firmware_update.h"

#include "activation.hpp"
#include "common/types.hpp"
#include "common/utils.hpp"
#include "dbusutil.hpp"
#include "update_manager.hpp"

#include <unistd.h>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <phosphor-logging/lg2.hpp>
#include <xyz/openbmc_project/Common/FilePath/server.hpp>
#include <xyz/openbmc_project/Common/UUID/server.hpp>
#include <xyz/openbmc_project/Common/error.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/Asset/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/SKU/server.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <tuple>
#include <unordered_set>

#define SW_PATH_OTHER "/xyz/openbmc_project/software/other"

namespace pldm
{

namespace fw_update
{

namespace MatchRules = sdbusplus::bus::match::rules;

pldm::utils::DBusHandlerInterface&
    OtherDeviceUpdateManager::defaultDbusHandler()
{
    static pldm::utils::DBusHandler handler;
    return handler;
}

Server::Activation::Activations
    OtherDeviceUpdateManager::getOverAllActivationState()
{
    Server::Activation::Activations state =
        Server::Activation::Activations::Active;
    for (auto& x : otherDevices)
    {
        if ((x.second)->activationState ==
            Server::Activation::Activations::Activating)
        {
            return Server::Activation::Activations::Activating;
        }
        if ((x.second)->activationState !=
            Server::Activation::Activations::Active)
        {
            state = Server::Activation::Activations::Failed;
        }
    }

    return state;
}

void OtherDeviceUpdateManager::activate()
{
    std::weak_ptr<bool> weakAlive = aliveFlag;
    for (auto& x : otherDevices)
    {
        auto& path = x.first;
        auto uuid = x.second->uuid;

        info("Activating : OBJPATH = {PATH}", "PATH", path);

        auto componentName = uuidMappings[uuid].componentName;
        setDBusPropertyAsync(
            path, Server::Activation::interface, "RequestedActivation",
            std::string(Server::Activation::interface) +
                ".RequestedActivations.Active",

            [this, weakAlive, uuid, componentName = std::move(componentName),
             path](bool success) {
                if (success)
                {
                    return;
                }
                error("Failed to set resource RequestedActivation : {PATH}",
                      "PATH", path);
                std::string resolution = "Retry firmware update operation";
                std::string messageArg0 = "Firmware Update Service";
                std::string messageArg1 =
                    componentName + " firmware update timed out";
                createLogEntry(resourceErrorDetected, messageArg0, messageArg1,
                               resolution);
                if (weakAlive.expired())
                {
                    return;
                }
                updateManager->updateOtherDeviceCompletion(uuid, false);
            });
    }
}

void OtherDeviceUpdateManager::onActivationChangedMsg(
    sdbusplus::message::message& msg)
{
    using Interface = std::string;
    Interface interface;
    pldm::dbus::PropertyMap properties;
    std::string objPath = msg.get_path();

    msg.read(interface, properties);
    onActivationChanged(objPath, properties);

    if (otherDevices.find(objPath) != otherDevices.end())
    {
        if (otherDevices[objPath]->activationState ==
            Server::Activation::Activations::Active)
        {
            /*
             * Conditions to add awaitToActivate message for Non PLDM Components
             * in Summary Log: Condition 1: Targets vector is empty implying
             * that no target filtering is done. In this case, the Active state
             * is from an update to the component Condition 2: Check if any Non
             * PLDM components are part of the target filtering list.
             * */
            if (targets.empty() ||
                std::find_if(targets.begin(), targets.end(),
                             [&](const std::string& target) {
                                 const std::string targetBaseName =
                                     target.substr(target.rfind('/') + 1);
                                 const std::string objBaseName =
                                     objPath.substr(objPath.rfind('/') + 1);

                                 return (targetBaseName.find(objBaseName) !=
                                         std::string::npos);
                             }) != targets.end())
            {
                updateManager->updateOtherDeviceCompletion(
                    otherDevices[objPath]->uuid, true,
                    uuidMappings[otherDevices[objPath]->uuid].componentName);
            }
            else
            {
                updateManager->updateOtherDeviceCompletion(
                    otherDevices[objPath]->uuid, true);
            }
        }
        else if (otherDevices[objPath]->activationState ==
                 Server::Activation::Activations::Failed)
        {
            updateManager->updateOtherDeviceCompletion(
                otherDevices[objPath]->uuid, false);
        }
    }
}

void OtherDeviceUpdateManager::onActivationChanged(
    const std::string& objPath, const pldm::dbus::PropertyMap& properties)
{
    std::optional<std::string> activationString;
    std::optional<std::string> reqActivation;
    auto prop = properties.find("Activation");
    if (prop != properties.end())
    {
        activationString = std::get<std::string>(prop->second);
    }
    prop = properties.find("RequestedActivation");
    if (prop != properties.end())
    {
        reqActivation = std::get<std::string>(prop->second);
    }
    if (otherDevices.find(objPath) != otherDevices.end())
    {
        if (activationString.has_value())
        {
            otherDevices[objPath]->activationState =
                Server::Activation::convertActivationsFromString(
                    *activationString);
        }
        if (reqActivation.has_value())
        {
            otherDevices[objPath]->requestedActivation =
                Server::Activation::convertRequestedActivationsFromString(
                    *reqActivation);
        }
    }
}

void OtherDeviceUpdateManager::setUpdatePolicy(const std::string& path,
                                               const std::string& uuid)
{
    std::weak_ptr<bool> weakAlive = aliveFlag;
    setDBusPropertyAsync(
        path, "xyz.openbmc_project.Software.UpdatePolicy", "Targets", targets,
        [this, weakAlive, path, uuid](bool success) {
            if (success)
            {
                return;
            }
            error("Failed to set targets for {PATH}", "PATH", path);
            if (weakAlive.expired())
            {
                return;
            }
            isImageFileProcessed[uuid] = false;
        });
}

void OtherDeviceUpdateManager::interfaceAdded(sdbusplus::message::message& m)
{
    sdbusplus::object_path objPath;
    pldm::dbus::InterfaceMap interfaces;
    m.read(objPath, interfaces);

    std::string path(std::move(objPath));
    if (interfaceAddedMatch == nullptr)
    {
        return;
    }
    for (const auto& intf : interfaces)
    {
        info("New Interface Added. OBJPATH={PATH}, INTF={INTF}", "PATH", path,
             "INTF", intf.first);
        if (intf.first ==
            sdbusplus::xyz::openbmc_project::Common::server::UUID::interface)
        {
            for (const auto& property : intf.second)
            {
                if (property.first == "UUID")
                {
                    std::string uuid = std::get<std::string>(property.second);
                    using namespace std;
                    transform(uuid.begin(), uuid.end(), uuid.begin(),
                              ::toupper);

                    if (otherDevices.find(path) == otherDevices.end())
                    {
                        otherDevices.emplace(
                            path,
                            std::make_unique<OtherDeviceUpdateActivation>());
                        otherDevices[path]->uuid = uuid;
                        activationMatches.emplace_back(
                            bus,
                            MatchRules::propertiesChanged(
                                path, Server::Activation::interface),
                            std::bind(&OtherDeviceUpdateManager::
                                          onActivationChangedMsg,
                                      this, std::placeholders::_1));

                        activationMatches.emplace_back(
                            bus,
                            MatchRules::propertiesChanged(
                                path, Server::ActivationProgress::interface),
                            std::bind(&OtherDeviceUpdateManager::
                                          onActivationChangedMsg,
                                      this, std::placeholders::_1));
                        isImageFileProcessed[uuid] = true;
                        std::weak_ptr<bool> weakAlive = aliveFlag;
                        setDBusPropertyAsync(
                            path,
                            "xyz.openbmc_project.Software.ExtendedVersion",
                            "ExtendedVersion", uuidMappings[uuid].version,
                            [this, weakAlive, path, uuid](bool success) {
                                if (success)
                                {
                                    return;
                                }
                                error(
                                    "Failed to set ExtendedVersion for {PATH} UUID={UUID}",
                                    "PATH", path, "UUID", uuid);
                                if (weakAlive.expired())
                                {
                                    return;
                                }
                                isImageFileProcessed[uuid] = false;
                            });
                        setUpdatePolicy(path, uuid);
                    }
                }
            }
        }
    }
    auto allProcessed = true;
    for (auto& x : isImageFileProcessed)
    {
        if (x.second == false)
        {
            allProcessed = false;
            break;
        }
    }
    if (allProcessed)
    {
        interfaceAddedMatch = nullptr;
        updateManager->updateOtherDeviceComponents(isImageFileProcessed);
    }
}

std::optional<std::pair<UUID, SKU>>
    OtherDeviceUpdateManager::fetchDescriptorsFromPackage(
        const FirmwareDeviceIDRecord& fwDeviceIDRecord)
{
    const auto& deviceIDDescriptors = std::get<Descriptors>(fwDeviceIDRecord);
    UUID uuid{};
    SKU sku{};
    for (const auto& [descriptorType, descriptorValue] :
         deviceIDDescriptors)                 // For each Descriptors
    {
        if (descriptorType == PLDM_FWUP_UUID) // Check UUID
        {
            std::ostringstream tempStream;
            for (int byte : std::get<0>(descriptorValue))
            {
                tempStream << std::setfill('0') << std::setw(2) << std::hex
                           << byte;
            }

            uuid = tempStream.str(); // Extract UUID
            std::transform(uuid.begin(), uuid.end(), uuid.begin(), ::toupper);
        }

        if (descriptorType == PLDM_FWUP_VENDOR_DEFINED) // Check SKU
        {
            const auto& [vendorDescTitle, vendorDescData] =
                std::get<VendorDefinedDescriptorInfo>(descriptorValue);
            if (vendorDescTitle == "APSKU")
            {
                if (vendorDescData.size() == 4)
                {
                    sku = std::format("0X{:02X}{:02X}{:02X}{:02X}",
                                      vendorDescData[0], vendorDescData[1],
                                      vendorDescData[2], vendorDescData[3]);
                }
                else
                {
                    std::string descBytes;
                    for (const auto& byte : vendorDescData)
                    {
                        descBytes += std::format("{:02x}", byte);
                    }
                    error(
                        "APSKU descriptor has invalid size {SIZE} bytes (must be 4 bytes), data: 0x{DATA}",
                        "SIZE", vendorDescData.size(), "DATA", descBytes);
                    return std::nullopt;
                }
            }
        }
    }

    return {{uuid, sku}};
}

TransferPackageState OtherDeviceUpdateManager::txComponentImage(
    const std::string& filePath, const ComponentImageInfo& componentImageInfo,
    std::istream& package)
{
    // Presence of DeadComponent triggers the Debug Token Install during Update
    // This component needs to be skipped since its handled by
    // DebugTokenInstaller
    if (std::get<static_cast<size_t>(ComponentImageInfoPos::CompIdentifierPos)>(
            componentImageInfo) == deadComponent)
    {
        return TransferPackageState::SKIPPED;
    }

    auto compOffset = std::get<5>(componentImageInfo);
    auto compSize = std::get<6>(componentImageInfo);
    package.seekg(0, std::ios::end);
    uintmax_t packageSize = package.tellg();

    // An enhancement designed to safeguard the package against
    // damage in the event of a truncated component. An attempt to
    // read such a component from the package may lead to an effort
    // to read a set of bytes beyond the package's boundaries,
    // triggering the state of the package to be set to
    // std::ios::failbit. This, in turn, could potentially block the
    // ability to read other components from the package.
    if (packageSize <
        static_cast<uintmax_t>(compOffset) + static_cast<uintmax_t>(compSize))
    {
        error("Failed to extract non pldm device component image");
        return TransferPackageState::FAILED;
    }

    package.seekg(compOffset); // SEEK to image offset
    std::vector<uint8_t> buffer(compSize);
    package.read(reinterpret_cast<char*>(buffer.data()), buffer.size());

    const auto& version = std::get<7>(componentImageInfo);
    info("Extracting {VERSION} to filePath : {FILENAME}", "VERSION", version,
         "FILENAME", filePath);

    std::ofstream outfile(filePath, std::ofstream::binary);
    outfile.write(reinterpret_cast<const char*>(&buffer[0]),
                  buffer.size() * sizeof(uint8_t)); // Write to image offset
    outfile.close();
    return TransferPackageState::SUCCESS;
}

TransferPackageState OtherDeviceUpdateManager::txSingleComponent(
    const std::string& dirPath, const ComponentImageInfo& componentImageInfo,
    std::istream& package, const std::string& objPath, const UUID& uuid)
{
    const std::string destinationFilePath =
        dirPath + "/" +
        boost::uuids::to_string(boost::uuids::random_generator()())
            .substr(0, 8);

    const auto& version = std::get<7>(componentImageInfo);
    uuidMappings[uuid] = {version, std::filesystem::path(objPath).filename()};

    return txComponentImage(destinationFilePath, componentImageInfo, package);
}

TransferPackageState OtherDeviceUpdateManager::txMultipleComponents(
    const std::string& dirPath, const ApplicableComponents& applicableCompVec,
    const ComponentImageInfos& componentImageInfos, std::istream& package,
    const std::string& objPath, const UUID& uuid)
{
    for (const auto& component : applicableCompVec)
    {
        const auto& componentImageInfo = componentImageInfos[component];
        const auto compIdentifier = std::get<static_cast<size_t>(
            ComponentImageInfoPos::CompIdentifierPos)>(componentImageInfo);

        const auto& compIdString = std::to_string(compIdentifier);
        std::string destinationDir = dirPath;
        destinationDir += "/" + compIdString;

        const auto transferState = txSingleComponent(
            destinationDir, componentImageInfo, package, objPath, uuid);
        if (transferState == TransferPackageState::FAILED or
            transferState == TransferPackageState::SKIPPED)
        {
            return TransferPackageState::FAILED;
        }
    }
    return TransferPackageState::SUCCESS;
}

exec::task<size_t> OtherDeviceUpdateManager::extractOtherDevicePkgs(
    [[maybe_unused]] const FirmwareDeviceIDRecords& fwDeviceIDRecords,
    [[maybe_unused]] const ComponentImageInfos& componentImageInfos,
    [[maybe_unused]] std::istream& package)
{
#ifndef NON_PLDM
    co_return 0;
#else
    size_t totalNumImages = 0;
    startWatchingInterfaceAddition();
    co_await buildDeviceDescriptorMap();

    for (size_t index = 0; index < fwDeviceIDRecords.size(); ++index)
    {
        const auto& fwDeviceIDRecord = fwDeviceIDRecords[index];

        auto packageDescriptors = fetchDescriptorsFromPackage(fwDeviceIDRecord);
        if (!packageDescriptors)
        {
            continue;
        }

        const auto& [uuid, sku] = *packageDescriptors;
        if (uuid.empty())
        {
            continue;
        }

        auto it = otherDeviceDescriptorMap.find({uuid, sku});
        if (it == otherDeviceDescriptorMap.end())
        {
            continue;
        }
        auto& [directoryName, objPath] = it->second;

        const auto& applicableCompVec =
            std::get<ApplicableComponents>(fwDeviceIDRecord);
        if (applicableCompVec.size() == 0)
        {
            error("Invalid applicable components");
            continue;
        }

        lg2::info("Found Component with UUID {UUID} and SKU {SKU}", "UUID",
                  uuid, "SKU", sku);

        info("Got Non PLDM directory path {DIR} from {OBJPATH}", "DIR",
             directoryName, "OBJPATH", objPath);

        if (applicableCompVec.size() == 1)
        {
            const auto& componentImageInfo =
                componentImageInfos[applicableCompVec[0]];

            const auto transferState = txSingleComponent(
                directoryName, componentImageInfo, package, objPath, uuid);
            if (transferState == TransferPackageState::FAILED)
            {
                co_return 0;
            }
            if (transferState == TransferPackageState::SKIPPED)
            {
                continue;
            }
        }
        else
        {
            const auto transferState = txMultipleComponents(
                directoryName, applicableCompVec, componentImageInfos, package,
                objPath, uuid);
            if (transferState == TransferPackageState::FAILED)
            {
                co_return 0;
            }
        }

        totalNumImages++;
        isImageFileProcessed[uuid] = false;
    }
    // Populate the timeout cache before the synchronous getter is called by
    // UpdateManager::activatePackage.
    [[maybe_unused]] auto _rc2 = co_await populateMaxItemUpdaterTimeoutCache();
    startTimer(totalNumImages * UPDATER_ACTIVATION_WAIT_PER_IMAGE_SEC);
    co_return totalNumImages;
#endif
}

void OtherDeviceUpdateManager::startTimer(int timerExpiryTime)
{
    timer = std::make_unique<sdbusplus::Timer>([this]() {
        if (this->interfaceAddedMatch != nullptr)
        {
            this->interfaceAddedMatch = nullptr;
            //  send update information to update manager
            updateManager->updateOtherDeviceComponents(
                this->isImageFileProcessed);
            for (auto& x : isImageFileProcessed)
            {
                if (x.second == false)
                {
                    error("{PATH} not processed at timeout", "PATH", x.first);
                    // update message registry
                    std::string resolution = "Retry firmware update operation";
                    std::string messageArg0 = "Firmware Update Service";
                    std::string messageArg1 =
                        uuidMappings[x.first].componentName +
                        " firmware update timed out";
                    createLogEntry(resourceErrorDetected, messageArg0,
                                   messageArg1, resolution);
                    updateManager->updateOtherDeviceCompletion(x.first,
                                                               x.second);
                }
            }
        }
    });
    info("Starting Timer to allow item updaters to process images");
    // Give time to add all activations
    timer->start(std::chrono::seconds(timerExpiryTime), false);
}

void OtherDeviceUpdateManager::startWatchingInterfaceAddition()
{
    interfaceAddedMatch = std::make_unique<sdbusplus::bus::match_t>(
        bus, MatchRules::interfacesAdded(SW_PATH_OTHER),
        std::bind(std::mem_fn(&OtherDeviceUpdateManager::interfaceAdded), this,
                  std::placeholders::_1));
}

int OtherDeviceUpdateManager::getNumberOfProcessedImages()
{
#ifndef NON_PLDM
    return 0;
#else
    return isImageFileProcessed.size();
#endif
}

exec::task<void> OtherDeviceUpdateManager::buildDeviceDescriptorMap()
{
    otherDeviceDescriptorMap.clear();

#ifdef NON_PLDM
    const std::string uuidIface{
        sdbusplus::xyz::openbmc_project::Common::server::UUID::interface};
    const std::string skuIface{sdbusplus::xyz::openbmc_project::Inventory::
                                   Decorator::server::SKU::interface};
    const std::string pathIface{
        sdbusplus::xyz::openbmc_project::Common::server::FilePath::interface};
    const std::string searchRoot{"/xyz/openbmc_project/software/other"};
    const std::string uuidProp{"UUID"};
    const std::string skuProp{"SKU"};
    const std::string pathProp{"Path"};
    pldm::dbus::Interfaces uuidOnly{uuidIface};

    try
    {
        auto subtree =
            co_await pldm::utils::coGetSubTree(searchRoot, 0, uuidOnly);

        for (const auto& [objPath, serviceMap] : subtree)
        {
            if (serviceMap.empty())
            {
                continue;
            }
            const std::string serviceName = serviceMap.begin()->first;
            const std::string objPathStr = objPath;

            std::string uuid;
            try
            {
                uuid = co_await pldm::utils::coGetDbusProperty<std::string>(
                    objPathStr, uuidProp, uuidIface, serviceName);
            }
            catch (const std::exception& e)
            {
                error("Failed to get UUID for {PATH} on {SERVICE}: {ERROR}",
                      "PATH", objPathStr, "SERVICE", serviceName, "ERROR", e);
                continue;
            }
            if (uuid.empty())
            {
                continue;
            }
            std::transform(uuid.begin(), uuid.end(), uuid.begin(), ::toupper);

            auto sku = co_await pldm::utils::coGetDbusProperty<std::string>(
                objPathStr, skuProp, skuIface, serviceName);
            std::transform(sku.begin(), sku.end(), sku.begin(), ::toupper);

            auto filePath =
                co_await pldm::utils::coGetDbusProperty<std::string>(
                    objPathStr, pathProp, pathIface, serviceName);
            if (filePath.empty())
            {
                continue;
            }
            auto dirPath =
                std::filesystem::path(filePath).parent_path().string();

            otherDeviceDescriptorMap[{uuid, sku}] = {dirPath, objPathStr};

            if (!sku.empty())
            {
                otherDeviceDescriptorMap[{uuid, ""}] = {dirPath, objPathStr};
            }
        }
    }
    catch (const std::exception& e)
    {
        error("buildDeviceDescriptorMap failed: {ERROR}", "ERROR", e);
    }
#endif
    co_return;
}

size_t OtherDeviceUpdateManager::getValidTargets(void)
{
#ifndef NON_PLDM
    return 0;
#endif
    return validTargetCount;
}

void OtherDeviceUpdateManager::updateValidTargets(void)
{
    // Called synchronously from the constructor — see header comment.
    // The remaining sync reads are bounded to this single call site at
    // startup of an update and intentionally use the sync dbusHandler so
    // the count is available when UpdateManager::processStreamDefer checks
    // getValidTargets() immediately after construction.
    std::vector<std::string> paths;
#ifdef NON_PLDM
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
#endif
    validTargetCount = 0;
    for (auto& obj : paths)
    {
        try
        {
            auto uuid =
                std::get<std::string>(dbusHandler.getDbusPropertyVariant(
                    obj.c_str(), "UUID",
                    sdbusplus::xyz::openbmc_project::Common::server::UUID::
                        interface));
            if (uuid != "")
            {
                validTargetCount++;
            }
        }
        catch (const std::exception& e)
        {
            error(
                "Failed to read UUID property from software D-Bus objects, ERROR={ERROR}",
                "ERROR", e);
        }
    }
}

uint64_t OtherDeviceUpdateManager::getMaxItemUpdaterTimeoutSec() const
{
    return maxItemUpdaterTimeoutCacheSec;
}

exec::task<int> OtherDeviceUpdateManager::populateMaxItemUpdaterTimeoutCache()
{
    maxItemUpdaterTimeoutCacheSec = 0;
#ifdef NON_PLDM
    const std::string updateTimeoutInterface{
        "com.nvidia.Software.UpdateTimeout"};
    const std::string timeoutProperty{"Timeout"};
    pldm::dbus::Interfaces timeoutIfaceList{updateTimeoutInterface};

    try
    {
        for (const auto& kv : otherDevices)
        {
            const std::string path = kv.first;
            auto serviceMap =
                co_await pldm::utils::coGetServiceMap(path, timeoutIfaceList);
            if (serviceMap.empty())
            {
                continue;
            }
            const std::string serviceName = serviceMap.begin()->first;
            auto value = co_await pldm::utils::coGetDbusProperty<uint64_t>(
                path, timeoutProperty, updateTimeoutInterface, serviceName);
            maxItemUpdaterTimeoutCacheSec =
                std::max(maxItemUpdaterTimeoutCacheSec, value);
        }
    }
    catch (const std::exception& e)
    {
        error("populateMaxItemUpdaterTimeoutCache failed: {ERROR}", "ERROR", e);
    }
#endif
    co_return 0;
}

} // namespace fw_update
} // namespace pldm
