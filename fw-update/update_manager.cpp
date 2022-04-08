#include "update_manager.hpp"

#include "activation.hpp"
#include "common/utils.hpp"
#include "package_parser.hpp"

#include <xyz/openbmc_project/Logging/Entry/server.hpp>

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
namespace software = sdbusplus::xyz::openbmc_project::Software::server;

UpdateManager::UpdateManager(
    Event& event, pldm::requester::Handler<pldm::requester::Request>& handler,
    Requester& requester, const DescriptorMap& descriptorMap,
    const ComponentInfoMap& componentInfoMap,
    ComponentNameMap& componentNameMap, const ComponentSkipList& compSkipList, bool fwDebug) :
    event(event),
    handler(handler), requester(requester), fwDebug(fwDebug),
    descriptorMap(descriptorMap), componentInfoMap(componentInfoMap),
    componentNameMap(componentNameMap), compSkipList(compSkipList),
    watch(event.get(), std::bind_front(&UpdateManager::processPackage, this))
{
    updatePolicy = std::make_unique<UpdatePolicy>(
        pldm::utils::DBusHandler::getBus(), "/xyz/openbmc_project/software");

    if (std::filesystem::exists("/tmp/images"))
    {
        for (const auto& entry :
             std::filesystem::directory_iterator("/tmp/images"))
            std::filesystem::remove_all(entry.path());
    }
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

void UpdateManager::createLogEntry(const std::string& messageID,
                                   const std::string& compName,
                                   const std::string& compVersion,
                                   const std::string& resolution)
{
    using namespace sdbusplus::xyz::openbmc_project::Logging::server;
    using Level =
        sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

    auto createLog = [&messageID](std::map<std::string, std::string>& addData,
                                  Level& level) {
        static constexpr auto logObjPath = "/xyz/openbmc_project/logging";
        static constexpr auto logInterface =
            "xyz.openbmc_project.Logging.Create";
        auto& bus = pldm::utils::DBusHandler::getBus();

        try
        {
            auto service =
                pldm::utils::DBusHandler().getService(logObjPath, logInterface);
            auto severity = sdbusplus::xyz::openbmc_project::Logging::server::
                convertForMessage(level);
            auto method = bus.new_method_call(service.c_str(), logObjPath,
                                              logInterface, "Create");
            method.append(messageID, severity, addData);
            bus.call_noreply(method);
        }
        catch (const std::exception& e)
        {
            std::cerr
                << "Failed to create D-Bus log entry for message registry, ERROR="
                << e.what() << "\n";
        }
    };

    std::map<std::string, std::string> addData;
    addData["REDFISH_MESSAGE_ID"] = messageID;
    Level level = Level::Informational;

    if (messageID == targetDetermined || messageID == updateSuccessful)
    {
        addData["REDFISH_MESSAGE_ARGS"] = (compName + "," + compVersion);
    }
    else if (messageID == transferFailed || messageID == verificationFailed ||
             messageID == applyFailed || messageID == activateFailed)
    {
        addData["REDFISH_MESSAGE_ARGS"] = (compVersion + "," + compName);
        level = Level::Critical;
    }
    else if (messageID == transferringToComponent ||
             messageID == awaitToActivate)
    {
        addData["REDFISH_MESSAGE_ARGS"] = (compVersion + "," + compName);
    }
    else
    {
        std::cerr << "Message Registry messageID is not recognised, "
                  << messageID << "\n";
        return;
    }

    if (!resolution.empty())
    {
        addData["xyz.openbmc_project.Logging.Entry.Resolution"] = resolution;
    }

    createLog(addData, level);
    return;
}

void UpdateManager::createMessageRegistry(
    mctp_eid_t eid, const FirmwareDeviceIDRecord& fwDeviceIDRecord,
    size_t compIndex, const std::string& messageID,
    const std::string& resolution)
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
}

int UpdateManager::processPackage(const std::filesystem::path& packageFilePath)
{
    // If a firmware activation of a package is in progress, don't proceed with
    // package processing
    if (activation)
    {

        if (activation->activation() ==
            software::Activation::Activations::Activating)
        {
            std::cerr
                << "Activation of PLDM FW update package already in progress"
                << ", PACKAGE_VERSION=" << parser->pkgVersion << "\n";
            std::filesystem::remove(packageFilePath);
            return -1;
        }
        else
        {
            clearActivationInfo();
        }
    }

    // create the device updater
    otherDeviceUpdateManager = std::make_unique<OtherDeviceUpdateManager>(
        pldm::utils::DBusHandler::getBus(), this);

    // If no devices discovered, take no action on the package.
    if (!descriptorMap.size() && !otherDeviceUpdateManager->getValidTargets())
    {
        otherDeviceUpdateManager = nullptr;
        return 0;
    }

    namespace software = sdbusplus::xyz::openbmc_project::Software::server;

    // Populate object path with the hash of the package file path
    size_t versionHash = std::hash<std::string>{}(packageFilePath);
    objPath = swRootPath + std::to_string(versionHash);

    package.open(packageFilePath,
                 std::ios::binary | std::ios::in | std::ios::ate);
    if (!package.good())
    {
        std::cerr << "Opening the PLDM FW update package failed, ERR="
                  << unsigned(errno) << ", PACKAGEFILE=" << packageFilePath
                  << "\n";
        activation = std::make_unique<Activation>(
            pldm::utils::DBusHandler::getBus(), objPath,
            software::Activation::Activations::Invalid, this);
        package.close();
        std::filesystem::remove(packageFilePath);
        return -1;
    }

    uintmax_t packageSize = package.tellg();
    if (packageSize < sizeof(pldm_package_header_information))
    {
        std::cerr << "PLDM FW update package length less than the length of "
                     "the package header information, PACKAGESIZE="
                  << packageSize << "\n";
        activation = std::make_unique<Activation>(
            pldm::utils::DBusHandler::getBus(), objPath,
            software::Activation::Activations::Invalid, this);
        package.close();
        std::filesystem::remove(packageFilePath);
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
        std::cerr << "Invalid PLDM package header information"
                  << "\n";
        activation = std::make_unique<Activation>(
            pldm::utils::DBusHandler::getBus(), objPath,
            software::Activation::Activations::Invalid, this);
        package.close();
        std::filesystem::remove(packageFilePath);
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
        std::cerr << "Invalid PLDM package header"
                  << "\n";
        activation = std::make_unique<Activation>(
            pldm::utils::DBusHandler::getBus(), objPath,
            software::Activation::Activations::Invalid, this);
        package.close();
        parser.reset();
        return -1;
    }

    const auto& compImageInfos = parser->getComponentImageInfos();
    auto targets = updatePolicy->targets();
    auto deviceUpdaterInfos = associatePkgToDevices(
        parser->getFwDeviceIDRecords(), descriptorMap, compImageInfos,
        componentNameMap, targets, compSkipList, fwDeviceIDRecords,
        totalNumComponentUpdates);

    std::cout << "Total Components: " << totalNumComponentUpdates << "\n";

    for (const auto& deviceUpdaterInfo : deviceUpdaterInfos)
    {
        std::cerr << "EID = " << unsigned(deviceUpdaterInfo.first)
                  << ", RecordOffset = " << unsigned(deviceUpdaterInfo.second)
                  << " ComponentIdentifiers = ";
        auto& applicableComponents = std::get<ApplicableComponents>(
            fwDeviceIDRecords[deviceUpdaterInfo.second]);
        for (const auto& index : applicableComponents)
        {
            const auto& compImageInfo = compImageInfos[index];
            CompIdentifier compIdentifier = std::get<static_cast<size_t>(
                ComponentImageInfoPos::CompIdentifierPos)>(compImageInfo);
            std::cout << unsigned(compIdentifier) << " ";
        }
        std::cerr << "\n";
    }

    // get non-pldm components, add to total component count
    size_t otherDevicesImageCount =
        otherDeviceUpdateManager->extractOtherDevicePkgs(
            parser->getFwDeviceIDRecords(), parser->getComponentImageInfos(),
            package);
    totalNumComponentUpdates += otherDevicesImageCount;

    if (!deviceUpdaterInfos.size() && !otherDevicesImageCount)
    {
        std::cerr
            << "No matching devices found with the PLDM firmware update package"
            << "\n";
        activation = std::make_unique<Activation>(
            pldm::utils::DBusHandler::getBus(), objPath,
            software::Activation::Activations::Ready, this);
        package.close();
        parser.reset();
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
                MAXIMUM_TRANSFER_SIZE, this));
    }
    fwPackageFilePath = packageFilePath;
    // delay activation object creation if there are non-pldm updates
    if (otherDevicesImageCount == 0)
    {
        createActivationObject();
    }
    return 0;
}

DeviceUpdaterInfos UpdateManager::associatePkgToDevices(
    const FirmwareDeviceIDRecords& inFwDeviceIDRecords,
    const DescriptorMap& descriptorMap,
    const ComponentImageInfos& compImageInfos,
    const ComponentNameMap& componentNameMap,
    const std::vector<sdbusplus::message::object_path>& objectPaths,
    const ComponentSkipList& compSkipList,
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
            std::cerr << "Target=" << target;
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
                if (compTargetList.empty())
                {
                    auto applicableComponents = std::get<ApplicableComponents>(
                        inFwDeviceIDRecords[index]);
                    for (const auto& compIndex : applicableComponents)
                    {
                        const auto& compImageInfo = compImageInfos[compIndex];
                        CompIdentifier compIdentifier =
                            std::get<static_cast<size_t>(
                                ComponentImageInfoPos::CompIdentifierPos)>(
                                compImageInfo);
                        if (compSkipList.contains(
                                std::make_pair(eid, compIdentifier)))
                        {
                            std::erase(applicableComponents, compIndex);
                        }
                    }

                    if (applicableComponents.size())
                    {
                        outFwDeviceIDRecords.emplace_back(
                            inFwDeviceIDRecords[index]);
                        std::get<ApplicableComponents>(
                            outFwDeviceIDRecords.back()) = applicableComponents;
                        deviceUpdaterInfos.emplace_back(std::make_pair(
                            eid, outFwDeviceIDRecords.size() - 1));
                        totalNumComponentUpdates += applicableComponents.size();
                    }
                }
                else
                {
                    if (compTargetList.contains(eid))
                    {
                        auto compList = compTargetList[eid];
                        auto applicableComponents =
                            std::get<ApplicableComponents>(
                                inFwDeviceIDRecords[index]);
                        for (const auto& compIndex : applicableComponents)
                        {
                            const auto& compImageInfo =
                                compImageInfos[compIndex];
                            CompIdentifier compIdentifier =
                                std::get<static_cast<size_t>(
                                    ComponentImageInfoPos::CompIdentifierPos)>(
                                    compImageInfo);
                            if ((std::find(compList.begin(), compList.end(),
                                           compIdentifier) == compList.end()) ||
                                compSkipList.contains(
                                    std::make_pair(eid, compIdentifier)))
                            {
                                std::erase(applicableComponents, compIndex);
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

void UpdateManager::updateDeviceCompletion(mctp_eid_t eid, bool status)
{
    /* update completion map */
    deviceUpdateCompletionMap.emplace(eid, status);

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
        std::cerr
            << "RequestFirmwareData reported PLDM_FWUP_COMMAND_NOT_EXPECTED, EID="
            << unsigned(eid) << "\n";
        auto ptr = reinterpret_cast<pldm_msg*>(response.data());
        auto rc = encode_cc_only_resp(request->hdr.instance_id,
                                      request->hdr.type, +request->hdr.command,
                                      PLDM_FWUP_COMMAND_NOT_EXPECTED, ptr);
        assert(rc == PLDM_SUCCESS);
    }

    return response;
}

void UpdateManager::clearActivationInfo()
{
    activation.reset();
    activationProgress.reset();
    objPath.clear();
    fwDeviceIDRecords.clear();

    deviceUpdaterMap.clear();
    deviceUpdateCompletionMap.clear();
    parser.reset();
    package.close();
    std::filesystem::remove(fwPackageFilePath);
    totalNumComponentUpdates = 0;
    compUpdateCompletedCount = 0;
    otherDeviceUpdateManager.reset();
    otherDeviceComponents.clear();
    otherDeviceCompleted.clear();
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
            activation = std::make_unique<Activation>(
                pldm::utils::DBusHandler::getBus(), objPath,
                software::Activation::Activations::Ready, this);
            activationProgress = std::make_unique<ActivationProgress>(
                pldm::utils::DBusHandler::getBus(), objPath);
        }
        catch (const sdbusplus::exception::SdBusError& e)
        {
            return false;
        }
    }
    else
    {
        clearActivationInfo();
        //  Client will time out and Redfish End will report error
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
        std::cerr << "Firmware update time: "
                  << std::chrono::duration<double, std::milli>(endTime -
                                                               startTime)
                         .count()
                  << " ms\n";
    }
}

void UpdateManager::updateActivationProgress()
{
    compUpdateCompletedCount++;
    auto progressPercent = static_cast<uint8_t>(std::floor(
        (100 * compUpdateCompletedCount) / totalNumComponentUpdates));
    activationProgress->progress(progressPercent);
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
            std::cerr << "Other device manager failed to get " << uuid
                      << " ready\n";
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

void UpdateManager::updateOtherDeviceCompletion(std::string uuid, bool status)
{
    /* update completion status map */
    otherDeviceCompleted.emplace(uuid, status);

    updateActivationProgress();
    updatePackageCompletion();
}

} // namespace fw_update
} // namespace pldm
