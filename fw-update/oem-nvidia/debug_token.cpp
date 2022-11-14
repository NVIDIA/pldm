
#include "debug_token.hpp"

#include "libpldm/firmware_update.h"

#include "../activation.hpp"
#include "../update_manager.hpp"
#include "../dbusutil.hpp"
#include "common/types.hpp"
#include "common/utils.hpp"

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
    std::cout << "Activating : OBJPATH =" << tokenPath << "\n";
    try
    {
        pldm::utils::DBusHandler().setDbusProperty(
            dbusMapping, std::string(Server::Activation::interface) +
                             ".RequestedActivations.Active");
    }
    catch (const std::exception& e)
    {
        std::cerr << "Failed to set resource RequestedActivation :" << tokenPath
                  << " " << std::string(e.what()) << "\n";
        std::string resolution;
        createLogEntry(transferFailed,
                       std::filesystem::path(tokenPath).filename(),
                       tokenVersion, resolution);
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
                    std::cerr << "Invalid applicable components"
                              << "\n";
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
                    std::cerr
                        << "failed to get filepath :" << std::string(e.what())
                        << "\n";
                    continue;
                }
                std::cerr << "Got filepath for install token \"" << filepath
                          << "\"\n";
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
                std::cerr << "Extracting " << version
                          << " to filepath : " << filepath << "\n";
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
            std::cerr << "failed to get filepath :" << std::string(e.what())
                      << "\n";
            std::string resolution;
            createLogEntry(transferFailed, "HGX_FW_Debug_Token_Erase", "0.0",
                           resolution);
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
        std::cerr << "Activation failed for debug token"
                  << "\n";
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
        std::cerr
            << "Failed to get software D-Bus objects implementing UUID interface, "
            << "ERROR=" << e.what() << "\n";
    }
}

void DebugToken::startTimer(auto timerExpiryTime)
{
    timer = std::make_unique<phosphor::Timer>([this]() {
        if (!tokenStatus)
        {
            std::string resolution;
            createLogEntry(transferFailed,
                           std::filesystem::path(tokenPath).filename(),
                           tokenVersion, resolution);
            std::cerr << "Activation Timer expired for install debug token"
                      << "\n";
            startUpdate();
        }
    });
    std::cerr << "Starting Timer to allow install or erase debug token"
              << "\n";
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
        std::cerr << "Failed to set extended version :" << std::string(e.what())
                  << "\n";
    }
}

} // namespace fw_update
} // namespace pldm