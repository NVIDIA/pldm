
#include "other_device_update_manager.hpp"

#include "libpldm/firmware_update.h"

#include "activation.hpp"
#include "common/types.hpp"
#include "common/utils.hpp"
#include "update_manager.hpp"
#include "watch.hpp"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <xyz/openbmc_project/Common/FilePath/server.hpp>
#include <xyz/openbmc_project/Common/UUID/server.hpp>
#include <xyz/openbmc_project/Common/error.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <tuple>

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
        std::cerr << "Activating : OBJPATH =" << path << "\n";
        try
        {
            pldm::utils::DBusHandler().setDbusProperty(
                dbusMapping, std::string(Server::Activation::interface) +
                                 ".RequestedActivations.Active");
        }
        catch (const std::exception& e)
        {
            std::cerr << "Failed to set resource RequestedActivation :" << path
                      << " " << std::string(e.what()) << "\n";
            std::string resolution;
            updateManager->createLogEntry(
                updateManager->transferFailed,
                uuidMappings[x.second->uuid].componentName,
                uuidMappings[x.second->uuid].version, resolution);
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
            updateManager->updateOtherDeviceCompletion(
                otherDevices[objPath]->uuid, true);
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
    pldm::utils::DBusMapping dbusMapping{
        path, "xyz.openbmc_project.Software.UpdatePolicy", "Targets",
        "array[object_path]"};
    try
    {
        pldm::utils::DBusHandler().setDbusProperty(dbusMapping, targets);
        return true;
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        std::cerr << "Failed to set targets :" << std::string(e.what()) << "\n";
        // when target filter is specified only selected devices should update
        // return error so that user can retry the update on failed devices
        return false;
    }
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
        std::cerr << "New Interface Added. OBJPATH=" << path
                  << ", INTF=" << intf.first << "\n";
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
                            std::cerr << "Failed to set extended version :"
                                      << std::string(e.what()) << "\n";
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

                const auto& applicableCompVec =
                    std::get<ApplicableComponents>(fwDeviceIDRecord);
                const auto& componentImageInfo =
                    componentImageInfos[applicableCompVec[0]];
                const auto& version = std::get<7>(componentImageInfo);
                std::string fileName = "";
                std::string objPath;
                try
                {
                    // get File PATH and object path
                    std::tie(fileName, objPath) = getFilePath(uuid);
                }
                catch (const sdbusplus::exception::SdBusError& e)
                {
                    std::cerr
                        << "failed to get filename :" << std::string(e.what())
                        << "\n";
                    continue;
                }
                std::cerr << "Got Filename \"" << fileName << "\"\n";
                if (fileName == "")
                {
                    continue;
                }
                package.seekg(
                    std::get<5>(componentImageInfo)); // SEEK to image offset
                std::vector<uint8_t> buffer(std::get<6>(componentImageInfo));
                package.read(reinterpret_cast<char*>(buffer.data()),
                             buffer.size());

                fileName += "/" + boost::uuids::to_string(
                                      boost::uuids::random_generator()())
                                      .substr(0, 8);
                std::cerr << "Extracting " << version
                          << " to fileName : " << fileName << "\n";

                std::ofstream outfile(fileName, std::ofstream::binary);
                outfile.write(reinterpret_cast<const char*>(&buffer[0]),
                              buffer.size() *
                                  sizeof(uint8_t)); // Write to image offset
                outfile.close();
                totalNumImages++;
                isImageFileProcessed[uuid] = false;
                uuidMappings[uuid] = {
                    version, std::filesystem::path(objPath).filename()};
            }
        }
    }
    startTimer(totalNumImages * UPDATER_ACTIVATION_WAIT_PER_IMAGE_SEC);
    return totalNumImages;
#endif
}

void OtherDeviceUpdateManager::startTimer(int timerExpiryTime)
{
    timer = std::make_unique<phosphor::Timer>([this]() {
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
                    std::cerr << x.first << " not processed at timeout"
                              << "\n";
                    // update message registry
                    std::string resolution;
                    updateManager->createLogEntry(
                        updateManager->transferFailed,
                        uuidMappings[x.first].componentName,
                        uuidMappings[x.first].version, resolution);
                    updateManager->updateOtherDeviceCompletion(x.first,
                                                               x.second);
                }
            }
            std::cerr << "Activation Timer expired"
                      << "\n";
        }
    });
    std::cerr << "Starting Timer to allow item updaters to process images"
              << "\n";
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

std::pair<std::string, std::string>
    OtherDeviceUpdateManager::getFilePath(const std::string& uuid)
{
    std::vector<std::string> paths;
    getValidPaths(paths);
    auto dbusHandler = pldm::utils::DBusHandler();
    for (auto& obj : paths)
    {
        std::cerr << "Checking path \"" << obj.c_str() << "\"\n";
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
                std::cerr << "Got Path: \"" << p << "\"\n";
                if (p != "")
                {
                    return {std::filesystem::path(p).parent_path(), obj};
                }
            }
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
            std::cerr
                << "Failed to read UUID property from software D-Bus objects, "
                << "ERROR=" << e.what() << "\n";
        }
    }
}

void OtherDeviceUpdateManager::getValidPaths(std::vector<std::string>& paths)
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
        std::cerr
            << "Failed to get software D-Bus objects implementing UUID interface, "
            << "ERROR=" << e.what() << "\n";
    }
}

} // namespace fw_update
} // namespace pldm