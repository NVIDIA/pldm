#include "config.h"

#include "mctp_endpoint_discovery.hpp"

#include "libpldm/requester/pldm.h"

#include "common/types.hpp"
#include "common/utils.hpp"

#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pldm
{
const std::string emptyUUID = "00000000-0000-0000-0000-000000000000";

MctpDiscovery::MctpDiscovery(
    sdbusplus::bus::bus& bus, mctp_socket::Handler& handler,
    std::initializer_list<MctpDiscoveryHandlerIntf*> list,
    const std::filesystem::path& staticEidTablePath) :
    bus(bus),
    handler(handler),
    mctpEndpointAddedSignal(
        bus,
        sdbusplus::bus::match::rules::interfacesAdded(
            "/xyz/openbmc_project/mctp"),
        std::bind_front(&MctpDiscovery::discoverEndpoints, this)),
    mctpEndpointRemovedSignal(
        bus,
        sdbusplus::bus::match::rules::interfacesRemoved(
            "/xyz/openbmc_project/mctp"),
        std::bind_front(&MctpDiscovery::cleanEndpoints, this)),
    handlers(list), staticEidTablePath(staticEidTablePath)
{
    dbus::ObjectValueTree objects;
    std::set<dbus::Service> mctpCtrlServices;
    MctpInfos mctpInfos;
    dbus::MctpInterfaces mctpInterfaces;

    try
    {
        const dbus::Interfaces ifaceList{"xyz.openbmc_project.MCTP.Endpoint"};
        auto getSubTreeResponse = utils::DBusHandler().getSubtree(
            "/xyz/openbmc_project/mctp", 0, ifaceList);
        for (const auto& [objPath, mapperServiceMap] : getSubTreeResponse)
        {
            for (const auto& [serviceName, interfaces] : mapperServiceMap)
            {
                mctpCtrlServices.emplace(serviceName);
            }
        }
    }
    catch (const std::exception& e)
    {
        loadStaticEndpoints(mctpInfos);
        handleMctpEndpoints(mctpInfos, mctpInterfaces);
        return;
    }

    for (const auto& service : mctpCtrlServices)
    {
        dbus::ObjectValueTree objects{};
        try
        {
            auto method = bus.new_method_call(
                service.c_str(), "/xyz/openbmc_project/mctp",
                "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
            auto reply = bus.call(method);
            reply.read(objects);
            for (const auto& [objectPath, interfaces] : objects)
            {
                populateMctpInfo(interfaces, mctpInfos, mctpInterfaces);

                // watch PropertiesChanged signal from
                // xyz.openbmc_project.Object.Enable PDI
                if (enableMatches.find(objectPath.str) == enableMatches.end())
                {
                    lg2::info("register match_t objectPath:{OBJPATH}",
                              "OBJPATH", objectPath.str);
                    enableMatches.emplace(
                        objectPath.str,
                        sdbusplus::bus::match_t(
                            bus,
                            sdbusplus::bus::match::rules::propertiesChanged(
                                objectPath.str,
                                "xyz.openbmc_project.Object.Enable"),
                            std::bind_front(&MctpDiscovery::refreshEndpoints,
                                            this)));
                }
            }
        }
        catch (const std::exception& e)
        {
            continue;
        }
    }

    loadStaticEndpoints(mctpInfos);
    handleMctpEndpoints(mctpInfos, mctpInterfaces);
}

void MctpDiscovery::populateMctpInfo(const dbus::InterfaceMap& interfaces,
                                     MctpInfos& mctpInfos,
                                     dbus::MctpInterfaces& mctpInterfaces)
{
    UUID uuid{};
    int type = 0;
    int protocol = 0;
    std::vector<uint8_t> address{};
    std::string bindingType;
    try
    {
        for (const auto& [intfName, properties] : interfaces)
        {
            if (intfName == uuidEndpointIntfName)
            {
                uuid = std::get<std::string>(properties.at("UUID"));
                mctpInterfaces[uuid] = interfaces;
            }

            if (intfName == unixSocketIntfName)
            {
                type = std::get<size_t>(properties.at("Type"));
                protocol = std::get<size_t>(properties.at("Protocol"));
                address =
                    std::get<std::vector<uint8_t>>(properties.at("Address"));
            }
        }

        if (uuid.empty() || address.empty() || !type)
        {
            return;
        }

        if (interfaces.contains(mctpBindingIntfName))
        {
            const auto& properties = interfaces.at(mctpBindingIntfName);
            if (properties.contains("BindingType"))
            {
                bindingType =
                    std::get<std::string>(properties.at("BindingType"));
            }
        }
        if (interfaces.contains(mctpEndpointIntfName))
        {
            const auto& properties = interfaces.at(mctpEndpointIntfName);
            if (properties.contains("EID") &&
                properties.contains("SupportedMessageTypes") &&
                properties.contains("MediumType"))
            {
                auto eid = std::get<size_t>(properties.at("EID"));
                auto mctpTypes = std::get<std::vector<uint8_t>>(
                    properties.at("SupportedMessageTypes"));
                auto mediumType =
                    std::get<std::string>(properties.at("MediumType"));
                auto networkId = std::get<size_t>(properties.at("NetworkId"));
                if (std::find(mctpTypes.begin(), mctpTypes.end(),
                              mctpTypePLDM) != mctpTypes.end())
                {
                    handler.registerMctpEndpoint(eid, type, protocol, address);
                    mctpInfos.emplace_back(std::make_tuple(
                        eid, uuid, mediumType, networkId, bindingType));
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Error while getting properties.", "ERROR", e);
    }
}

void MctpDiscovery::discoverEndpoints(sdbusplus::message::message& msg)
{
    constexpr std::string_view mctpEndpointIntfName{
        "xyz.openbmc_project.MCTP.Endpoint"};
    MctpInfos mctpInfos;
    dbus::MctpInterfaces mctpInterfaces;

    sdbusplus::message::object_path objPath;
    dbus::InterfaceMap interfaces;
    msg.read(objPath, interfaces);

    populateMctpInfo(interfaces, mctpInfos, mctpInterfaces);

    // watch PropertiesChanged signal from xyz.openbmc_project.Object.Enable PDI
    if (enableMatches.find(objPath.str) == enableMatches.end())
    {
        lg2::info("register match_t objectPath:{OBJPATH}", "OBJPATH",
                  objPath.str);
        enableMatches.emplace(
            objPath.str,
            sdbusplus::bus::match_t(
                bus,
                sdbusplus::bus::match::rules::propertiesChanged(
                    objPath.str, "xyz.openbmc_project.Object.Enable"),
                std::bind_front(&MctpDiscovery::refreshEndpoints, this)));
    }

    loadStaticEndpoints(mctpInfos);
    handleMctpEndpoints(mctpInfos, mctpInterfaces);
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
        lg2::error("Parsing json file failed. FilePath={FILE_PATH}",
                   "FILE_PATH", std::string(staticEidTablePath));
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
        auto mediumType = endpoint.value("MediumType", emptyString);
        auto networkId = endpoint.value("NetworkId", 0xFF);
        auto bindingType = endpoint.value("BindingType", emptyString);
        if (std::find(types.begin(), types.end(), mctpTypePLDM) != types.end())
        {
            mctpInfos.emplace_back(
                MctpInfo(eid, emptyUUID, mediumType, networkId, bindingType));
        }
    }
}

void MctpDiscovery::handleMctpEndpoints(const MctpInfos& mctpInfos,
                                        dbus::MctpInterfaces& mctpInterfaces)
{
    for (MctpDiscoveryHandlerIntf* handler : handlers)
    {
        if (handler)
        {
            handler->handleMctpEndpoints(mctpInfos, mctpInterfaces);
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
        lg2::info(
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
                lg2::error("refreshEndpoints: failed to get UUID,  {ERROR}",
                           "ERROR", e);
            }
        }
    }
}

void MctpDiscovery::cleanEndpoints(
    [[maybe_unused]] sdbusplus::message::message& msg)
{
    // place holder: implement the function once mctp-ctrl service support the
    // InterfacesRemoved signal
    sdbusplus::message::object_path objPath;
    dbus::InterfaceMap interfaces;
    msg.read(objPath, interfaces);

    lg2::info("cleanEndpoints objectPath:{OBJPATH}", "OBJPATH", objPath);
}

} // namespace pldm
