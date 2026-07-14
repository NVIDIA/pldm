#include "config.h"

#include "mctp_endpoint_discovery.hpp"

#include "common/types.hpp"
#include "common/utils.hpp"

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

pldm::utils::DBusHandlerInterface& MctpDiscovery::defaultDbusHandler()
{
    static pldm::utils::DBusHandler handler;
    return handler;
}

MctpDiscovery::MctpDiscovery(
    sdbusplus::bus_t& bus,
    std::initializer_list<MctpDiscoveryHandlerIntf*> list,
    const std::filesystem::path& staticEidTablePath,
    pldm::utils::DBusHandlerInterface& dbusHandler) :
    bus(bus),
    mctpEndpointAddedSignal(
        bus, interfacesAddedAtPath(MCTPNetworksPath) + sender(MCTPService),
        [this](sdbusplus::message_t& msg) { this->discoverEndpoints(msg); }),
    mctpEndpointRemovedSignal(
        bus, interfacesRemovedAtPath(MCTPNetworksPath) + sender(MCTPService),
        [this](sdbusplus::message_t& msg) { this->removeEndpoints(msg); }),
    mctpEndpointPropChangedSignal(
        bus, propertiesChangedNamespace(MCTPPath, MCTPInterfaceCC),
        [this](sdbusplus::message_t& msg) { this->propertiesChangedCb(msg); }),
    handlers(list), staticEidTablePath(staticEidTablePath),
    dbusHandler(dbusHandler)
{
    std::map<MctpInfo, Availability> currentMctpInfoMap;
    getMctpInfos(currentMctpInfoMap);
    for (const auto& mapIt : currentMctpInfoMap)
    {
        if (mapIt.second)
        {
            // Only add the available endpoints to the terminus
            // Let the propertiesChanged signal tells us when it comes back
            // to Available again
            addToExistingMctpInfos(MctpInfos(1, mapIt.first));
        }
    }
    loadStaticEndpoints(existingMctpInfos);
    handleMctpEndpoints(existingMctpInfos);
}

void MctpDiscovery::getMctpInfos(std::map<MctpInfo, Availability>& mctpInfoMap)
{
    // Find all implementations of the MCTP Endpoint interface
    pldm::utils::GetSubTreeResponse mapperResponse;
    try
    {
        mapperResponse = dbusHandler.getSubtree(
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
            const MctpEndpointProps& epProps =
                getMctpEndpointProps(service, path);
            const UUID& uuid = getEndpointUUIDProp(service, path);
            const Availability& availability =
                getEndpointConnectivityProp(path);
            auto types = std::get<MCTPMsgTypes>(epProps);
            if (std::find(types.begin(), types.end(), mctpTypePLDM) !=
                types.end())
            {
                const auto& mctpBinding = std::get<4>(epProps);
                const auto& mctpMedium = std::get<3>(epProps);
                const auto& mctpLocalEid = std::get<5>(epProps);
                auto mctpInfo =
                    MctpInfo(std::get<eid>(epProps), uuid, mctpMedium,
                             std::get<NetworkId>(epProps), std::nullopt,
                             mctpBinding, mctpLocalEid);
                searchConfigurationFor(mctpInfo);
                mctpInfoMap[std::move(mctpInfo)] = availability;
            }
            else
            {
                continue;
            }
            // Watch for PropertiesChanged signal from
            // xyz.openbmc_project.Object.Enable PDI
            if (enableMatches.find(path) == enableMatches.end())
            {
                info("register match_t path:{OBJPATH}", "OBJPATH", path);
                enableMatches.emplace(
                    path, sdbusplus::bus::match_t(
                              bus,
                              sdbusplus::bus::match::rules::propertiesChanged(
                                  path.c_str(),
                                  "au.com.codeconstruct.MCTP.Endpoint1"),
                              std::bind_front(&MctpDiscovery::refreshEndpoints,
                                              this)));
            }
        }
    }
}

MctpEndpointProps MctpDiscovery::getMctpEndpointProps(
    const std::string& service, const std::string& path)
{
    try
    {
        auto properties = dbusHandler.getDbusPropertiesVariant(
            service.c_str(), path.c_str(), MCTPInterface);

        if (!properties.contains("NetworkId") or !properties.contains("EID") or
            !properties.contains("SupportedMessageTypes") or
            !properties.contains("MediumType"))
        {
            return MctpEndpointProps(0, MCTP_ADDR_ANY, {}, {}, {},
                                     std::nullopt);
        }

        auto networkId = std::get<NetworkId>(properties.at("NetworkId"));
        auto eid = std::get<mctp_eid_t>(properties.at("EID"));
        auto types = std::get<std::vector<uint8_t>>(
            properties.at("SupportedMessageTypes"));
        auto mediumType = std::get<MctpMedium>(properties.at("MediumType"));
        auto binding = std::get<MctpBinding>(dbusHandler.getDbusPropertyVariant(
            path.c_str(), "BindingType", MCTPBindingInterface));

        LocalEid localEid = std::nullopt;
        if (properties.contains("LocalEID"))
        {
            localEid = std::get<mctp_eid_t>(properties.at("LocalEID"));
        }

        return MctpEndpointProps(networkId, eid, types, mediumType, binding,
                                 localEid);
    }
    catch (const sdbusplus::exception_t& e)
    {
        error(
            "Error reading MCTP Endpoint property at path '{PATH}' and service '{SERVICE}', error - {ERROR}",
            "SERVICE", service, "PATH", path, "ERROR", e);
        return MctpEndpointProps(0, MCTP_ADDR_ANY, {}, {}, {}, std::nullopt);
    }
    catch (const std::exception& e)
    {
        error(
            "Unexpected error reading MCTP Endpoint property at path '{PATH}' and service '{SERVICE}', error - {ERROR}",
            "SERVICE", service, "PATH", path, "ERROR", e.what());
        return MctpEndpointProps(0, MCTP_ADDR_ANY, {}, {}, {}, std::nullopt);
    }
}

UUID MctpDiscovery::getEndpointUUIDProp(const std::string& service,
                                        const std::string& path)
{
    try
    {
        auto properties = dbusHandler.getDbusPropertiesVariant(
            service.c_str(), path.c_str(), EndpointUUID);

        if (properties.contains("UUID"))
        {
            return std::get<UUID>(properties.at("UUID"));
        }
        error(
            "UUID property not found for endpoint at path '{PATH}' and service '{SERVICE}'",
            "PATH", path, "SERVICE", service);
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

Availability MctpDiscovery::getEndpointConnectivityProp(const std::string& path)
{
    Availability available = false;
    try
    {
        pldm::utils::PropertyValue propertyValue =
            dbusHandler.getDbusPropertyVariant(
                path.c_str(), MCTPConnectivityProp, MCTPInterfaceCC);
        if (std::get<std::string>(propertyValue) == "Available")
        {
            available = true;
        }
    }
    catch (const sdbusplus::exception_t& e)
    {
        error(
            "Error reading Endpoint Connectivity property at path '{PATH}', error - {ERROR}",
            "PATH", path, "ERROR", e);
    }

    return available;
}

void MctpDiscovery::getAddedMctpInfos(sdbusplus::message_t& msg,
                                      MctpInfos& mctpInfos)
{
    using ObjectPath = sdbusplus::message::object_path;
    ObjectPath objPath;
    using Property = std::string;
    using PropertyMap = std::map<Property, dbus::Value>;
    std::map<std::string, PropertyMap> interfaces;
    std::string uuid = emptyUUID;

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
    // If the MCTP Endpoint interface is absent, this signal is not an endpoint
    // addition (e.g. a Bridge interface being added to an existing endpoint).
    // Ignore it to avoid spurious D-Bus calls.
    if (!interfaces.contains(MCTPInterface))
    {
        info(
            "getAddedMctpInfos: {PATH} MCTPInterface absent from signal, ignoring (not an endpoint signal)",
            "PATH", objPath.str);
        return;
    }

    // Read Connectivity from signal; default to true if absent.
    Availability availability = true;
    if (interfaces.contains(MCTPInterfaceCC))
    {
        const auto& ccProps = interfaces.at(MCTPInterfaceCC);
        if (ccProps.contains(MCTPConnectivityProp))
        {
            availability =
                (std::get<std::string>(ccProps.at(MCTPConnectivityProp)) ==
                 "Available");
        }
        else
        {
            info(
                "getAddedMctpInfos: {PATH} Connectivity property missing in signal, defaulting to Unavailable",
                "PATH", objPath.str);
        }
    }
    else
    {
        // Connectivity absent from signal — do not call ObjectMapper/D-Bus here
        // (ObjectMapper is also reacting to this InterfacesAdded and may not
        // have processed it yet). Default to unavailable; endpoint will still
        // be registered and can be updated later via PropertiesChanged.
        info(
            "getAddedMctpInfos: {PATH} Connectivity interface absent from signal, defaulting to unavailable",
            "PATH", objPath.str);
    }

    // Read UUID from signal only — do not fall back to ObjectMapper/D-Bus call.
    if (interfaces.contains(EndpointUUID))
    {
        const auto& uuidProps = interfaces.at(EndpointUUID);
        if (uuidProps.contains("UUID"))
        {
            uuid = std::get<UUID>(uuidProps.at("UUID"));
        }
        else
        {
            info("getAddedMctpInfos: {PATH} UUID property missing in signal",
                 "PATH", objPath.str);
        }
    }
    else
    {
        // UUID absent from signal — drop silently; mctpd emits all interfaces
        // atomically so this is an unexpected condition, not a retry scenario.
        info(
            "getAddedMctpInfos: {PATH} UUID interface absent from signal, dropping",
            "PATH", objPath.str);
        return;
    }

    if (uuid == emptyUUID)
    {
        error("Empty UUID for endpoint {PATH}, skipping", "PATH", objPath.str);
        return;
    }

    // Cache the full interface map keyed by UUID so handleMctpEndpoints()
    // can use it directly, avoiding ObjectMapper calls during boot.
    signalMctpInterfaces[uuid] = interfaces;

    for (const auto& [intfName, properties] : interfaces)
    {
        if (intfName == MCTPInterface)
        {
            if (properties.contains("NetworkId") &&
                properties.contains("EID") &&
                properties.contains("SupportedMessageTypes") &&
                properties.contains("MediumType"))
            {
                NetworkId networkId{};
                mctp_eid_t eid{};
                std::vector<uint8_t> types{};
                MctpMedium mediumType{};
                try
                {
                    networkId = std::get<NetworkId>(properties.at("NetworkId"));
                    eid = std::get<mctp_eid_t>(properties.at("EID"));
                    types = std::get<std::vector<uint8_t>>(
                        properties.at("SupportedMessageTypes"));
                    mediumType =
                        std::get<MctpMedium>(properties.at("MediumType"));
                }
                catch (const std::exception& e)
                {
                    error(
                        "Error parsing MCTP interface properties for endpoint {PATH}, error - {ERROR}",
                        "PATH", objPath.str, "ERROR", e.what());
                    continue;
                }

                MctpBinding binding{};
                try
                {
                    if (interfaces.contains(MCTPBindingInterface))
                    {
                        const auto& bindingProps =
                            interfaces.at(MCTPBindingInterface);
                        if (bindingProps.contains("BindingType"))
                        {
                            binding = std::get<MctpBinding>(
                                bindingProps.at("BindingType"));
                        }
                        else
                        {
                            warning(
                                "getAddedMctpInfos: {PATH} BindingType property missing in signal, skipping endpoint",
                                "PATH", objPath.str);
                            continue;
                        }
                    }
                    else
                    {
                        // BindingType absent from signal — skip this endpoint
                        warning(
                            "getAddedMctpInfos: {PATH} BindingType interface absent from signal, skipping endpoint",
                            "PATH", objPath.str);
                        continue;
                    }
                }
                catch (const std::exception& e)
                {
                    error(
                        "Unexpected error getting BindingType for endpoint {PATH}, error - {ERROR}",
                        "PATH", objPath.str, "ERROR", e.what());
                    continue;
                }

                if (!availability)
                {
                    // Log an error message here, but still add it to the
                    // terminus
                    error(
                        "mctpd added a DEGRADED endpoint {EID} networkId {NET} to D-Bus",
                        "NET", networkId, "EID", static_cast<unsigned>(eid));
                }
                LocalEid localEid = std::nullopt;
                if (properties.contains("LocalEID"))
                {
                    try
                    {
                        localEid =
                            std::get<mctp_eid_t>(properties.at("LocalEID"));
                    }
                    catch (const std::exception& e)
                    {
                        error(
                            "Error parsing LocalEID property for endpoint {PATH}, error - {ERROR}",
                            "PATH", objPath.str, "ERROR", e.what());
                    }
                }

                if (std::find(types.begin(), types.end(), mctpTypePLDM) !=
                    types.end())
                {
                    info(
                        "Adding Endpoint networkId '{NETWORK}' and EID '{EID}' UUID '{UUID}'",
                        "NETWORK", networkId, "EID", eid, "UUID", uuid);
                    auto mctpInfo = MctpInfo(eid, uuid, mediumType, networkId,
                                             std::nullopt, binding, localEid);
                    searchConfigurationFor(mctpInfo);
                    mctpInfos.emplace_back(std::move(mctpInfo));

                    // watch PropertiesChanged signal from
                    // au.com.codeconstruct.MCTP.Endpoint1 PDI
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
                                    "au.com.codeconstruct.MCTP.Endpoint1"),
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

void MctpDiscovery::propertiesChangedCb(sdbusplus::message_t& msg)
{
    using Interface = std::string;
    using Property = std::string;
    using Value = std::string;
    using Properties = std::map<Property, std::variant<Value>>;

    Interface interface;
    Properties properties;
    std::string objPath{};
    std::string service{};

    try
    {
        msg.read(interface, properties);
        objPath = msg.get_path();
    }
    catch (const sdbusplus::exception_t& e)
    {
        error(
            "Error handling Connectivity property changed message, error - {ERROR}",
            "ERROR", e);
        return;
    }

    for (const auto& [key, valueVariant] : properties)
    {
        Value propVal = std::get<std::string>(valueVariant);
        auto availability = (propVal == "Available") ? true : false;

        if (key == MCTPConnectivityProp)
        {
            try
            {
                service =
                    dbusHandler.getService(objPath.c_str(), MCTPInterface);
                const MctpEndpointProps& epProps =
                    getMctpEndpointProps(service, objPath);

                auto types = std::get<MCTPMsgTypes>(epProps);
                if (!std::ranges::contains(types, mctpTypePLDM))
                {
                    return;
                }
                const UUID& uuid = getEndpointUUIDProp(service, objPath);
                const auto& mctpBinding = std::get<4>(epProps);
                const auto& mctpMedium = std::get<3>(epProps);
                const auto& mctpLocalEid = std::get<5>(epProps);

                MctpInfo mctpInfo(std::get<eid>(epProps), uuid, mctpMedium,
                                  std::get<NetworkId>(epProps), std::nullopt,
                                  mctpBinding, mctpLocalEid);
                searchConfigurationFor(mctpInfo);
                if (!std::ranges::contains(existingMctpInfos, mctpInfo))
                {
                    if (availability)
                    {
                        // The endpoint not in existingMctpInfos and is
                        // available Add it to existingMctpInfos
                        info(
                            "Adding Endpoint networkId {NETWORK} ID {EID} by propertiesChanged signal",
                            "NETWORK", std::get<3>(mctpInfo), "EID",
                            unsigned(std::get<0>(mctpInfo)));
                        addToExistingMctpInfos(MctpInfos(1, mctpInfo));
                        handleMctpEndpoints(MctpInfos(1, mctpInfo));
                    }
                }
                else
                {
                    // The endpoint already in existingMctpInfos
                    updateMctpEndpointAvailability(mctpInfo, availability);
                }
            }
            catch (const sdbusplus::exception_t& e)
            {
                error(
                    "Error in propertiesChangedCb for path '{PATH}', error - {ERROR}",
                    "PATH", objPath, "ERROR", e);
                return;
            }
        }
    }
}

void MctpDiscovery::discoverEndpoints(sdbusplus::message_t& msg)
{
    MctpInfos addedInfos;
    getAddedMctpInfos(msg, addedInfos);
    for (const auto& mctpInfo : addedInfos)
    {
        lg2::info(
            "discoverEndpoints: EID {EID} added (networkId {NETWORK}) via InterfacesAdded signal",
            "EID", unsigned(std::get<pldm::eid>(mctpInfo)), "NETWORK",
            std::get<NetworkId>(mctpInfo));
    }
    addToExistingMctpInfos(addedInfos);
    loadStaticEndpoints(addedInfos);
    handleMctpEndpoints(addedInfos);
}

void MctpDiscovery::removeEndpoints(sdbusplus::message_t& msg)
{
    using ObjectPath = sdbusplus::message::object_path;
    ObjectPath objPath;
    std::vector<std::string> interfaces;

    try
    {
        msg.read(objPath, interfaces);
    }
    catch (const sdbusplus::exception_t& e)
    {
        error(
            "Error reading MCTP Endpoint removed interface message, error - {ERROR}",
            "ERROR", e);
        return;
    }

    // Only act when the MCTP Endpoint interface itself is removed.  Signals
    // for other interfaces on the same object (e.g. a Bridge interface being
    // torn down) do not mean the endpoint is gone.
    if (!std::ranges::contains(interfaces, std::string(MCTPInterface)))
    {
        return;
    }

    // Match the signal's object path against existingMctpInfos to find the
    // endpoint that was actually removed.  This avoids the racy full D-Bus
    // ObjectMapper scan that could falsely classify unrelated endpoints as
    // removed when the bus is under heavy load during host power cycles.
    auto it = std::ranges::find_if(
        existingMctpInfos, [&objPath](const auto& mctpInfo) {
            auto eidVal = std::get<pldm::eid>(mctpInfo);
            auto networkId = std::get<NetworkId>(mctpInfo);
            std::string expectedPath = std::string{MCTPPath} + "/networks/" +
                                       std::to_string(networkId) +
                                       "/endpoints/" + std::to_string(eidVal);
            return objPath.str == expectedPath;
        });

    if (it == existingMctpInfos.end())
    {
        lg2::debug(
            "removeEndpoints: MCTP endpoint removed signal for {PATH} did not match any tracked endpoint; ignoring",
            "PATH", objPath.str);
        return;
    }

    MctpInfos removedInfos{*it};
    info("Removing Endpoint EID '{EID}'", "EID", std::get<pldm::eid>(*it));
    existingMctpInfos.erase(it);

    handleRemovedMctpEndpoints(removedInfos);
    removeConfigs(removedInfos);
}

void MctpDiscovery::handleMctpEndpoints(const MctpInfos& mctpInfos)
{
    if (mctpInfos.empty())
    {
        return;
    }

    for (const auto& handler : handlers)
    {
        if (handler)
        {
            handler->handleConfigurations(configurations);
            handler->handleMctpEndpoints(mctpInfos, signalMctpInterfaces);
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

void MctpDiscovery::updateMctpEndpointAvailability(const MctpInfo& mctpInfo,
                                                   Availability availability)
{
    for (const auto& handler : handlers)
    {
        if (handler)
        {
            handler->updateMctpEndpointAvailability(mctpInfo, availability);
        }
    }
}

std::string MctpDiscovery::getNameFromProperties(
    const utils::PropertyMap& properties)
{
    if (!properties.contains("Name"))
    {
        error("Missing name property");
        return "";
    }
    return std::get<std::string>(properties.at("Name"));
}

std::string MctpDiscovery::constructMctpReactorObjectPath(
    const MctpInfo& mctpInfo)
{
    const auto networkId = std::get<NetworkId>(mctpInfo);
    const auto eid = std::get<pldm::eid>(mctpInfo);
    return std::string{MCTPPath} + "/networks/" + std::to_string(networkId) +
           "/endpoints/" + std::to_string(eid) + "/configured_by";
}

void MctpDiscovery::searchConfigurationFor(MctpInfo& mctpInfo)
{
    const auto mctpReactorObjectPath = constructMctpReactorObjectPath(mctpInfo);
    try
    {
        std::string associatedObjPath;
        std::string associatedService;
        std::string associatedInterface;
        sdbusplus::message::object_path inventorySubtreePath(
            inventorySubtreePathStr);

        //"/{board or chassis type}/{board or chassis}/{device}"
        auto constexpr subTreeDepth = 3;
        auto response = dbusHandler.getAssociatedSubTree(
            mctpReactorObjectPath, inventorySubtreePath, subTreeDepth,
            interfaceFilter);
        if (response.empty())
        {
            warning("No associated subtree found for path {PATH}", "PATH",
                    mctpReactorObjectPath);
            return;
        }
        // Assume the first entry is the one we want
        auto subTree = response.begin();
        associatedObjPath = subTree->first;
        auto associatedServiceProp = subTree->second;
        if (associatedServiceProp.empty())
        {
            warning("No associated service found for path {PATH}", "PATH",
                    mctpReactorObjectPath);
            return;
        }
        // Assume the first entry is the one we want
        auto entry = associatedServiceProp.begin();
        associatedService = entry->first;
        auto dBusIntfList = entry->second;
        auto associatedInterfaceItr = std::find_if(
            dBusIntfList.begin(), dBusIntfList.end(), [](const auto& intf) {
                return std::find(interfaceFilter.begin(), interfaceFilter.end(),
                                 intf) != interfaceFilter.end();
            });
        if (associatedInterfaceItr == dBusIntfList.end())
        {
            error("No associated interface found for path {PATH}", "PATH",
                  mctpReactorObjectPath);
            return;
        }
        associatedInterface = *associatedInterfaceItr;
        auto mctpTargetProperties = dbusHandler.getDbusPropertiesVariant(
            associatedService.c_str(), associatedObjPath.c_str(),
            associatedInterface.c_str());
        auto name = getNameFromProperties(mctpTargetProperties);
        if (!name.empty())
        {
            std::get<std::optional<std::string>>(mctpInfo) = name;
        }
        configurations.emplace(associatedObjPath, mctpInfo);
    }
    catch (const std::exception& e)
    {
        error(
            "Error getting associated subtree for path {PATH}, error - {ERROR}",
            "PATH", mctpReactorObjectPath, "ERROR", e);
        return;
    }
}

void MctpDiscovery::removeConfigs(const MctpInfos& removedInfos)
{
    for (const auto& mctpInfo : removedInfos)
    {
        auto eidToRemove = std::get<eid>(mctpInfo);
        std::erase_if(configurations, [eidToRemove](const auto& config) {
            auto& [__, mctpInfo] = config;
            auto eidValue = std::get<eid>(mctpInfo);
            return eidValue == eidToRemove;
        });
    }
}

void MctpDiscovery::refreshEndpoints(sdbusplus::message::message& msg)
{
    std::string interface;
    pldm::dbus::PropertyMap properties;
    std::string objPath = msg.get_path();
    std::string service = msg.get_sender();

    msg.read(interface, properties);
    auto prop = properties.find("Connectivity");
    if (prop != properties.end())
    {
        auto connectivity = std::get<std::string>(prop->second);
        info(
            "Received au.com.codeconstruct.MCTP.Endpoint1 PropertiesChanged signal for "
            "Connectivity={CONN} at PATH={OBJ_PATH} from SERVICE={SERVICE}",
            "CONN", connectivity, "OBJ_PATH", objPath, "SERVICE", service);

        for (MctpDiscoveryHandlerIntf* handler : handlers)
        {
            try
            {
                const auto uuid =
                    std::get<std::string>(dbusHandler.getDbusPropertyVariant(
                        objPath.c_str(), "UUID",
                        "xyz.openbmc_project.Common.UUID"));

                const auto eid =
                    std::get<uint8_t>(dbusHandler.getDbusPropertyVariant(
                        objPath.c_str(), "EID",
                        "xyz.openbmc_project.MCTP.Endpoint"));

                if (connectivity == "Available")
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
                error(
                    "refreshEndpoints: failed to get UUID for {PATH}, error - {ERROR}",
                    "PATH", objPath, "ERROR", e);
            }
        }
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
            mctpInfos.emplace_back(MctpInfo(eid, emptyUUID, {}, {},
                                            std::nullopt, {}, std::nullopt));
        }
    }
}

} // namespace pldm
