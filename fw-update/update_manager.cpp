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
#include "update_manager.hpp"

#include "activation.hpp"
#include "common/mmap_stream.hpp"
#include "common/utils.hpp"
#include "error_handling.hpp"
#include "package_parser.hpp"
#include "package_signature.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/exception.hpp>

#include <bitset>
#include <cassert>
#include <cmath>
#include <ranges>
#include <set>
#include <string>

PHOSPHOR_LOG2_USING;

namespace pldm
{

namespace fw_update
{

/** @brief Check if package descriptors are a subset of device descriptor
 *
 *  Determines whether a firmware package is applicable to a device by
 *  verifying that every descriptor required by the package exists in the
 *  device's descriptor set. The package descriptors represent the minimum
 *  requirements; the device may have additional descriptors that are not
 *  specified in the package.
 *
 *  Relationship:
 *  @code
 *  +---------------------------------------------+
 *  |           Device Descriptors                |
 *  |  +-------------------------------+          |
 *  |  |    Package Descriptors        |          |
 *  |  |    (must all be present)      |          |
 *  |  +-------------------------------+          |
 *  |         (extra descriptors OK)              |
 *  +---------------------------------------------+
 *
 *  Package ⊆ Device  →  MATCH (returns true)
 *  @endcode
 *
 *  @param[in] deviceDescriptors - Descriptors reported by the device
 *  @param[in] pkgDescriptors - Descriptors from the firmware package
 *  @return true if all package descriptors exist in device descriptors
 */
static bool descriptorsMatch(const Descriptors& deviceDescriptors,
                             const Descriptors& pkgDescriptors)
{
    for (const auto& pkgDesc : pkgDescriptors)
    {
        const auto& pkgDescriptorType = pkgDesc.first;
        const auto& pkgDescriptorValue = pkgDesc.second;
        auto range = deviceDescriptors.equal_range(pkgDescriptorType);
        bool found =
            std::any_of(range.first, range.second,
                        [&pkgDescriptorValue](const auto& deviceDesc) {
                            return deviceDesc.second == pkgDescriptorValue;
                        });
        if (!found)
        {
            return false;
        }
    }
    return true;
}

UpdateManager::UpdateManager(
    Event& event, pldm::requester::Handler<pldm::requester::Request>& handler,
    InstanceIdDb& instanceIdDb, const DescriptorMap& descriptorMap,
    const ComponentInfoMap& componentInfoMap,
    ComponentNameMap& componentNameMap, bool fwDebug,
    RefreshSingleEndpointCallback refreshSingleEndpointCallback) :
    event(event), handler(handler), instanceIdDb(instanceIdDb),
    fwDebug(fwDebug),
    refreshSingleEndpointCallback(std::move(refreshSingleEndpointCallback)),
    descriptorMap(descriptorMap), componentInfoMap(componentInfoMap),
    componentNameMap(componentNameMap),
    updater(
        std::make_unique<Update>(pldm::utils::DBusHandler::getBus(),
                                 "/xyz/openbmc_project/software/pldm", this))
{
    progressTimer = nullptr;
    forceUpdate = false;
}

UpdateManager::~UpdateManager() = default;

std::string UpdateManager::getSwId()
{
    return std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string UpdateManager::getActivationMethod(
    bitfield16_t compActivationModification)
{
    static std::unordered_map<size_t, std::string> compActivationMethod = {
        {PLDM_ACTIVATION_AUTOMATIC, "Automatic"},
        {PLDM_ACTIVATION_SELF_CONTAINED, "Self-Contained"},
        {PLDM_ACTIVATION_MEDIUM_SPECIFIC_RESET, "Medium-specific reset"},
        {PLDM_ACTIVATION_SYSTEM_REBOOT, "System reboot"},
        {PLDM_ACTIVATION_DC_POWER_CYCLE, "DC power cycle"},
        {PLDM_ACTIVATION_AC_POWER_CYCLE, "AC power cycle"}};

    std::string compActivationMethods{};
    std::bitset<16> activationMethods(compActivationModification.value);

    for (std::size_t idx = 0; idx < activationMethods.size(); idx++)
    {
        if (activationMethods.test(idx) && compActivationMethods.empty() &&
            compActivationMethod.contains(idx))
        {
            compActivationMethods += compActivationMethod[idx];
        }
        else if (activationMethods.test(idx) &&
                 !compActivationMethods.empty() &&
                 compActivationMethod.contains(idx))
        {
            compActivationMethods += " or " + compActivationMethod[idx];
        }
    }

    return compActivationMethods;
}

void UpdateManager::createMessageRegistry(
    mctp_eid_t eid, const FirmwareDeviceIDRecord& fwDeviceIDRecord,
    size_t compIndex, const std::string& messageID,
    const std::string& resolution,
    const pldm_firmware_update_commands commandType, const uint8_t errorCode)
{
    if (!parser)
    {
        error("Parser is not initialized. Cannot create message registry.");
        return;
    }
    const auto& compImageInfos = parser->getComponentImageInfos();
    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
    const auto& comp = compImageInfos[applicableComponents[compIndex]];
    CompIdentifier compIdentifier =
        std::get<static_cast<size_t>(ComponentImageInfoPos::CompIdentifierPos)>(
            comp);
    const auto& compVersion =
        std::get<static_cast<size_t>(ComponentImageInfoPos::CompVersionPos)>(
            comp);

    std::string compName;
    if (componentNameMap.contains(eid))
    {
        auto eidSearch = componentNameMap.find(eid);
        const auto& compIdNameInfo = eidSearch->second;
        if (compIdNameInfo.contains(compIdentifier))
        {
            auto compIdSearch = compIdNameInfo.find(compIdentifier);
            compName = compIdSearch->second;
        }
        else
        {
            compName = std::to_string(compIdentifier);
        }
    }
    else
    {
        compName = std::to_string(compIdentifier);
    }

    createLogEntry(messageID, compName, compVersion, resolution);
    if (commandType != 0)
    {
        auto [messageStatus, oemMessageId, oemMessageError,
              oemResolution] = getOemMessage(commandType, errorCode);
        if (messageStatus)
        {
            createMessageRegistryResourceErrors(
                eid, fwDeviceIDRecord, compIndex, oemMessageId, oemMessageError,
                oemResolution);
        }
    }
}

void UpdateManager::createMessageRegistryResourceErrors(
    mctp_eid_t eid, const FirmwareDeviceIDRecord& fwDeviceIDRecord,
    size_t compIndex, const std::string& messageID,
    const std::string& messageError, const std::string& resolution,
    bool overrideSeverity)
{
    if (!parser)
    {
        error(
            "Parser is not initialized. Cannot create message registry for resource errors.");
        return;
    }
    const auto& compImageInfos = parser->getComponentImageInfos();
    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
    const auto& comp = compImageInfos[applicableComponents[compIndex]];
    CompIdentifier compIdentifier =
        std::get<static_cast<size_t>(ComponentImageInfoPos::CompIdentifierPos)>(
            comp);

    std::string compName;
    if (componentNameMap.contains(eid))
    {
        auto eidSearch = componentNameMap.find(eid);
        const auto& compIdNameInfo = eidSearch->second;
        if (compIdNameInfo.contains(compIdentifier))
        {
            auto compIdSearch = compIdNameInfo.find(compIdentifier);
            compName = compIdSearch->second;
        }
        else
        {
            compName = std::to_string(compIdentifier);
        }
    }
    else
    {
        compName = std::to_string(compIdentifier);
    }

    createLogEntry(messageID, compName, messageError, resolution, "FWUpdate",
                   overrideSeverity);
}

void UpdateManager::handleDuplicateDescriptorMatch(
    mctp_eid_t eid, const FirmwareDeviceIDRecord& fwDeviceIDRecord)
{
    const std::string messageError =
        "Multiple Firmware Device ID Records in the package match this"
        " device";
    const std::string resolution =
        "Ensure every Firmware Device ID Record in the package is unique"
        " so the correct firmware version is installed";
    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
    for (size_t compIdx = 0; compIdx < applicableComponents.size(); ++compIdx)
    {
        createMessageRegistryResourceErrors(
            eid, fwDeviceIDRecord, compIdx, resourceErrorDetected, messageError,
            resolution, /*overrideSeverity=*/true);
    }
}

std::string UpdateManager::processStreamDefer(
    std::istream& package, uintmax_t packageSize, bool forceUpdateFlag,
    std::vector<sdbusplus::message::object_path> targets)
{
    auto swId = getSwId();
    objPath = swRootPath + swId;
    forceUpdate = forceUpdateFlag;

    info(
        "Update Parameters: ForceUpdate: {FORCEUPDATE}, ApplyTime: {APPLYTIME}",
        "FORCEUPDATE", forceUpdate, "APPLYTIME",
        sdbusplus::xyz::openbmc_project::Software::server::convertForMessage(
            requestedApplyTime));

    otherDeviceUpdateManager = std::make_unique<OtherDeviceUpdateManager>(
        pldm::utils::DBusHandler::getBus(), this, targets);

    if (!activation)
    {
        activation = std::make_unique<Activation>(
            pldm::utils::DBusHandler::getBus(), objPath,
            software::Activation::Activations::Ready, this);
        activationProgress = std::make_unique<ActivationProgress>(
            pldm::utils::DBusHandler::getBus(), objPath);
    }

    // If no devices discovered, take no action on the package.
    if (!descriptorMap.size() && !otherDeviceUpdateManager->getValidTargets())
    {
        error("No devices found for firmware update");

        std::string compName = "Firmware Update Service";
        std::string messageError = "No Matching Devices";
        std::string resolution =
            "Verify the FW package has devices that are listed in the"
            " Redfish FW Inventory";
        createLogEntry(resourceErrorDetected, compName, messageError,
                       resolution);

        activation->activation(software::Activation::Activations::Failed);
        activationProgress->progress(100);

        return objPath;
    }

    updateDeferHandler = std::make_unique<sdeventplus::source::Defer>(
        event, [this, &package, packageSize,
                targets](sdeventplus::source::EventBase&) {
            // Start processStream coroutine in detached mode
            stdexec::start_detached(
                this->processStream(package, packageSize, targets),
                exec::default_task_context<void>(exec::inline_scheduler{}));
        });

    return objPath;
}

exec::task<void> UpdateManager::processStream(
    std::istream& package, uintmax_t packageSize,
    std::vector<sdbusplus::message::object_path> targets)
{
    startTime = std::chrono::steady_clock::now();

    package.clear();
    package.seekg(0, std::ios::beg);
    if (!package.good())
    {
        error("Package stream is not in a valid state");
        handleInvalidPackageError();
        co_return;
    }

    if (packageSize < sizeof(pldm_package_header_information))
    {
        error(
            "PLDM fw update package length {SIZE} less than the length of the package header information '{PACKAGE_HEADER_INFO_SIZE}'.",
            "SIZE", packageSize, "PACKAGE_HEADER_INFO_SIZE",
            sizeof(pldm_package_header_information));
        handleInvalidPackageError();
        co_return;
    }

    auto* mmapStream = dynamic_cast<pldm::MmapStream*>(&package);

    if (mmapStream != nullptr)
    {
        parser = parsePkgHeader(mmapStream->data(), mmapStream->size());
    }

    if (parser == nullptr)
    {
        error("Invalid PLDM package header information");
        handleInvalidPackageHeaderError();
        parser.reset();
        co_return;
    }

    try
    {
        parser->parse(mmapStream->data(), packageSize);
    }
    catch (const sdbusplus::error::xyz::openbmc_project::software::update::
               InvalidSignature&)
    {
        error("Firmware package checksum validation failed");
        handlePayloadChecksumError();
        parser.reset();
        co_return;
    }
    catch (const std::exception& e)
    {
        error("Invalid PLDM package header, error - {ERROR}", "ERROR", e);
        handleInvalidPackageError();
        parser.reset();
        co_return;
    }

    ComponentTargetList compTargetList =
        getComponentTargetList(componentNameMap, targets);

    if (refreshSingleEndpointCallback && !descriptorMap.empty())
    {
        info("Refreshing firmware inventory for {COUNT} discovered endpoints",
             "COUNT", descriptorMap.size());

        // Copy EIDs to avoid iterator invalidation and from modifications to
        // descriptorMap during refresh
        std::vector<mctp_eid_t> eids;
        eids.reserve(descriptorMap.size());
        for (const auto& [eid, _] : descriptorMap)
        {
            eids.push_back(eid);
        }

        exec::async_scope refreshScope;
        for (const auto& eid : eids)
        {
            bool isTarget = compTargetList.contains(eid);
            refreshScope.spawn(
                stdexec::just() |
                stdexec::let_value([this, eid, isTarget]() -> exec::task<void> {
                    [[maybe_unused]] auto rc =
                        co_await refreshSingleEndpointCallback(eid, isTarget);
                }));
        }
        co_await refreshScope.on_empty();

        info("Firmware inventory refresh completed");
    }

    const auto& compImageInfos = parser->getComponentImageInfos();
    auto deviceUpdaterInfos = associatePkgToDevices(
        parser->getFwDeviceIDRecords(), descriptorMap, compImageInfos,
        compTargetList, targets, fwDeviceIDRecords, totalNumComponentUpdates);

    info("Total Components: {TOTAL_NUM_COMPONENT_UPDATES}",
         "TOTAL_NUM_COMPONENT_UPDATES", totalNumComponentUpdates);

    for (const auto& deviceUpdaterInfo : deviceUpdaterInfos)
    {
        auto& applicableComponents = std::get<ApplicableComponents>(
            fwDeviceIDRecords[deviceUpdaterInfo.second]);
        std::string compIdentifiers;
        for (const auto& index : applicableComponents)
        {
            const auto& compImageInfo = compImageInfos[index];
            CompIdentifier compIdentifier = std::get<static_cast<size_t>(
                ComponentImageInfoPos::CompIdentifierPos)>(compImageInfo);
            if (compIdentifiers.empty())
            {
                compIdentifiers = std::to_string(compIdentifier);
            }
            else
            {
                compIdentifiers += " " + std::to_string(compIdentifier);
            }
        }
        info("eid={EID}, RecordOffset={RECORDOFFSET}, ComponentIdentifiers"
             "={COMPIDENTIFIERS}",
             "EID", deviceUpdaterInfo.first, "RECORDOFFSET",
             deviceUpdaterInfo.second, "COMPIDENTIFIERS", compIdentifiers);
    }

    package.clear();
    package.seekg(0, std::ios::beg);

    // get non-pldm components, add to total component count
    size_t otherDevicesImageCount =
        otherDeviceUpdateManager->extractOtherDevicePkgs(
            parser->getFwDeviceIDRecords(), parser->getComponentImageInfos(),
            package);
    totalNumComponentUpdates += otherDevicesImageCount;

    // Log if no matching devices found (but don't set activation state -
    // startNonPLDMUpdate() will handle that and create message registry)
    if (!deviceUpdaterInfos.size() && !otherDevicesImageCount)
    {
        error(
            "No matching devices found with the PLDM firmware update package");
    }

    package.clear();
    package.seekg(0, std::ios::beg);

    for (const auto& deviceUpdaterInfo : deviceUpdaterInfos)
    {
        const auto& fwDeviceIDRecord =
            fwDeviceIDRecords[deviceUpdaterInfo.second];
        auto search = componentInfoMap.find(deviceUpdaterInfo.first);
        if (search == componentInfoMap.end())
        {
            continue;
        }

        auto compIdNameInfoSearch =
            componentNameMap.find(deviceUpdaterInfo.first);
        ComponentIdNameMap compIdNameInfo{};
        if (compIdNameInfoSearch != componentNameMap.end())
        {
            compIdNameInfo = compIdNameInfoSearch->second;
        }

        deviceUpdaterMap.emplace(
            deviceUpdaterInfo.first,
            std::make_unique<DeviceUpdater>(
                deviceUpdaterInfo.first, package, fwDeviceIDRecord,
                compImageInfos, search->second, compIdNameInfo,
                MAXIMUM_TRANSFER_SIZE, this));
    }

    // delay activation object creation if there are non-pldm updates
    if (otherDevicesImageCount == 0)
    {
        if (activation)
        {
            activation->activation(
                software::Activation::Activations::Activating);
        }
    }
}

void UpdateManager::performSecurityChecksAsync(
    std::function<void(bool)> onComplete,
    [[maybe_unused]] std::function<void(const std::string& errorMsg)> onError)
{
    SecurityCheckType securityCheckType = SecurityCheckType::Disabled;

#ifdef PLDM_PACKAGE_INTEGRITY_CHECK

    securityCheckType = SecurityCheckType::Integrity;
    // Perform integrity check on the firmware package
    packageIntegrityCheckAsync(
        [onComplete](bool integrityCheck) {
            if (integrityCheck)
            {
                info("Firmware package integrity check completed successfully");
                onComplete(true);
            }
            else
            {
                error("Firmware package integrity check failed");
                onComplete(false);
            }
        },
        onError);

#endif

#ifdef PLDM_PACKAGE_VERIFICATION
    securityCheckType = SecurityCheckType::Authentication;
    // Verify the signature of the firmware package
    verifyPackageAsync(
        [onComplete](bool verificationCheck) {
            if (verificationCheck)
            {
                lg2::info(
                    "Firmware package verification completed successfully");
                onComplete(true);
            }
            else
            {
                lg2::error("Firmware package verification failed");
                onComplete(false);
            }
        },
        onError);

#endif

    // Return the overall result of security checks
    if (securityCheckType == SecurityCheckType::Disabled)
    {
        onComplete(true);
    }
}

void UpdateManager::packageIntegrityCheckAsync(
    std::function<void(bool)> onComplete,
    std::function<void(const std::string& errorMsg)> onError)
{
    const static std::string compName = "Firmware Update Service";
    const static std::string messageError =
        "Integrity check failed for FW Package";
    const static std::string messageErrorParseSignatureHeader =
        "Failed to parse FW Package signature header";
    const static std::string resolution =
        "Retry firmware update using a valid package.";

    const auto calcPkgSize = parser->calculatePackageSize();
    std::vector<uint8_t> pkgSignHdrData;

    try
    {
        pkgSignHdrData = PackageSignature::getSignatureHeader(
            updater->getImageStream(), calcPkgSize);
    }
    catch (const std::exception& e)
    {
        error("Failed to get signature header.");
        createLogEntry(resourceErrorDetected, compName, messageError,
                       resolution);
        onComplete(false);
        return;
    }

    if (pkgSignHdrData.size())
    {
        try
        {
            packageSignatureParser =
                PackageSignature::createPackageSignatureParser(pkgSignHdrData);
        }
        catch (const std::exception& e)
        {
            info("Failed to create signature header parser.");

            onComplete(true);
            return;
        }

        try
        {
            packageSignatureParser->parseHeader();
        }
        catch (const std::exception& e)
        {
            error("Failed to parse signature header.", "ERROR", e);
            createLogEntry(resourceErrorDetected, compName,
                           messageErrorParseSignatureHeader, resolution);

            onComplete(false);
            return;
        }

        auto sizeOfSignedData =
            packageSignatureParser->calculateSizeOfSignedData(calcPkgSize);
        packageSignatureParser->integrityCheckAsync(
            updater->getImageStream(), sizeOfSignedData,
            [onComplete](bool integritycheckResult) {
                if (integritycheckResult)
                {
                    info("Integrity check successful for FW Package");
                    onComplete(true);
                }
                else
                {
                    createLogEntry(resourceErrorDetected, compName,
                                   messageError, resolution);
                    onComplete(false);
                }
            },
            onError);

        return;
    }
    else
    {
        info("FW package does not contain signature header");
        onComplete(true);
        return;
    }

    createLogEntry(resourceErrorDetected, compName, messageError, resolution);

    onComplete(false);
}

void UpdateManager::verifyPackageAsync(
    std::function<void(bool)> onComplete,
    std::function<void(const std::string& errorMsg)> onError)
{
    const static std::string compName = "Firmware Update Service";
    const static std::string messageError =
        "Validating FW Package signature failed";
    const static std::string messageErrorParseSignatureHeader =
        "Failed to parse FW Package signature header";
    const static std::string messageErrorUnsupportedVersion =
        "Unsupported version of package signature";
    const static std::string resolution =
        "Retry firmware update operation with correctly signed FW package.";

    uintmax_t calcPkgSize = parser->calculatePackageSize();
    std::vector<uint8_t> pkgSignHdrData;

    try
    {
        pkgSignHdrData = PackageSignature::getSignatureHeader(
            updater->getImageStream(), calcPkgSize);
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to get signature header.");
        createLogEntry(resourceErrorDetected, compName, messageError,
                       resolution);
        onComplete(false);
        return;
    }
    if (pkgSignHdrData.size())
    {
        try
        {
            packageSignatureParser =
                PackageSignature::createPackageSignatureParser(pkgSignHdrData);
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to create signature header parser.");

            createLogEntry(resourceErrorDetected, compName,
                           messageErrorUnsupportedVersion, resolution);
            onComplete(false);

            return;
        }

        try
        {
            packageSignatureParser->parseHeader();
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to parse signature header.", "ERROR", e);
            createLogEntry(resourceErrorDetected, compName,
                           messageErrorParseSignatureHeader, resolution);

            onComplete(false);
            return;
        }

        uintmax_t sizeOfSignedData =
            packageSignatureParser->calculateSizeOfSignedData(calcPkgSize);

        packageSignatureParser->verifyAsync(
            updater->getImageStream(), PLDM_PACKAGE_VERIFICATION_KEY,
            sizeOfSignedData,
            [onComplete](bool verificationCheckResult) {
                if (verificationCheckResult)
                {
                    lg2::info("FW package signature was successfully verified");
                    onComplete(true);
                }
                else
                {
                    createLogEntry(resourceErrorDetected, compName,
                                   messageError, resolution);
                    onComplete(false);
                }
            },
            onError);

        return;
    }
    else
    {
#ifdef PLDM_PACKAGE_VERIFICATION_MUST_BE_SIGNED
        std::string messageErrorNotContainSignatureHeader =
            "Package does not contain signature header";

        createLogEntry(resourceErrorDetected, compName,
                       messageErrorNotContainSignatureHeader, resolution);

        onComplete(false);
#else
        lg2::info("FW package does not contain signature header");
        onComplete(true);
#endif
        return;
    }
}

ComponentTargetList UpdateManager::getComponentTargetList(
    const ComponentNameMap& componentNameMap,
    const std::vector<sdbusplus::message::object_path>& objectPaths)
{
    ComponentTargetList compTargetList{};

    if (!objectPaths.empty())
    {
        auto targets =
            objectPaths | std::views::filter([](std::string path) {
                return path.starts_with("/xyz/openbmc_project/software/");
            }) |
            std::views::transform([](std::string path) {
                return path.substr(path.find_last_of('/') + 1);
            });

        for (const auto& target : targets)
        {
            info("Target={TARGET}", "TARGET", target);
        }

        for (const auto& [eid, componentIdNameMap] : componentNameMap)
        {
            for (const auto& [compIdentifier, compName] : componentIdNameMap)
            {
                if (std::find(targets.begin(), targets.end(), compName) !=
                    targets.end())
                {
                    if (compTargetList.contains(eid))
                    {
                        auto compIdentifiers = compTargetList[eid];
                        compIdentifiers.emplace_back(compIdentifier);
                        compTargetList[eid] = compIdentifiers;
                    }
                    else
                    {
                        compTargetList[eid] = {compIdentifier};
                    }
                }
            }
        }
    }

    return compTargetList;
}

DeviceUpdaterInfos UpdateManager::associatePkgToDevices(
    const FirmwareDeviceIDRecords& inFwDeviceIDRecords,
    const DescriptorMap& descriptorMap,
    const ComponentImageInfos& compImageInfos,
    const ComponentTargetList& compTargetList,
    const std::vector<sdbusplus::message::object_path>& objectPaths,
    FirmwareDeviceIDRecords& outFwDeviceIDRecords,
    TotalComponentUpdates& totalNumComponentUpdates)
{
    DeviceUpdaterInfos deviceUpdaterInfos;
    // Per DSP0267 the first package record whose descriptors match an FD is
    // selected for that FD; later matches are ignored. Track EIDs that have
    // already been associated so progress accounting (totalNumComponentUpdates)
    // stays consistent with the single DeviceUpdater created per EID.
    std::set<mctp_eid_t> matchedEids;
    for (size_t index = 0; index < inFwDeviceIDRecords.size(); ++index)
    {
        const auto& deviceIDDescriptors =
            std::get<Descriptors>(inFwDeviceIDRecords[index]);
        for (const auto& [eid, descriptors] : descriptorMap)
        {
            if (descriptorsMatch(descriptors, deviceIDDescriptors))
            {
                if (!matchedEids.insert(eid).second)
                {
                    bool isTarget =
                        (compTargetList.empty() && objectPaths.empty()) ||
                        compTargetList.contains(eid);
                    if (isTarget)
                    {
                        warning(
                            "Multiple package records match EID={EID}; using first match per spec and ignoring record at index {INDEX}",
                            "EID", eid, "INDEX", index);
                        handleDuplicateDescriptorMatch(
                            eid, inFwDeviceIDRecords[index]);
                    }
                    continue;
                }
                if (compTargetList.empty() && objectPaths.empty())
                {
                    outFwDeviceIDRecords.emplace_back(
                        inFwDeviceIDRecords[index]);
                    auto& applicableComponents = std::get<ApplicableComponents>(
                        outFwDeviceIDRecords.back());
                    deviceUpdaterInfos.emplace_back(
                        std::make_pair(eid, outFwDeviceIDRecords.size() - 1));
                    totalNumComponentUpdates += applicableComponents.size();
                }
                else
                {
                    if (compTargetList.contains(eid))
                    {
                        auto compList = compTargetList.at(eid);
                        auto applicableComponents =
                            std::get<ApplicableComponents>(
                                inFwDeviceIDRecords[index]);

                        std::erase_if(applicableComponents, [&](const auto&
                                                                    idx) {
                            const auto& compImageInfo = compImageInfos[idx];
                            CompIdentifier compIdentifier =
                                std::get<static_cast<size_t>(
                                    ComponentImageInfoPos::CompIdentifierPos)>(
                                    compImageInfo);

                            if (std::find(compList.begin(), compList.end(),
                                          compIdentifier) == compList.end())
                            {
                                lg2::info(
                                    "Component {ID} not found in list - skipping",
                                    "ID", compIdentifier);
                                return true; // Remove this component
                            }
                            return false;    // Keep this component
                        });
                        if (applicableComponents.size())
                        {
                            outFwDeviceIDRecords.emplace_back(
                                inFwDeviceIDRecords[index]);
                            std::get<ApplicableComponents>(
                                outFwDeviceIDRecords.back()) =
                                applicableComponents;
                            deviceUpdaterInfos.emplace_back(std::make_pair(
                                eid, outFwDeviceIDRecords.size() - 1));
                            totalNumComponentUpdates +=
                                applicableComponents.size();
                        }
                    }
                }
            }
        }
    }
    return deviceUpdaterInfos;
}

void UpdateManager::updateDeviceCompletion(
    mctp_eid_t eid, bool status,
    const std::vector<ComponentName>& successCompNames)
{
    const auto [it, inserted] = deviceUpdateCompletionMap.emplace(eid, status);
    if (!inserted)
    {
        warning(
            "Ignoring duplicate device completion update for EID={EID}, existing status={STATUS}",
            "EID", eid, "STATUS", it->second);
        return;
    }

    // Update listCompNames with the components successfully updated
    if (status && !successCompNames.empty())
    {
        for (const auto& compName : successCompNames)
        {
            if (listCompNames.empty())
            {
                listCompNames += compName;
            }
            else
            {
                listCompNames += " " + compName;
            }
        }
    }

    updateActivationProgress();
    /* Update package completion */
    updatePackageCompletion();
    return;
}

Response UpdateManager::handleRequest(mctp_eid_t eid, uint8_t command,
                                      const pldm_msg* request, size_t reqMsgLen)
{
    Response response(sizeof(pldm_msg), 0);
    if (deviceUpdaterMap.contains(eid))
    {
        auto search = deviceUpdaterMap.find(eid);
        if (command == PLDM_REQUEST_FIRMWARE_DATA)
        {
            return search->second->requestFwData(request, reqMsgLen);
        }
        else if (command == PLDM_TRANSFER_COMPLETE)
        {
            return search->second->transferComplete(request, reqMsgLen);
        }
        else if (command == PLDM_VERIFY_COMPLETE)
        {
            return search->second->verifyComplete(request, reqMsgLen);
        }
        else if (command == PLDM_APPLY_COMPLETE)
        {
            return search->second->applyComplete(request, reqMsgLen);
        }
        else
        {
            auto ptr = new (response.data()) pldm_msg;
            auto rc = encode_cc_only_resp(
                request->hdr.instance_id, request->hdr.type,
                request->hdr.command, PLDM_ERROR_INVALID_DATA, ptr);
            assert(rc == PLDM_SUCCESS);
        }
    }
    else
    {
        error(
            "RequestFirmwareData reported PLDM_FWUP_COMMAND_NOT_EXPECTED, eid={EID}",
            "EID", eid);
        auto ptr = new (response.data()) pldm_msg;
        auto rc = encode_cc_only_resp(request->hdr.instance_id,
                                      request->hdr.type, +request->hdr.command,
                                      PLDM_FWUP_COMMAND_NOT_EXPECTED, ptr);
        assert(rc == PLDM_SUCCESS);
    }

    return response;
}

void UpdateManager::onResponseSendComplete(mctp_eid_t eid, bool success)
{
    if (deviceUpdaterMap.contains(eid))
    {
        deviceUpdaterMap[eid]->onResponseSendComplete(success);
    }
}

software::Activation::Activations UpdateManager::activatePackage()
{
    namespace software = sdbusplus::xyz::openbmc_project::Software::server;
    createProgressUpdateTimer();
    progressTimer->start(std::chrono::minutes(PROGRESS_UPDATE_INTERVAL), true);
    activationBlocksTransition = std::make_unique<ActivationBlocksTransition>(
        pldm::utils::DBusHandler::getBus(), objPath);
#ifdef DEBUG_TOKEN
    debugToken =
        std::make_unique<DebugToken>(pldm::utils::DBusHandler::getBus(), this);
    debugToken->updateDebugToken(parser->getFwDeviceIDRecords(),
                                 parser->getComponentImageInfos(),
                                 updater->getImageStream());
    return software::Activation::Activations::Activating;
#endif
    startPLDMUpdate();
    auto nonPLDMState = startNonPLDMUpdate();
    if (nonPLDMState == software::Activation::Activations::Failed ||
        nonPLDMState == software::Activation::Activations::Active)
    {
        return nonPLDMState;
    }
    return software::Activation::Activations::Activating;
}

void UpdateManager::startPLDMUpdate()
{
    for (const auto& [eid, deviceUpdaterPtr] : deviceUpdaterMap)
    {
        const auto& applicableComponents =
            std::get<ApplicableComponents>(deviceUpdaterPtr->fwDeviceIDRecord);
        for (size_t compIndex = 0; compIndex < applicableComponents.size();
             compIndex++)
        {
            createMessageRegistry(eid, deviceUpdaterPtr->fwDeviceIDRecord,
                                  compIndex, targetDetermined);
        }
        deviceUpdaterPtr->startFwUpdateFlow();
    }
}

software::Activation::Activations UpdateManager::startNonPLDMUpdate()
{
    // In case no device found set activation stage to active to complete
    // task.
    if ((deviceUpdaterMap.size() == 0) &&
        (otherDeviceUpdateManager->getNumberOfProcessedImages() == 0))
    {
        info("Nothing to activate, Setting Activations state to Active!");
        if (activationProgress == nullptr)
        {
            activationProgress = std::make_unique<ActivationProgress>(
                pldm::utils::DBusHandler::getBus(), objPath);
        }
        progressTimer->stop();
        progressTimer.reset();
        activationProgress->progress(100);
#ifdef OEM_NVIDIA
        if (debugToken->isDebugTokenComponentPresent() &&
            parser->getComponentImageInfos().size() == 1)
        {
            activationBlocksTransition.reset();
            clearFirmwareUpdatePackage();
            return software::Activation::Activations::Active;
        }
#endif
        std::string compName = "Firmware Update Service";
        std::string messageError = "No Matching Devices";
        std::string resolution =
            "Verify the FW package has devices that are listed in the"
            " Redfish FW Inventory";
        createLogEntry(resourceErrorDetected, compName, messageError,
                       resolution);
        activationBlocksTransition.reset();
        clearFirmwareUpdatePackage();
        return software::Activation::Activations::Failed;
    }
    if (!otherDeviceUpdateManager->activate())
    {
        if (deviceUpdaterMap.size() == 0)
        {
            return software::Activation::Activations::Failed;
        }
    }
    return software::Activation::Activations::Activating;
}

void UpdateManager::clearActivationInfo()
{
    activation.reset();
    activationProgress.reset();
    activationBlocksTransition.reset();
    objPath.clear();
    fwDeviceIDRecords.clear();

    deviceUpdaterMap.clear();
    deviceUpdateCompletionMap.clear();
    parser.reset();
    clearFirmwareUpdatePackage();
    totalNumComponentUpdates = 0;
    compUpdateCompletedCount = 0;
    otherDeviceUpdateManager.reset();
    otherDeviceComponents.clear();
    otherDeviceCompleted.clear();
    listCompNames.clear();
    if (progressTimer)
    {
        progressTimer->stop();
    }
    progressTimer.reset();
}

void UpdateManager::updatePackageCompletion()
{
    namespace software = sdbusplus::xyz::openbmc_project::Software::server;
    auto pldmState = checkUpdateCompletionMap(deviceUpdaterMap.size(),
                                              deviceUpdateCompletionMap);
    auto otherState = checkUpdateCompletionMap(otherDeviceComponents.size(),
                                               otherDeviceCompleted);

    if ((pldmState != software::Activation::Activations::Activating) &&
        (otherState != software::Activation::Activations::Activating))
    {
        // If atleast one component(PLDM or non-PLDM) succeeded, log
        // Update.1.0.AwaitToActivate to the Default namespace as summary with
        // the list of components updated.
        if (!listCompNames.empty())
        {
            createLogEntry(
                awaitToActivate, listCompNames, parser->pkgVersion,
                "Perform the requested action to advance the update operation.",
                "");
        }

        if ((pldmState == software::Activation::Activations::Failed) ||
            (otherState == software::Activation::Activations::Failed))
        {
            activation->activation(software::Activation::Activations::Failed);
        }
        else
        {
            activation->activation(software::Activation::Activations::Active);
        }
        auto endTime = std::chrono::steady_clock::now();
        info("Firmware update time: {UPDATE_TIME} ms", "UPDATE_TIME",
             std::chrono::duration<double, std::milli>(endTime - startTime)
                 .count());
        activationBlocksTransition.reset();
        clearFirmwareUpdatePackage();
    }
}

void UpdateManager::updateActivationProgress()
{
    compUpdateCompletedCount++;
    if (compUpdateCompletedCount == totalNumComponentUpdates)
    {
        if (progressTimer)
        {
            progressTimer->stop();
            progressTimer.reset();
        }
        activationProgress->progress(100);
    }
}

void UpdateManager::updateOtherDeviceComponents(
    std::unordered_map<std::string, bool>& otherDeviceMap)
{
    /* run through the map, if any failed we need to trigger a failure,
       otherwise create the activation object */
    for (const auto& [uuid, success] : otherDeviceMap)
    {
        if (!success)
        {
            error("Other device manager failed to get {UUID} ready", "UUID",
                  uuid);
            /* report the error, but continue on */
        }
    }
    /* as long as there is an other device, create the activation object
       otherwise it will have already been done */
    if (otherDeviceMap.size() > 0)
    {
        otherDeviceComponents = otherDeviceMap;
        if (activation)
        {
            activation->activation(
                software::Activation::Activations::Activating);
        }
    }
}

void UpdateManager::updateOtherDeviceCompletion(
    std::string uuid, bool status, const ComponentName& successCompName)
{
    /* update completion status map */
    if (otherDeviceCompleted.find(uuid) == otherDeviceCompleted.end())
    {
        otherDeviceCompleted.emplace(uuid, status);

        if (status && !successCompName.empty())
        {
            if (listCompNames.empty())
            {
                listCompNames += successCompName;
            }
            else
            {
                listCompNames += " " + successCompName;
            }
        }
        updateActivationProgress();
        updatePackageCompletion();
    }
}

void UpdateManager::resetActivationBlocksTransition()
{
    activationBlocksTransition.reset();
}

void UpdateManager::clearFirmwareUpdatePackage()
{
    if (updater)
    {
        updater->clearImageStream();
    }
}

void UpdateManager::setActivationStatus(
    const software::Activation::Activations& state)
{
    if (!activation)
    {
        error("Activation object is not initialized");
        return;
    }
    activation->activation(state);
}

void UpdateManager::clearExistingActivation()
{
    if (activation)
    {
        if (activation->activation() ==
            software::Activation::Activations::Activating)
        {
            error(
                "Activation of package already in progress, clearing the current activation");
        }
        clearActivationInfo();
    }
}

ComponentName UpdateManager::getComponentName(
    mctp_eid_t eid, const FirmwareDeviceIDRecord& fwDeviceIDRecord,
    size_t compIndex)
{
    const auto& compImageInfos = parser->getComponentImageInfos();
    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
    const auto& comp = compImageInfos[applicableComponents[compIndex]];
    CompIdentifier compIdentifier =
        std::get<static_cast<size_t>(ComponentImageInfoPos::CompIdentifierPos)>(
            comp);
    std::string compName{};
    if (componentNameMap.contains(eid))
    {
        auto eidSearch = componentNameMap.find(eid);
        const auto& compIdNameInfo = eidSearch->second;
        if (compIdNameInfo.contains(compIdentifier))
        {
            auto compIdSearch = compIdNameInfo.find(compIdentifier);
            compName = compIdSearch->second;
        }
    }
    return compName;
}

void UpdateManager::createProgressUpdateTimer()
{
    updateInterval = 0;
    progressTimer = std::make_unique<sdbusplus::Timer>([this]() {
        updateInterval += 1;
        // Cancel in-progress updates when firmware update time is reached
        // percent update should always be less than 100 when task is
        // aborted/cancelled. Setting to 100 percent will cause redfish task
        // service to show running and 100 percent
        if (updateInterval == totalInterval)
        {
            error("Firmware update timeout - cancelling in-progress updates");
            progressTimer->stop();
            cancelAllUpdates();
            return;
        }
        auto progressPercent = static_cast<uint8_t>(
            std::floor((100 * updateInterval) / totalInterval));
        info("Progress Percent: {PROGRESSPERCENT}", "PROGRESSPERCENT",
             progressPercent);
        activationProgress->progress(progressPercent);
        return;
    });
}

void UpdateManager::cancelAllUpdates()
{
    for (const auto& [eid, deviceUpdaterPtr] : deviceUpdaterMap)
    {
        if (!deviceUpdaterPtr || deviceUpdateCompletionMap.contains(eid))
        {
            continue;
        }

        deviceUpdaterPtr->handleUpdateTimeout();
    }
}

void UpdateManager::handleInvalidPackageError()
{
    std::string compName = "Firmware Update Service";
    std::string messageError = "Invalid FW Package";
    std::string resolution =
        "Retry firmware update operation with valid FW package.";
    createLogEntry(resourceErrorDetected, compName, messageError, resolution);
    clearFirmwareUpdatePackage();

    if (activation)
    {
        activation->activation(software::Activation::Activations::Failed);
    }
    else
    {
        activation = std::make_unique<Activation>(
            pldm::utils::DBusHandler::getBus(), objPath,
            software::Activation::Activations::Failed, this);
    }
}

void UpdateManager::handlePayloadChecksumError()
{
    std::string compName = "Firmware Update Service";
    std::string messageError = "FW package payload checksum validation failed";
    std::string resolution =
        "Retry firmware update operation with valid FW package.";
    createLogEntry(resourceErrorDetected, compName, messageError, resolution);
    clearFirmwareUpdatePackage();

    if (activation)
    {
        activation->activation(software::Activation::Activations::Failed);
    }
    else
    {
        activation = std::make_unique<Activation>(
            pldm::utils::DBusHandler::getBus(), objPath,
            software::Activation::Activations::Failed, this);
    }
}

void UpdateManager::handleInvalidPackageHeaderError()
{
    std::string compName = "Firmware Update Service";
    std::string messageError = "Invalid FW Package header";
    std::string resolution =
        "Retry firmware update operation with valid FW package.";
    createLogEntry(resourceErrorDetected, compName, messageError, resolution);
    clearFirmwareUpdatePackage();

    if (activation)
    {
        activation->activation(software::Activation::Activations::Failed);
    }
    else
    {
        activation = std::make_unique<Activation>(
            pldm::utils::DBusHandler::getBus(), objPath,
            software::Activation::Activations::Failed, this);
    }
}

} // namespace fw_update

} // namespace pldm
