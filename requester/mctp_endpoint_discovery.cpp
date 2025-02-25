#include "mctp_endpoint_discovery.hpp"

#include "common/types.hpp"
#include "common/utils.hpp"

#include <libpldm/pldm.h>
#include <linux/mctp.h>

#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using namespace sdbusplus::bus::match::rules;

PHOSPHOR_LOG2_USING;

namespace pldm
{
MctpDiscovery::MctpDiscovery(
    sdbusplus::bus_t& bus,
    std::initializer_list<MctpDiscoveryHandlerIntf*> list,
    const std::filesystem::path& staticEidTablePath) :
    bus(bus), mctpEndpointAddedSignal(
                  bus, interfacesAdded(MCTPPath),
                  std::bind_front(&MctpDiscovery::discoverEndpoints, this)),
    mctpEndpointRemovedSignal(
        bus, interfacesRemoved(MCTPPath),
        std::bind_front(&MctpDiscovery::removeEndpoints, this)),
    handlers(list), staticEidTablePath(staticEidTablePath)
{
    getMctpInfos(existingMctpInfos);
    loadStaticEndpoints(existingMctpInfos);
    handleMctpEndpoints(existingMctpInfos);
}

void MctpDiscovery::getMctpInfos(MctpInfos& mctpInfos)
{
    // Find all implementations of the MCTP Endpoint interface
    dbus::MctpInterfaces mctpInterfaces;
    pldm::utils::GetSubTreeResponse mapperResponse;
    try
    {
        mapperResponse = pldm::utils::DBusHandler().getSubtree(
            MCTPPath, 0, std::vector<std::string>({MCTPInterface}));
    }
    catch (const sdbusplus::exception_t& e)
    {
        error(
            "Failed to getSubtree call at path '{PATH}' and interface '{INTERFACE}', error - {ERROR} ",
            "ERROR", e, "PATH", MCTPPath, "INTERFACE", MCTPInterface);
        return;
    }

    for (const auto& [path, services] : mapperResponse)
    {
        for (const auto& serviceIter : services)
        {
            const std::string& service = serviceIter.first;
            std::string uuid{};
            try
            {
                const MctpEndpointProps& epProps =
                    getMctpEndpointProps(service, path);
                const UUID& uuid = getEndpointUUIDProp(service, path);
                if (uuid.empty())
                {
                    continue;
                }
                auto types = std::get<MCTPMsgTypes>(epProps);
                if (std::find(types.begin(), types.end(), mctpTypePLDM) !=
                    types.end())
                {
                    mctpInfos.emplace_back(
                        MctpInfo(std::get<EID>(epProps), uuid, {}));
                }
                else
                {
                    continue;
                }
                // watch PropertiesChanged signal from
                // xyz.openbmc_project.Object.Enable PDI
                if (enableMatches.find(path) == enableMatches.end())
                {
                    info("register match_t path:{OBJPATH}", "OBJPATH", path);
                    enableMatches.emplace(
                        path,
                        sdbusplus::bus::match_t(
                            bus,
                            sdbusplus::bus::match::rules::propertiesChanged(
                                path.c_str(),
                                "xyz.openbmc_project.Object.Enable"),
                            std::bind_front(&MctpDiscovery::refreshEndpoints,
                                            this)));
                }
            }
            catch (const sdbusplus::exception_t& e)
            {
                error(
                    "Error reading MCTP Endpoint property at path '{PATH}' and service '{SERVICE}', error - {ERROR}",
                    "ERROR", e, "SERVICE", service, "PATH", path);
                return;
            }
        }
    }
}

MctpEndpointProps
    MctpDiscovery::getMctpEndpointProps(const std::string& service,
                                        const std::string& path)
{
    try
    {
        auto properties = pldm::utils::DBusHandler().getDbusPropertiesVariant(
            service.c_str(), path.c_str(), MCTPInterface);

        if (properties.contains("NetworkId") && properties.contains("EID") &&
            properties.contains("SupportedMessageTypes"))
        {
            auto networkId = std::get<NetworkId>(properties.at("NetworkId"));
            auto eid = std::get<mctp_eid_t>(properties.at("EID"));
            auto types = std::get<std::vector<uint8_t>>(
                properties.at("SupportedMessageTypes"));
            return MctpEndpointProps(networkId, eid, types);
        }
    }
    catch (const sdbusplus::exception_t& e)
    {
        error(
            "Error reading MCTP Endpoint property at path '{PATH}' and service '{SERVICE}', error - {ERROR}",
            "SERVICE", service, "PATH", path, "ERROR", e);
        return MctpEndpointProps(0, MCTP_ADDR_ANY, {});
    }

    return MctpEndpointProps(0, MCTP_ADDR_ANY, {});
}

UUID MctpDiscovery::getEndpointUUIDProp(const std::string& service,
                                        const std::string& path)
{
    try
    {
        auto properties = pldm::utils::DBusHandler().getDbusPropertiesVariant(
            service.c_str(), path.c_str(), EndpointUUID);

        if (properties.contains("UUID"))
        {
            return std::get<UUID>(properties.at("UUID"));
        }
    }
    catch (const sdbusplus::exception_t& e)
    {
        error(
            "Error reading Endpoint UUID property at path '{PATH}' and service '{SERVICE}', error - {ERROR}",
            "SERVICE", service, "PATH", path, "ERROR", e);
        return static_cast<UUID>(emptyUUID);
    }

    return static_cast<UUID>(emptyUUID);
}

void MctpDiscovery::getAddedMctpInfos(sdbusplus::message_t& msg,
                                      MctpInfos& mctpInfos)
{
    using ObjectPath = sdbusplus::message::object_path;
    ObjectPath objPath;
    using Property = std::string;
    using PropertyMap = std::map<Property, pldm::dbus::Value>;
    std::map<std::string, PropertyMap> interfaces;
    std::vector<uint8_t> address{};

    try
    {
        msg.read(objPath, interfaces);
    }
    catch (const sdbusplus::exception_t& e)
    {
        error(
            "Error reading MCTP Endpoint added interface message, error - {ERROR}",
            "ERROR", e);
        return;
    }

    /* Get UUID */
    UUID uuid{};
    try
    {
        auto service = pldm::utils::DBusHandler().getService(
            objPath.str.c_str(), EndpointUUID);
        uuid = getEndpointUUIDProp(service, objPath.str);
    }
    catch (const sdbusplus::exception_t& e)
    {
        error("Error getting Endpoint UUID D-Bus interface, error - {ERROR}",
              "ERROR", e);
    }
    for (const auto& [intfName, properties] : interfaces)
    {
        if (intfName == MCTPInterface)
        {
            if (properties.contains("NetworkId") &&
                properties.contains("EID") &&
                properties.contains("SupportedMessageTypes"))
            {
                auto networkId =
                    std::get<NetworkId>(properties.at("NetworkId"));
                auto eid = std::get<mctp_eid_t>(properties.at("EID"));
                auto types = std::get<std::vector<uint8_t>>(
                    properties.at("SupportedMessageTypes"));
                if (std::find(types.begin(), types.end(), mctpTypePLDM) !=
                    types.end())
                {
                    info(
                        "Adding Endpoint networkId '{NETWORK}' and EID '{EID}' UUID '{UUID}'",
                        "NETWORK", networkId, "EID", eid, "UUID", uuid);
                    mctpInfos.emplace_back(MctpInfo(eid, uuid, {}));

                    // watch PropertiesChanged signal from
                    // xyz.openbmc_project.Object.Enable PDI
                    if (enableMatches.find(objPath.str) == enableMatches.end())
                    {
                        info("register match_t objectPath:{OBJPATH}", "OBJPATH",
                             objPath.str);
                        enableMatches.emplace(
                            objPath.str,
                            sdbusplus::bus::match_t(
                                bus,
                                sdbusplus::bus::match::rules::propertiesChanged(
                                    objPath.str,
                                    "xyz.openbmc_project.Object.Enable"),
                                std::bind_front(
                                    &MctpDiscovery::refreshEndpoints, this)));
                    }
                }
            }
        }
    }
}

void MctpDiscovery::addToExistingMctpInfos(const MctpInfos& addedInfos)
{
    for (const auto& mctpInfo : addedInfos)
    {
        if (std::find(existingMctpInfos.begin(), existingMctpInfos.end(),
                      mctpInfo) == existingMctpInfos.end())
        {
            existingMctpInfos.emplace_back(mctpInfo);
        }
    }
}

void MctpDiscovery::removeFromExistingMctpInfos(MctpInfos& mctpInfos,
                                                MctpInfos& removedInfos)
{
    for (const auto& mctpInfo : existingMctpInfos)
    {
        if (std::find(mctpInfos.begin(), mctpInfos.end(), mctpInfo) ==
            mctpInfos.end())
        {
            removedInfos.emplace_back(mctpInfo);
        }
    }
    for (const auto& mctpInfo : removedInfos)
    {
        info("Removing Endpoint EID '{EID}'", "EID", std::get<0>(mctpInfo));
        existingMctpInfos.erase(std::remove(existingMctpInfos.begin(),
                                            existingMctpInfos.end(), mctpInfo),
                                existingMctpInfos.end());
    }
}

void MctpDiscovery::loadStaticEndpoints(MctpInfos& mctpInfos)
{
    if (!std::filesystem::exists(staticEidTablePath))
    {
        return;
    }

    std::ifstream jsonFile(staticEidTablePath);
    auto data = nlohmann::json::parse(jsonFile, nullptr, false);
    if (data.is_discarded())
    {
        error("Parsing json file failed. FilePath={FILE_PATH}", "FILE_PATH",
              std::string(staticEidTablePath));
        return;
    }

    const std::vector<nlohmann::json> emptyJsonArray{};
    auto endpoints = data.value("Endpoints", emptyJsonArray);
    for (const auto& endpoint : endpoints)
    {
        const std::vector<uint8_t> emptyUnit8Array;
        const std::string emptyString;
        auto eid = endpoint.value("EID", 0xFF);
        auto types = endpoint.value("SupportedMessageTypes", emptyUnit8Array);
        if (std::find(types.begin(), types.end(), mctpTypePLDM) != types.end())
        {
            error("Added Static MCTP Info for EID: {EID}", "EID", eid);
            mctpInfos.emplace_back(MctpInfo(eid, emptyUUID, {}));
        }
    }
}

void MctpDiscovery::discoverEndpoints(sdbusplus::message_t& msg)
{
    MctpInfos addedInfos;
    getAddedMctpInfos(msg, addedInfos);
    addToExistingMctpInfos(addedInfos);
    loadStaticEndpoints(addedInfos);
    handleMctpEndpoints(addedInfos);
}

void MctpDiscovery::removeEndpoints(sdbusplus::message_t&)
{
    MctpInfos mctpInfos;
    MctpInfos removedInfos;
    getMctpInfos(mctpInfos);
    removeFromExistingMctpInfos(mctpInfos, removedInfos);
    handleRemovedMctpEndpoints(removedInfos);
}

void MctpDiscovery::handleMctpEndpoints(const MctpInfos& mctpInfos)
{
    for (const auto& handler : handlers)
    {
        if (handler)
        {
            handler->handleMctpEndpoints(mctpInfos);
        }
    }
}
void MctpDiscovery::refreshEndpoints(sdbusplus::message::message& msg)
{
    std::string interface;
    pldm::dbus::PropertyMap properties;
    std::string objPath = msg.get_path();
    std::string service = msg.get_sender();

    msg.read(interface, properties);
    auto prop = properties.find("Enabled");
    if (prop != properties.end())
    {
        auto enabled = std::get<bool>(prop->second);
        info(
            "Received xyz.openbmc_poject.Object.Enabled PropertiesChanged signal for "
            "Enabled={ENABLED} at PATH={OBJ_PATH} from SERVICE={SERVICE}",
            "ENABLED", enabled, "OBJ_PATH", objPath, "SERVICE", service);

        for (MctpDiscoveryHandlerIntf* handler : handlers)
        {
            try
            {
                const auto uuid =
                    pldm::utils::DBusHandler().getDbusProperty<std::string>(
                        objPath.c_str(), "UUID",
                        "xyz.openbmc_project.Common.UUID");

                const auto eid =
                    pldm::utils::DBusHandler().getDbusProperty<uint32_t>(
                        objPath.c_str(), "EID",
                        "xyz.openbmc_project.MCTP.Endpoint");

                if (enabled)
                {
                    handler->onlineMctpEndpoint(uuid, eid);
                }
                else
                {
                    handler->offlineMctpEndpoint(uuid, eid);
                }
            }
            catch (const std::exception& e)
            {
                error("refreshEndpoints: failed to get UUID,  {ERROR}", "ERROR",
                      e);
            }
        }
    }
}

void MctpDiscovery::handleRemovedMctpEndpoints(const MctpInfos& mctpInfos)
{
    for (const auto& handler : handlers)
    {
        if (handler)
        {
            handler->handleRemovedMctpEndpoints(mctpInfos);
        }
    }
}

} // namespace pldm
