#pragma once
#include "common/types.hpp"
#include "common/utils.hpp"

#include <sdbusplus/bus.hpp>
#include <xyz/openbmc_project/Logging/Entry/server.hpp>

constexpr auto dbusProperties = "org.freedesktop.DBus.Properties";
constexpr auto mapperService = "xyz.openbmc_project.ObjectMapper";
constexpr auto mapperPath = "/xyz/openbmc_project/object_mapper";
constexpr auto mapperInterface = "xyz.openbmc_project.ObjectMapper";
const std::string transferFailed{"Update.1.0.TransferFailed"};
const std::string transferringToComponent{"Update.1.0.TransferringToComponent"};
const std::string verificationFailed{"Update.1.0.VerificationFailed"};
const std::string updateSuccessful{"Update.1.0.UpdateSuccessful"};
const std::string awaitToActivate{"Update.1.0.AwaitToActivate"};
const std::string applyFailed{"Update.1.0.ApplyFailed"};
const std::string activateFailed{"Update.1.0.ActivateFailed"};
const std::string targetDetermined{"Update.1.0.TargetDetermined"};
const std::string resourceErrorDetected{
    "ResourceEvent.1.0.ResourceErrorsDetected"};

/**
 * @brief Get the D-Bus service using mapper lookup
 *
 * @param[in] bus
 * @param[in] path
 * @param[in] interface
 * @return std::string
 */
inline std::string getService(sdbusplus::bus::bus& bus, const char* path,
                              const char* interface)
{
    using DbusInterfaceList = std::vector<std::string>;
    std::map<std::string, std::vector<std::string>> mapperResponse;

    auto mapper = bus.new_method_call(mapperService, mapperPath,
                                      mapperInterface, "GetObject");
    mapper.append(path, DbusInterfaceList({interface}));

    auto mapperResponseMsg = bus.call(mapper);
    mapperResponseMsg.read(mapperResponse);
    return mapperResponse.begin()->first;
}

/**
 * @brief set D-Bus property. New bus will be used for every set to avoid
 * contention with single thread using same bus
 *
 * @param[in] dbusMap - D-Bus mappings
 * @param[in] value - value to set
 */
inline void setDBusProperty(const pldm::utils::DBusMapping& dbusMap,
                            const std::string& value)
{
    auto bus = sdbusplus::bus::new_default();
    std::string dBusService =
        getService(bus, dbusMap.objectPath.c_str(), dbusMap.interface.c_str());
    auto method = bus.new_method_call(
        dBusService.c_str(), dbusMap.objectPath.c_str(), dbusProperties, "Set");
    pldm::utils::PropertyValue propertyValue = value;
    method.append(dbusMap.interface.c_str(), dbusMap.propertyName.c_str(),
                  propertyValue);
    bus.call_noreply(method);
}

/** @brief Create the D-Bus log entry for message registry
 *
 *  @param[in] messageID - Message ID
 *  @param[in] arg0 - argument 0
 *  @param[in] arg1 - argument 1
 *  @param[in] resolution - Resolution field
 */
inline void createLogEntry(const std::string& messageID,
                           const std::string& arg0, const std::string& arg1,
                           const std::string& resolution)
{
    using namespace sdbusplus::xyz::openbmc_project::Logging::server;
    using Level =
        sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

    std::map<std::string, std::string> addData;
    addData["REDFISH_MESSAGE_ID"] = messageID;
    Level level = Level::Informational;

    if (messageID == targetDetermined || messageID == updateSuccessful)
    {
        addData["REDFISH_MESSAGE_ARGS"] = (arg0 + "," + arg1);
    }
    else if (messageID == transferFailed || messageID == verificationFailed ||
             messageID == applyFailed || messageID == activateFailed)
    {
        addData["REDFISH_MESSAGE_ARGS"] = (arg1 + "," + arg0);
        level = Level::Critical;
    }
    else if (messageID == transferringToComponent ||
             messageID == awaitToActivate)
    {
        addData["REDFISH_MESSAGE_ARGS"] = (arg1 + "," + arg0);
    }
    else if (messageID == resourceErrorDetected)
    {
        addData["REDFISH_MESSAGE_ARGS"] = (arg0 + "," + arg1);
        level = Level::Critical;
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

    // use separate container for fwupdate message registry
    addData["namespace"] = "FWUpdate";
    auto& asioConnection = pldm::utils::DBusHandler::getAsioConnection();
    auto severity =
        sdbusplus::xyz::openbmc_project::Logging::server::convertForMessage(
            level);
    asioConnection->async_method_call(
        [](boost::system::error_code ec) {
            if (ec)
            {
                std::cerr << "error while logging message registry: "
                          << ec.message() << "\n";
                return;
            }
        },
        "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
        "xyz.openbmc_project.Logging.Create", "Create", messageID, severity,
        addData);
    return;
}