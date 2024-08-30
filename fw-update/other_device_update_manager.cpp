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
#include "update_manager.hpp"
#include "watch.hpp"

#include <fmt/format.h>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <phosphor-logging/lg2.hpp>
#include <xyz/openbmc_project/Common/FilePath/server.hpp>
#include <xyz/openbmc_project/Common/UUID/server.hpp>
#include <xyz/openbmc_project/Common/error.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/Asset/server.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <tuple>
#include <unordered_set>
#include <unistd.h>

#define SW_PATH_OTHER "/xyz/openbmc_project/software/other"

namespace pldm
{

namespace fw_update
{

namespace MatchRules = sdbusplus::bus::match::rules;

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

bool OtherDeviceUpdateManager::activate()
{
    bool activationStatus = true;
    for (auto& x : otherDevices)
    {
        auto& path = x.first;

        pldm::utils::DBusMapping dbusMapping{path,
                                             Server::Activation::interface,
                                             "RequestedActivation", "string"};
        lg2::info("Activating : OBJPATH = {PATH}", "PATH", path);
        try
        {
            pldm::utils::DBusHandler().setDbusProperty(
                dbusMapping, std::string(Server::Activation::interface) +
                                 ".RequestedActivations.Active");
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to set resource RequestedActivation : {PATH}."
                       " Error={ERROR}",
                       "PATH", path, "ERROR", e);
            std::string resolution = "Retry firmware update operation";
            std::string messageArg0 = "Firmware Update Service";
            std::string messageArg1 =
                uuidMappings[x.second->uuid].componentName +
                " firmware update timed out";
            createLogEntry(resourceErrorDetected, messageArg0, messageArg1,
                           resolution);
            updateManager->updateOtherDeviceCompletion(x.second->uuid, false);
            activationStatus = false;
        }
    }
    return activationStatus;
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
    std::optional<uint8_t> progress;
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

bool OtherDeviceUpdateManager::setUpdatePolicy(const std::string& path)
{
    pldm::utils::DBusMapping targetsDBusMapping{
        path, "xyz.openbmc_project.Software.UpdatePolicy", "Targets",
        "array[object_path]"};
    try
    {
        pldm::utils::DBusHandler().setDbusProperty(targetsDBusMapping, targets);
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        lg2::error("Failed to set targets : {ERROR}", "ERROR", e);
        // when target filter is specified only selected devices should update
        // return error so that user can retry the update on failed devices
        return false;
    }
    return true;
}

void OtherDeviceUpdateManager::interfaceAdded(sdbusplus::message::message& m)
{
    sdbusplus::message::object_path objPath;
    pldm::dbus::InterfaceMap interfaces;
    m.read(objPath, interfaces);

    std::string path(std::move(objPath));
    if (interfaceAddedMatch == nullptr)
    {
        return;
    }
    for (const auto& intf : interfaces)
    {
        lg2::info("New Interface Added. OBJPATH={PATH}, INTF={INTF}", "PATH",
                  path, "INTF", intf.first);
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
                        // set the version info so that item updater can update
                        // message registry and pass this to concurrent update
                        pldm::utils::DBusMapping dbusMapping{
                            path,
                            "xyz.openbmc_project.Software.ExtendedVersion",
                            "ExtendedVersion", "string"};
                        try
                        {
                            pldm::utils::DBusHandler().setDbusProperty(
                                dbusMapping, uuidMappings[uuid].version);
                        }
                        catch (const sdbusplus::exception::SdBusError& e)
                        {
                            lg2::error(
                                "Failed to set extended version : {ERROR}",
                                "ERROR", e);
                        }
                        if (!setUpdatePolicy(path))
                        {
                            // if update policy D-Bus call fails, mark as image
                            // not processed to log transfer failed at timeout
                            isImageFileProcessed[uuid] = false;
                        }
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

std::pair<UUID, SKU> OtherDeviceUpdateManager::fetchDescriptorsFromPackage(
    const FirmwareDeviceIDRecord& fwDeviceIDRecord)
{
    const auto& deviceIDDescriptors = std::get<Descriptors>(fwDeviceIDRecord);
    UUID uuid{};
    SKU sku{};
    for (const auto& [descriptorType, descriptorValue] :
         deviceIDDescriptors) // For each Descriptors
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
                sku = fmt::format("0x{:02X}{:02X}{:02X}{:02X}",
                                  vendorDescData[0], vendorDescData[1],
                                  vendorDescData[2], vendorDescData[3]);
            }
        }
    }

    return {uuid, sku};
}


TransferPackageState OtherDeviceUpdateManager::txComponentImage(const std::string& filePath, const ComponentImageInfo& componentImageInfo,
        std::istream& package)
{
    // Presence of DeadComponent triggers the Debug Token Install during Update
    // This component needs to be skipped since its handled by DebugTokenInstaller
    if (std::get<static_cast<size_t>(
            ComponentImageInfoPos::CompIdentifierPos)>(
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
    if (packageSize < static_cast<uintmax_t>(compOffset) +
                          static_cast<uintmax_t>(compSize))
    {
        lg2::error(
            "Failed to extract non pldm device component image");
        return TransferPackageState::FAILED;
    }


    package.seekg(compOffset); // SEEK to image offset
    std::vector<uint8_t> buffer(compSize);
    package.read(reinterpret_cast<char*>(buffer.data()),
                 buffer.size());

    const auto& version = std::get<7>(componentImageInfo);
    lg2::info("Extracting {VERSION} to filePath : {FILENAME}",
              "VERSION", version, "FILENAME", filePath);

    std::ofstream outfile(filePath, std::ofstream::binary);
    outfile.write(reinterpret_cast<const char*>(&buffer[0]),
                  buffer.size() *
                      sizeof(uint8_t)); // Write to image offset
    outfile.close();
    return TransferPackageState::SUCCESS;

}


TransferPackageState OtherDeviceUpdateManager::txSingleComponent(const std::string& dirPath, const ComponentImageInfo& componentImageInfo,
        std::istream& package, const std::string& objPath, const UUID& uuid)
{
    const std::string destinationFilePath = dirPath + "/" + boost::uuids::to_string(
                          boost::uuids::random_generator()())
                          .substr(0, 8);

    const auto& version = std::get<7>(componentImageInfo);
    uuidMappings[uuid] = {version, std::filesystem::path(objPath).filename()};

    return txComponentImage(destinationFilePath, componentImageInfo, package);
}

TransferPackageState OtherDeviceUpdateManager::txMultipleComponents(const std::string& dirPath, const ApplicableComponents& applicableCompVec,
        const ComponentImageInfos& componentImageInfos, std::istream& package, const std::string& objPath, const UUID& uuid)
{
    for (const auto& component: applicableCompVec)
    {
        const auto& componentImageInfo =
            componentImageInfos[component];
        const auto compIdentifier =
        std::get<static_cast<size_t>(ComponentImageInfoPos::CompIdentifierPos)>(componentImageInfo);

        const auto& compIdString = std::to_string(compIdentifier);
        const std::string destinationDir = dirPath + "/" + compIdString;

        const auto transferState = txSingleComponent(destinationDir, componentImageInfo, package, objPath, uuid);
        if (transferState == TransferPackageState::FAILED or
                transferState == TransferPackageState::SKIPPED)
        {
            return TransferPackageState::FAILED;
        }
    }
    return TransferPackageState::SUCCESS;
}

size_t OtherDeviceUpdateManager::extractOtherDevicePkgs(
    const FirmwareDeviceIDRecords& fwDeviceIDRecords,
    const ComponentImageInfos& componentImageInfos, std::istream& package)
{
#ifndef NON_PLDM
    return 0;
#else
    size_t totalNumImages = 0;
    startWatchingInterfaceAddition();
    for (size_t index = 0; index < fwDeviceIDRecords.size(); ++index)
    {
        const auto& fwDeviceIDRecord = fwDeviceIDRecords[index];

        const auto& [uuid, sku] = fetchDescriptorsFromPackage(fwDeviceIDRecord);

        if (uuid.empty())
        {
            continue;
        }

        if (sku.empty())
        {
            lg2::warning("No Sku descriptor found in package for UUID {UUID}",
                         "UUID", uuid);
        }

        const auto& applicableCompVec =
            std::get<ApplicableComponents>(fwDeviceIDRecord);
        if (applicableCompVec.size() == 0)
        {
            lg2::error("Invalid applicable components");
            continue;
        }

        auto [directoryName, objPath] = getFilePath(uuid, sku);

        if (directoryName == "")
        {
            continue;
        }

        lg2::info("Found Component with UUID {UUID} and SKU {SKU}", "UUID",
                  uuid, "SKU", sku);

        lg2::info("Got Non PLDM directory path {DIR} from {OBJPATH}",
                "DIR", directoryName, "OBJPATH", objPath);

        if (applicableCompVec.size() == 1)
        {
            const auto& componentImageInfo =
                componentImageInfos[applicableCompVec[0]];

            const auto transferState = txSingleComponent(directoryName, componentImageInfo,
                    package, objPath, uuid);
            if (transferState == TransferPackageState::FAILED)
            {
                return 0;
            }
            if (transferState == TransferPackageState::SKIPPED)
            {
                continue;
            }

        }
        else
        {
            const auto transferState = txMultipleComponents(directoryName, applicableCompVec, componentImageInfos,
                    package, objPath, uuid);
            if (transferState == TransferPackageState::FAILED)
            {
                return 0;
            }
        }

        totalNumImages++;
        isImageFileProcessed[uuid] = false;

    }
    startTimer(totalNumImages * UPDATER_ACTIVATION_WAIT_PER_IMAGE_SEC);
    return totalNumImages;
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
                    lg2::error("{PATH} not processed at timeout", "PATH",
                               x.first);
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
    lg2::info("Starting Timer to allow item updaters to process images");
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

bool OtherDeviceUpdateManager::validateDescriptor(const dbus::ObjectPath& objPath,
        std::string descriptor,
        const char* descriptorName, const char* dbusInterface) const noexcept
{
    static auto dbusHandler = pldm::utils::DBusHandler();
    std::string tmpDescriptor{};
    try
    {
        tmpDescriptor = dbusHandler.getDbusProperty<std::string>(
            objPath.c_str(), descriptorName, dbusInterface);
    }
    catch (const std::exception&)
    {
        lg2::warning(fmt::format("Object {} does not have descriptor {}",
                                 objPath, descriptorName)
                         .c_str());
        return false;
    }

    std::transform(descriptor.begin(), descriptor.end(), descriptor.begin(),
                   ::toupper);
    std::transform(tmpDescriptor.begin(), tmpDescriptor.end(),
                   tmpDescriptor.begin(), ::toupper);
    return (descriptor == tmpDescriptor);
}

std::pair<std::string, std::string>
    OtherDeviceUpdateManager::getFilePath(const UUID& uuid,
            const SKU& packageSKU) const noexcept
{
    std::vector<std::string> paths;
    getValidPaths(paths);

    auto dbusHandler = pldm::utils::DBusHandler();
    for (auto& obj : paths)
    {

        if (!validateDescriptor(obj, uuid, "UUID",
                                sdbusplus::xyz::openbmc_project::Common::
                                    server::UUID::interface))
        {
            continue;
        }

        if (!packageSKU.empty())
        {
            if (validateDescriptor(obj, packageSKU, "SKU",
                                   sdbusplus::xyz::openbmc_project::Inventory::
                                       Decorator::server::Asset::interface))
            {
                lg2::info("Found object {OBJ} with matching SKU {SKU}", "OBJ", obj, "SKU", packageSKU);
            }
            else
            {
                continue;
            }
        }

        std::string path{};
        try
        {
            path = dbusHandler.getDbusProperty<std::string>(
                obj.c_str(), "Path",
                sdbusplus::xyz::openbmc_project::Common::server::FilePath::
                    interface);
        }
        catch (const sdbusplus::exception::SdBusError& e)
        {
            lg2::error("failed to fetch path from D-Bus object. {ERROR}",
                    "ERROR", e.what());
            continue;
        }

        if (!path.empty())
        {
            return {std::filesystem::path(path).parent_path(), obj};
        }
    }
    return {};
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
    std::vector<std::string> paths;
    getValidPaths(paths);
    auto dbusHandler = pldm::utils::DBusHandler();
    validTargetCount = 0;
    for (auto& obj : paths)
    {
        try
        {
            auto uuid = dbusHandler.getDbusProperty<std::string>(
                obj.c_str(), "UUID",
                sdbusplus::xyz::openbmc_project::Common::server::UUID::
                    interface);
            if (uuid != "")
            {
                validTargetCount++;
            }
        }
        catch (const std::exception& e)
        {
            lg2::error(
                "Failed to read UUID property from software D-Bus objects, ERROR={ERROR}",
                "ERROR", e);
        }
    }
}

void OtherDeviceUpdateManager::getValidPaths(std::vector<std::string>& paths) const
{
#ifndef NON_PLDM
    (void)paths; // suppress unused warning
    return;
#endif

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

} // namespace fw_update
} // namespace pldm
