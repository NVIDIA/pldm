#include "update_manager.hpp"

#include "activation.hpp"
#include "common/utils.hpp"
#include "package_parser.hpp"
#include "package_signature.hpp"

#include <phosphor-logging/lg2.hpp>

#include <bitset>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>

namespace pldm
{

namespace fw_update
{

namespace fs = std::filesystem;

UpdateManager::UpdateManager(
    Event& event, pldm::requester::Handler<pldm::requester::Request>& handler,
    Requester& requester, const DescriptorMap& descriptorMap,
    const ComponentInfoMap& componentInfoMap,
    ComponentNameMap& componentNameMap, bool fwDebug) :
    event(event),
    handler(handler), requester(requester), fwDebug(fwDebug),
    descriptorMap(descriptorMap), componentInfoMap(componentInfoMap),
    componentNameMap(componentNameMap),
    watch(event.get(), std::bind_front(&UpdateManager::processPackage, this),
          std::bind_front(&UpdateManager::processStagedPackage, this))
{
    updatePolicy = std::make_unique<UpdatePolicy>(
        pldm::utils::DBusHandler::getBus(), "/xyz/openbmc_project/software");

    if (std::filesystem::exists(FIRMWARE_PACKAGE_STAGING_DIR))
    {
        for (const auto& entry :
             std::filesystem::directory_iterator(FIRMWARE_PACKAGE_STAGING_DIR))
        {
            if (entry.is_directory() &&
                entry.path() ==
                    (std::string{FIRMWARE_PACKAGE_STAGING_DIR} + "/pldm"))
            {
                // skip removing pldm directory which contains non-pldm image
                // directories.
            }
            else
            {
                std::filesystem::remove_all(entry.path());
            }
        }
    }

    // initiate object paths for staged image
    if (std::filesystem::exists(FIRMWARE_PACKAGE_SPLIT_STAGING_DIR))
    {
        for (const auto& entry : std::filesystem::directory_iterator(
                 FIRMWARE_PACKAGE_SPLIT_STAGING_DIR))
        {
            if (!(entry.is_directory()))
            {
                if (processStagedPackage(entry.path()) == 0)
                {
                    lg2::info(
                        "Objects creation success for staged image: {IMAGE_PATH}",
                        "IMAGE_PATH", entry.path());
                    break; // only one image supported
                }
                else
                {
                    lg2::error(
                        "Objects creation failed for staged image: {IMAGE_PATH}",
                        "IMAGE_PATH", entry.path());
                }
            }
        }
    }
    progressTimer = nullptr;
}

UpdateManager::~UpdateManager() = default;

std::string
    UpdateManager::getActivationMethod(bitfield16_t compActivationModification)
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
        auto [messageStatus, oemMessageId, oemMessageError, oemResolution] =
            getOemMessage(commandType, errorCode);
        if (messageStatus)
        {
            createMessageRegistryResourceErrors(eid, fwDeviceIDRecord,
                                                compIndex, oemMessageId,
                                                oemMessageError, oemResolution);
        }
    }
}

void UpdateManager::createMessageRegistryResourceErrors(
    mctp_eid_t eid, const FirmwareDeviceIDRecord& fwDeviceIDRecord,
    size_t compIndex, const std::string& messageID,
    const std::string& messageError, const std::string& resolution)
{
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

    createLogEntry(messageID, compName, messageError, resolution);
}

int UpdateManager::processPackage(const std::filesystem::path& packageFilePath)
{
    startTime = std::chrono::steady_clock::now();
    if (activation)
    {
        if (activation->activation() ==
            software::Activation::Activations::Activating)
        {
            lg2::error(
                "Activation of package already in progress, PACKAGE_VERSION={PACKAGE_VERSION}, clearing the current activation",
                "PACKAGE_VERSION", parser->pkgVersion);
        }
        clearActivationInfo();
    }
    else
    {
        // clear for staged packages
        clearActivationInfo();
    }

    namespace software = sdbusplus::xyz::openbmc_project::Software::server;
    std::vector<sdbusplus::message::object_path> targets;
    if (packageFilePath == stagedfwPackageFilePath)
    {
        objPath = stagedObjPath;
        activation = std::move(activationStaged);
        activationProgress = std::move(activationProgressStaged);
        activationProgress->progress(0);
        targets = updatePolicyStaged->targets();
    }
    else
    {
        size_t versionHash = std::hash<std::string>{}(packageFilePath);
        objPath = swRootPath + std::to_string(versionHash);
        targets = updatePolicy->targets();
    }
    fwPackageFilePath = packageFilePath;

    // create the device updater
    otherDeviceUpdateManager = std::make_unique<OtherDeviceUpdateManager>(
        pldm::utils::DBusHandler::getBus(), this, targets);

    // If no devices discovered, take no action on the package.
    if (!descriptorMap.size() && !otherDeviceUpdateManager->getValidTargets())
    {
        lg2::error("No devices found for firmware update");
        if (activation)
        {
            activation->activation(software::Activation::Activations::Ready);
        }
        else
        {
            activation = std::make_unique<Activation>(
                pldm::utils::DBusHandler::getBus(), objPath,
                software::Activation::Activations::Ready, this);
        }
        return 0;
    }

    package.open(packageFilePath,
                 std::ios::binary | std::ios::in | std::ios::ate);
    if (!package.good())
    {
        lg2::error(
            "Opening the PLDM FW update package failed, ERR={ERRNO}, PACKAGEFILEPATH={PACKAGEFILEPATH}",
            "ERRNO", strerror(errno), "PACKAGEFILEPATH", packageFilePath);
        if (activation)
        {
            activation->activation(software::Activation::Activations::Invalid);
        }
        else
        {
            activation = std::make_unique<Activation>(
                pldm::utils::DBusHandler::getBus(), objPath,
                software::Activation::Activations::Invalid, this);
        }
        return -1;
    }

    uintmax_t packageSize = package.tellg();
    if (packageSize < sizeof(pldm_package_header_information))
    {
        lg2::error(
            "PLDM FW update package length less than the length of the package"
            " header information, PACKAGESIZE={PACKAGESIZE}",
            "PACKAGESIZE", packageSize);
        if (activation)
        {
            activation->activation(software::Activation::Activations::Invalid);
        }
        else
        {
            activation = std::make_unique<Activation>(
                pldm::utils::DBusHandler::getBus(), objPath,
                software::Activation::Activations::Invalid, this);
        }
        return -1;
    }

    package.seekg(0);
    std::vector<uint8_t> packageHeader(sizeof(pldm_package_header_information));
    package.read(reinterpret_cast<char*>(packageHeader.data()),
                 sizeof(pldm_package_header_information));

    auto pkgHeaderInfo =
        reinterpret_cast<const pldm_package_header_information*>(
            packageHeader.data());
    auto pkgHeaderInfoSize = sizeof(pldm_package_header_information) +
                             pkgHeaderInfo->package_version_string_length;
    packageHeader.clear();
    packageHeader.resize(pkgHeaderInfoSize);
    package.seekg(0);
    package.read(reinterpret_cast<char*>(packageHeader.data()),
                 pkgHeaderInfoSize);

    parser = parsePkgHeader(packageHeader);
    if (parser == nullptr)
    {
        lg2::error("Invalid PLDM package header information");
        if (activation)
        {
            activation->activation(software::Activation::Activations::Invalid);
        }
        else
        {
            activation = std::make_unique<Activation>(
                pldm::utils::DBusHandler::getBus(), objPath,
                software::Activation::Activations::Invalid, this);
        }
        return -1;
    }

    package.seekg(0);
    packageHeader.resize(parser->pkgHeaderSize);
    package.read(reinterpret_cast<char*>(packageHeader.data()),
                 parser->pkgHeaderSize);
    try
    {
        parser->parse(packageHeader, packageSize);
    }
    catch (const std::exception& e)
    {
        lg2::error("Invalid PLDM package header");
        if (activation)
        {
            activation->activation(software::Activation::Activations::Invalid);
        }
        else
        {
            activation = std::make_unique<Activation>(
                pldm::utils::DBusHandler::getBus(), objPath,
                software::Activation::Activations::Invalid, this);
        }
        return -1;
    }

#ifdef PLDM_PACKAGE_VERIFICATION

    try
    {
        if (!verifyPackage())
        {
            if (activation)
            {
                activation->activation(
                    software::Activation::Activations::Failed);
            }
            else
            {
                activation = std::make_unique<Activation>(
                    pldm::utils::DBusHandler::getBus(), objPath,
                    software::Activation::Activations::Failed, this);
            }
            return -1;
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Invalid PLDM package signature");
        if (activation)
        {
            activation->activation(software::Activation::Activations::Invalid);
        }
        else
        {
            activation = std::make_unique<Activation>(
                pldm::utils::DBusHandler::getBus(), objPath,
                software::Activation::Activations::Invalid, this);
        }
        return -1;
    }

#endif

    const auto& compImageInfos = parser->getComponentImageInfos();
    auto deviceUpdaterInfos = associatePkgToDevices(
        parser->getFwDeviceIDRecords(), descriptorMap, compImageInfos,
        componentNameMap, targets, fwDeviceIDRecords, totalNumComponentUpdates);

    lg2::info("Total Components: {TOTAL_NUM_COMPONENT_UPDATES}",
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
        lg2::info("EID={EID}, RecordOffset={RECORDOFFSET}, ComponentIdentifiers"
                  "={COMPIDENTIFIERS}",
                  "EID", deviceUpdaterInfo.first, "RECORDOFFSET",
                  deviceUpdaterInfo.second, "COMPIDENTIFIERS", compIdentifiers);
    }

    // get non-pldm components, add to total component count
    size_t otherDevicesImageCount =
        otherDeviceUpdateManager->extractOtherDevicePkgs(
            parser->getFwDeviceIDRecords(), parser->getComponentImageInfos(),
            package);
    totalNumComponentUpdates += otherDevicesImageCount;

    if (!deviceUpdaterInfos.size() && !otherDevicesImageCount)
    {
        lg2::error(
            "No matching devices found with the PLDM firmware update package");
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
        return 0;
    }

    for (const auto& deviceUpdaterInfo : deviceUpdaterInfos)
    {
        const auto& fwDeviceIDRecord =
            fwDeviceIDRecords[deviceUpdaterInfo.second];
        auto search = componentInfoMap.find(deviceUpdaterInfo.first);
        auto compIdNameInfoSearch =
            componentNameMap.find(deviceUpdaterInfo.first);
        deviceUpdaterMap.emplace(
            deviceUpdaterInfo.first,
            std::make_unique<DeviceUpdater>(
                deviceUpdaterInfo.first, package, fwDeviceIDRecord,
                compImageInfos, search->second, compIdNameInfoSearch->second,
                MAXIMUM_TRANSFER_SIZE, this, fwDebug));
    }

    // delay activation object creation if there are non-pldm updates
    if (otherDevicesImageCount == 0)
    {
        createActivationObject();
    }
    return 0;
}


bool UpdateManager::verifyPackage()
{
    std::string compName = "Firmware Update Service";
    std::string messageError = "Validating FW Package signature failed";
    std::string messageErrorUnsupportedVersion =
        "Unsupported version of package signature";
    std::string resolution =
        "Retry firmware update operation with correctly signed FW package.";

    uintmax_t calcPkgSize = parser->calculatePackageSize();

    auto pkgSignHdrData =
        PackageSignature::getSignatureHeader(package, calcPkgSize);

    if (pkgSignHdrData.size())
    {

        std::unique_ptr<PackageSignature> packageSignatureParser;

        try
        {
            packageSignatureParser =
                PackageSignature::createPackageSignatureParser(
                    pkgSignHdrData, PLDM_PACKAGE_VERIFICATION_KEY);

            if (packageSignatureParser == nullptr)
            {
                createLogEntry(resourceErrorDetected, compName,
                               messageErrorUnsupportedVersion, resolution);

                return false;
            }
        }
        catch (const std::exception& e)
        {
            createLogEntry(resourceErrorDetected, compName,
                           messageErrorUnsupportedVersion, resolution);

            return false;
        }

        try
        {
            packageSignatureParser->parseHeader();
        }
        catch (const std::exception& e)
        {
            createLogEntry(resourceErrorDetected, compName, messageError,
                           resolution);

            return false;
        }

        uintmax_t sizeOfSignedData =
            packageSignatureParser->calculateSizeOfSignedData(calcPkgSize);

        bool isSignedProperly =
            packageSignatureParser->verify(package, sizeOfSignedData);

        if (!isSignedProperly)
        {
            createLogEntry(resourceErrorDetected, compName, messageError,
                           resolution);

            return false;
        }

        lg2::info("FW package signature was successfully verified");
    }
    else
    {
        lg2::info("FW package does not contain signature header");
    }

    return true;
}


DeviceUpdaterInfos UpdateManager::associatePkgToDevices(
    const FirmwareDeviceIDRecords& inFwDeviceIDRecords,
    const DescriptorMap& descriptorMap,
    const ComponentImageInfos& compImageInfos,
    const ComponentNameMap& componentNameMap,
    const std::vector<sdbusplus::message::object_path>& objectPaths,
    FirmwareDeviceIDRecords& outFwDeviceIDRecords,
    TotalComponentUpdates& totalNumComponentUpdates)
{
    using ComponentTargetList =
        std::unordered_map<EID, std::vector<CompIdentifier>>;
    ComponentTargetList compTargetList{};

    // Process target filtering
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
            lg2::info("Target={TARGET}", "TARGET", target);
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

    DeviceUpdaterInfos deviceUpdaterInfos;
    for (size_t index = 0; index < inFwDeviceIDRecords.size(); ++index)
    {
        const auto& deviceIDDescriptors =
            std::get<Descriptors>(inFwDeviceIDRecords[index]);
        for (const auto& [eid, descriptors] : descriptorMap)
        {
            if (std::includes(descriptors.begin(), descriptors.end(),
                              deviceIDDescriptors.begin(),
                              deviceIDDescriptors.end()))
            {
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
                        auto compList = compTargetList[eid];
                        auto applicableComponents =
                            std::get<ApplicableComponents>(
                                inFwDeviceIDRecords[index]);
                        for (const auto& index : applicableComponents)
                        {
                            const auto& compImageInfo = compImageInfos[index];
                            CompIdentifier compIdentifier =
                                std::get<static_cast<size_t>(
                                    ComponentImageInfoPos::CompIdentifierPos)>(
                                    compImageInfo);
                            if ((std::find(compList.begin(), compList.end(),
                                           compIdentifier) == compList.end()))
                            {
                                std::erase(applicableComponents, index);
                            }
                        }
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

    /* update completion map */
    deviceUpdateCompletionMap.emplace(eid, status);

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
            auto ptr = reinterpret_cast<pldm_msg*>(response.data());
            auto rc = encode_cc_only_resp(
                request->hdr.instance_id, request->hdr.type,
                request->hdr.command, PLDM_ERROR_INVALID_DATA, ptr);
            assert(rc == PLDM_SUCCESS);
        }
    }
    else
    {
        lg2::error(
            "RequestFirmwareData reported PLDM_FWUP_COMMAND_NOT_EXPECTED, EID={EID}",
            "EID", eid);
        auto ptr = reinterpret_cast<pldm_msg*>(response.data());
        auto rc = encode_cc_only_resp(request->hdr.instance_id,
                                      request->hdr.type, +request->hdr.command,
                                      PLDM_FWUP_COMMAND_NOT_EXPECTED, ptr);
        assert(rc == PLDM_SUCCESS);
    }

    return response;
}

software::Activation::Activations UpdateManager::activatePackage()
{
    namespace software = sdbusplus::xyz::openbmc_project::Software::server;
    createProgressUpdateTimer();
    progressTimer->start(std::chrono::minutes(PROGRESS_UPDATE_INTERVAL), true);
    activationBlocksTransition = std::make_unique<ActivationBlocksTransition>(
        pldm::utils::DBusHandler::getBus(), objPath, this);
#ifdef OEM_NVIDIA
    debugToken =
        std::make_unique<DebugToken>(pldm::utils::DBusHandler::getBus(), this);
    debugToken->updateDebugToken(parser->getFwDeviceIDRecords(),
                                 parser->getComponentImageInfos(), package);
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
        lg2::info("Nothing to activate, Setting Activations state to Active!");
        if (activationProgress == nullptr)
        {
            activationProgress = std::make_unique<ActivationProgress>(
                pldm::utils::DBusHandler::getBus(), objPath);
        }
        progressTimer->stop();
        progressTimer.reset();
        activationProgress->progress(100);
        std::string compName = "Firmware Update Service";
        std::string messageError = "No Matching Devices";
        std::string resolution =
            "Verify the FW package has devices that are listed in the"
            " Redfish FW Inventory";
        createLogEntry(resourceErrorDetected, compName, messageError,
                       resolution);
        activationBlocksTransition.reset();
        clearFirmwareUpdatePackage();
        return software::Activation::Activations::Active;
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
    package.close();
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

bool UpdateManager::createActivationObject()
{
    if (deviceUpdaterMap.size() ||
        otherDeviceUpdateManager->getNumberOfProcessedImages())
    {
        try
        {
            namespace software =
                sdbusplus::xyz::openbmc_project::Software::server;
            if (activation == nullptr)
            {
                activation = std::make_unique<Activation>(
                    pldm::utils::DBusHandler::getBus(), objPath,
                    software::Activation::Activations::Ready, this);
            }
            if (activationProgress == nullptr)
            {
                activationProgress = std::make_unique<ActivationProgress>(
                    pldm::utils::DBusHandler::getBus(), objPath);
            }
        }
        catch (const sdbusplus::exception::SdBusError& e)
        {
            return false;
        }
    }
    return true;
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
        lg2::info("Firmware update time: {UPDATE_TIME} ms", "UPDATE_TIME",
                  std::chrono::duration<double, std::milli>(endTime - startTime)
                      .count());
        activationBlocksTransition.reset();
        if (fwPackageFilePath == stagedfwPackageFilePath)
        {
            // set requested activation to none to support b2b updates
            // activation->requestedActivation(
            //     software::Activation::RequestedActivations::None);
            // targets should be cleared to avoid previous targets getting
            // re-used in b2b updates
            updatePolicyStaged->targets({});
            restoreStagedPackageActivationObjects();
        }
        clearFirmwareUpdatePackage();
    }
}

void UpdateManager::updateActivationProgress()
{
    compUpdateCompletedCount++;
    if (compUpdateCompletedCount == totalNumComponentUpdates)
    {
        progressTimer->stop();
        progressTimer.reset();
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
            lg2::error("Other device manager failed to get {UUID} ready",
                       "UUID", uuid);
            /* report the error, but continue on */
        }
    }
    /* as long as there is an other device, create the activation object
       otherwise it will have already been done */
    if (otherDeviceMap.size() > 0)
    {
        otherDeviceComponents = otherDeviceMap;
        createActivationObject();
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
    package.close();
    // do not remove staged package after update complete, there is a redfish
    // api to delete and delete intf handler will do that.
    if (fwPackageFilePath != stagedfwPackageFilePath)
    {
        std::filesystem::remove(fwPackageFilePath);
    }
}

void UpdateManager::setActivationStatus(
    const software::Activation::Activations& state)
{
    activation->activation(state);
    restoreStagedPackageActivationObjects();
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
    progressTimer = std::make_unique<phosphor::Timer>([this]() {
        updateInterval += 1;
        auto progressPercent = static_cast<uint8_t>(
            std::floor((100 * updateInterval) / totalInterval));
        if (fwDebug)
        {
            lg2::info("Progress Percent: {PROGRESSPERCENT}", "PROGRESSPERCENT",
                       progressPercent);
        }
        activationProgress->progress(progressPercent);
        // percent update should always be less than 100 when task is
        // aborted/cancelled. Setting to 100 percent will cause redfish task
        // service to show running and 100 percent
        if (updateInterval == totalInterval - 1)
        {
            if (fwDebug)
            {
                lg2::error("Firmware update timeout");
            }
            progressTimer->stop();
        }
        return;
    });
}

void UpdateManager::clearStagedPackage()
{
    updatePolicyStaged.reset();
    epochTime.reset();
    packageHash.reset();
    packageInfo.reset();
    activationStaged.reset();
    activationProgressStaged.reset();
    stagedObjPath.clear();
    std::filesystem::remove(stagedfwPackageFilePath);
    stagedfwPackageFilePath.clear();
}

void UpdateManager::updateStagedPackageProperties(
    bool packageVerificationStatus, uintmax_t packageSize)
{
    std::string packageVersion;
    if (packageVerificationStatus)
    {
        packageVersion = parser->pkgVersion;
    }
    std::unique_ptr<PackageSignatureShaBase> signatureSha =
        std::make_unique<PackageSignatureSha384>();
    auto digestVector =
        signatureSha->calculateDigest(package, packageSize);
    std::ostringstream tempStream;
    for (int byte : digestVector)
    {
        tempStream << std::setfill('0') << std::setw(2) << std::hex << byte;
    }
    std::string digest = tempStream.str();
    std::filesystem::file_time_type packageTimeStamp =
        std::filesystem::last_write_time(stagedfwPackageFilePath);
    const auto packageTimeStampSys =
        std::chrono::file_clock::to_sys(packageTimeStamp);
    uint64_t packageTimeStampInEpoch =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            packageTimeStampSys.time_since_epoch())
            .count();
    updatePolicyStaged = std::make_unique<UpdatePolicy>(
        pldm::utils::DBusHandler::getBus(), stagedObjPath);
    epochTime =
        std::make_unique<EpochTime>(pldm::utils::DBusHandler::getBus(),
                                    stagedObjPath, packageTimeStampInEpoch);
    packageHash = std::make_unique<PackageHash>(
        pldm::utils::DBusHandler::getBus(), stagedObjPath, digest,
        packageSignatureSha384Name);
    packageInfo = std::make_unique<PackageInformation>(
        pldm::utils::DBusHandler::getBus(), stagedObjPath, packageVersion,
        packageVerificationStatus);
    activationStaged = std::make_unique<Activation>(
        pldm::utils::DBusHandler::getBus(), stagedObjPath,
        software::Activation::Activations::Ready, this);
    activationProgressStaged = std::make_unique<ActivationProgress>(
        pldm::utils::DBusHandler::getBus(), stagedObjPath);
    package.close();
}

int UpdateManager::processStagedPackage(
    const std::filesystem::path& packageFilePath)
{
    namespace software = sdbusplus::xyz::openbmc_project::Software::server;
    size_t versionHash = std::hash<std::string>{}(packageFilePath);
    stagedObjPath = swRootPath + "staged/" + std::to_string(versionHash);
    stagedfwPackageFilePath = packageFilePath;

    package.open(packageFilePath,
                 std::ios::binary | std::ios::in | std::ios::ate);
    uintmax_t packageSize = package.tellg();
    if (!package.good())
    {
        lg2::error("Opening the PLDM FW update package failed, ERR={ERRNO}, "
                   "PACKAGEFILEPATH={PACKAGEFILEPATH}",
                   "ERRNO", strerror(errno), "PACKAGEFILEPATH",
                   packageFilePath);
        updateStagedPackageProperties(false, packageSize);
        return -1;
    }

    if (packageSize < sizeof(pldm_package_header_information))
    {
        lg2::error(
            "PLDM FW update package length less than the length of the package"
            " header information, PACKAGESIZE={PACKAGESIZE}",
            "PACKAGESIZE", packageSize);
        updateStagedPackageProperties(false, packageSize);
        return -1;
    }

    package.seekg(0);
    std::vector<uint8_t> packageHeader(sizeof(pldm_package_header_information));
    package.read(reinterpret_cast<char*>(packageHeader.data()),
                 sizeof(pldm_package_header_information));

    auto pkgHeaderInfo =
        reinterpret_cast<const pldm_package_header_information*>(
            packageHeader.data());
    auto pkgHeaderInfoSize = sizeof(pldm_package_header_information) +
                             pkgHeaderInfo->package_version_string_length;
    packageHeader.clear();
    packageHeader.resize(pkgHeaderInfoSize);
    package.seekg(0);
    package.read(reinterpret_cast<char*>(packageHeader.data()),
                 pkgHeaderInfoSize);

    parser = parsePkgHeader(packageHeader);
    if (parser == nullptr)
    {
        lg2::error("Invalid PLDM package header information");
        updateStagedPackageProperties(false, packageSize);
        return -1;
    }

    package.seekg(0);
    packageHeader.resize(parser->pkgHeaderSize);
    package.read(reinterpret_cast<char*>(packageHeader.data()),
                 parser->pkgHeaderSize);
    try
    {
        parser->parse(packageHeader, packageSize);
    }
    catch (const std::exception& e)
    {
        updateStagedPackageProperties(false, packageSize);
        return -1;
    }

#ifdef PLDM_PACKAGE_VERIFICATION

    try
    {
        if (!verifyPackage())
        {
            updateStagedPackageProperties(false, packageSize);
            return -1;
        }
    }
    catch (const std::exception& e)
    {
        updateStagedPackageProperties(false, packageSize);
        return -1;
    }

#endif
    updateStagedPackageProperties(true, packageSize);
    return 0;
}

void UpdateManager::restoreStagedPackageActivationObjects()
{
    if (fwPackageFilePath == stagedfwPackageFilePath)
    {
        activationStaged = std::move(activation);
        activationProgressStaged = std::move(activationProgress);
        activation = nullptr;
        activationProgress = nullptr;
    }
}

} // namespace fw_update
} // namespace pldm
