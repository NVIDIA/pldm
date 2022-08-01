#include "mctp_endpoint_discovery.hpp"

#include "libpldm/requester/pldm.h"

#include "common/types.hpp"
#include "common/utils.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pldm
{

MctpDiscovery::MctpDiscovery(sdbusplus::bus::bus& bus,
                             mctp_socket::Handler& handler,
                             fw_update::Manager* fwManager) :
    bus(bus),
    handler(handler), fwManager(fwManager),
    mctpEndpointSignal(bus,
                       sdbusplus::bus::match::rules::interfacesAdded(
                           "/xyz/openbmc_project/mctp"),
                       std::bind_front(&MctpDiscovery::discoverEndpoints, this))
{
    std::set<dbus::Service> mctpCtrlServices;

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
        return;
    }

    MctpInfos mctpInfos;
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
                populateMctpInfo(interfaces, mctpInfos);
            }
        }
        catch (const std::exception& e)
        {
            continue;
        }
    }

    if (mctpInfos.size() && fwManager)
    {
        fwManager->handleMCTPEndpoints(mctpInfos);
    }
}

void MctpDiscovery::populateMctpInfo(const dbus::InterfaceMap& interfaces,
                                     MctpInfos& mctpInfos)
{
    UUID uuid{};
    int type = 0;
    int protocol = 0;
    std::vector<uint8_t> address{};

    try
    {
        for (const auto& [intfName, properties] : interfaces)
        {
            if (intfName == uuidEndpointIntfName)
            {
                uuid = std::get<std::string>(properties.at("UUID"));
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
                if (std::find(mctpTypes.begin(), mctpTypes.end(),
                              mctpTypePLDM) != mctpTypes.end())
                {
                    handler.registerMctpEndpoint(eid, type, protocol, address);
                    mctpInfos.emplace_back(
                        std::make_tuple(eid, uuid, mediumType));
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
    }
}

void MctpDiscovery::discoverEndpoints(sdbusplus::message::message& msg)
{
    MctpInfos mctpInfos;

    sdbusplus::message::object_path objPath;
    dbus::InterfaceMap interfaces;
    msg.read(objPath, interfaces);

    populateMctpInfo(interfaces, mctpInfos);
    if (mctpInfos.size() && fwManager)
    {
        fwManager->handleMCTPEndpoints(mctpInfos);
    }
}

} // namespace pldm