#include "config.h"

#include "mctp_endpoint_discovery.hpp"

#include "common/types.hpp"
#include "common/utils.hpp"
#include "mctp_endpoint_discovery_typed_accessors.hpp"

#include <linux/mctp.h>

#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <thread>
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

namespace
{

/** @brief Resolve the bus-owner service name implementing MCTPInterface at
 *         the MCTPPath subtree via ObjectMapper.GetSubTree.
 *
 *  Per unify-mctp_discovery_guidelines.md § 2.1 Phase 1.A and § 2.4
 *  Anti-patterns, consumers MUST resolve the bus-owner dynamically and use
 *  the resolved name in the `sender=` filter for subscription matches —
 *  not hardcode a constant. Today the only owner is
 *  `au.com.codeconstruct.MCTP1`, but pinning to that constant locks the
 *  daemon to a specific upstream choice; resolution at startup keeps the
 *  consumer bus-owner-implementation-agnostic.
 *
 *  Falls back to the legacy `MCTPService` constant on any failure so the
 *  constructor's match-rule initialisers can still proceed. Bounded retry
 *  on mapper failure lands in a later commit in this series.
 */
std::string resolveBusOwner(pldm::utils::DBusHandlerInterface& dbusHandler)
{
    try
    {
        auto resp = dbusHandler.getSubtree(
            pldm::MCTPPath, /*depth=*/0,
            std::vector<std::string>({pldm::MCTPInterface}));
        if (!resp.empty() && !resp.begin()->second.empty())
        {
            return resp.begin()->second.begin()->first;
        }
        info("resolveBusOwner: mapper returned no service for MCTP subtree; "
             "falling back to legacy service constant {SERVICE}",
             "SERVICE", std::string(pldm::MCTPService));
    }
    catch (const sdbusplus::exception_t& e)
    {
        error("resolveBusOwner: getSubtree threw; falling back to legacy "
              "service constant {SERVICE}, error - {ERROR}",
              "SERVICE", std::string(pldm::MCTPService), "ERROR", e);
    }
    catch (const std::exception& e)
    {
        error("resolveBusOwner: unexpected error; falling back to legacy "
              "service constant {SERVICE}, error - {ERROR}",
              "SERVICE", std::string(pldm::MCTPService), "ERROR", e.what());
    }
    return pldm::MCTPService;
}

/** @brief Resolve the service publishing endpoint identity — the
 *         Association.Definitions (configured_by) objects under the MCTP
 *         subtree — via ObjectMapper.GetSubTree, mirroring resolveBusOwner.
 *
 *  Discovery is driven by the identity publisher's InterfacesAdded signal,
 *  so the `sender=` filter must name whichever service owns those objects
 *  (today mctpreactor) rather than hardcode it.
 *
 *  At pldmd startup the publisher may not have configured any endpoint yet,
 *  so an empty mapper response is expected — fall back to the
 *  MCTPReactorService constant. Sender matching by well-known name applies
 *  once the name is owned, so the fallback works even when the publisher
 *  starts later.
 */
std::string resolveIdentityOwner(pldm::utils::DBusHandlerInterface& dbusHandler)
{
    try
    {
        auto resp = dbusHandler.getSubtree(
            pldm::MCTPPath, /*depth=*/0,
            std::vector<std::string>({pldm::MCTPReactorConfiguredInterface}));
        if (!resp.empty() && !resp.begin()->second.empty())
        {
            return resp.begin()->second.begin()->first;
        }
        info("resolveIdentityOwner: no configured endpoints published yet; "
             "falling back to service constant {SERVICE}",
             "SERVICE", std::string(pldm::MCTPReactorService));
    }
    catch (const std::exception& e)
    {
        error("resolveIdentityOwner: lookup failed; falling back to service "
              "constant {SERVICE}, error - {ERROR}",
              "SERVICE", std::string(pldm::MCTPReactorService), "ERROR",
              e.what());
    }
    return pldm::MCTPReactorService;
}

} // namespace

MctpDiscovery::MctpDiscovery(
    sdbusplus::bus_t& bus,
    std::initializer_list<MctpDiscoveryHandlerIntf*> list,
    const std::filesystem::path& staticEidTablePath,
    pldm::utils::DBusHandlerInterface& dbusHandler,
    const std::vector<std::chrono::milliseconds>& retryBackoffOverride) :
    bus(bus), resolvedMctpService(resolveBusOwner(dbusHandler)),
    mctpEndpointRemovedSignal(
        bus,
        interfacesRemovedAtPath(MCTPNetworksPath) + sender(resolvedMctpService),
        [this](sdbusplus::message_t& msg) { this->removeEndpoints(msg); }),
    resolvedIdentityService(resolveIdentityOwner(dbusHandler)),
    mctpReactorConfiguredSignal(bus,
                                interfacesAddedAtPath(MCTPNetworksPath) +
                                    sender(resolvedIdentityService),
                                [this](sdbusplus::message_t& msg) {
                                    this->onMctpReactorConfigured(msg);
                                }),
    handlers(list), staticEidTablePath(staticEidTablePath),
    dbusHandler(dbusHandler)
{
    // Apply caller-supplied retry override (test-only mechanism) before
    // getMctpInfos consults retryBackoff for its bounded-retry loop.
    if (!retryBackoffOverride.empty())
    {
        retryBackoff = retryBackoffOverride;
    }

    std::map<MctpInfo, Availability> currentMctpInfoMap;
    const bool mapperOk = getMctpInfos(currentMctpInfoMap);
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

    // Per unify-mctp_discovery_guidelines.md mandatory item 6
    // ("daemon does not publish an empty / partial inventory before at
    // least one GetManagedObjects round succeeds"), only invoke
    // handleMctpEndpoints if mapper enumeration succeeded — including the
    // mapper-healthy-but-no-endpoints case (mapperOk=true, empty list is
    // the truth). When mapperOk=false the daemon stays in a "waiting for
    // endpoints" state; subsequent endpoints arriving via the
    // InterfacesAdded signal will then publish via discoverEndpoints.
    if (mapperOk)
    {
        handleMctpEndpoints(existingMctpInfos);
    }
    else
    {
        warning("MctpDiscovery: mapper unhealthy at boot after bounded retry; "
                "deferring inventory publication until first successful "
                "InterfacesAdded signal");
    }
}

bool MctpDiscovery::getMctpInfos(std::map<MctpInfo, Availability>& mctpInfoMap)
{
    // Enumerate the endpoints mctpreactor has configured (those exposing its
    // configured_by association); endpoint properties are read from mctpd.
    //
    // Per unify-mctp_discovery_guidelines.md mandatory item 7, retry on
    // mapper failure with bounded exponential backoff. Schedule is taken
    // from the retryBackoff member so the TestMctpDiscovery fixture can
    // shrink it for unit-test runtime.
    pldm::utils::GetSubTreeResponse mapperResponse;
    auto attempt = [&]() -> bool {
        try
        {
            mapperResponse = dbusHandler.getSubtree(
                MCTPPath, 0,
                std::vector<std::string>({MCTPReactorConfiguredInterface}));
            return true;
        }
        catch (const sdbusplus::exception_t& e)
        {
            error("getMctpInfos: getSubtree threw at path '{PATH}' interface "
                  "'{INTERFACE}', error - {ERROR}",
                  "PATH", std::string(MCTPPath), "INTERFACE",
                  std::string(MCTPReactorConfiguredInterface), "ERROR", e);
            return false;
        }
    };

    bool ok = attempt();
    for (size_t i = 0; !ok && i < retryBackoff.size(); ++i)
    {
        info("getMctpInfos: mapper retry {ATTEMPT} in {DELAY_MS}ms", "ATTEMPT",
             static_cast<unsigned>(i + 1), "DELAY_MS",
             static_cast<unsigned long long>(retryBackoff[i].count()));
        std::this_thread::sleep_for(retryBackoff[i]);
        ok = attempt();
    }
    if (!ok)
    {
        error("getMctpInfos: mapper unhealthy after {N} retries; deferring "
              "inventory publication",
              "N", static_cast<unsigned>(retryBackoff.size()));
        return false;
    }

    for (const auto& mapperEntry : mapperResponse)
    {
        // The subtree is filtered on mctpreactor's configured_by interface, so
        // every path here is an endpoint mctpreactor has configured. Every
        // endpoint property is still read from the mctpd object at this path.
        const std::string& path = mapperEntry.first;
        std::string service;
        try
        {
            service = dbusHandler.getService(path.c_str(), MCTPInterface);
        }
        catch (const sdbusplus::exception_t& e)
        {
            // mctpreactor configured this endpoint but mctpd has not (yet)
            // published the MCTP.Endpoint object; a later InterfacesAdded
            // signal will pick it up.
            error(
                "getMctpInfos: no mctpd MCTP.Endpoint at path '{PATH}', error - {ERROR}",
                "PATH", path, "ERROR", e);
            continue;
        }

        const MctpEndpointProps& epProps = getMctpEndpointProps(service, path);
        const UUID& uuid = getEndpointUUIDProp(service, path);
        const Availability& availability = getEndpointConnectivityProp(path);
        auto types = std::get<MCTPMsgTypes>(epProps);
        if (std::find(types.begin(), types.end(), mctpTypePLDM) == types.end())
        {
            continue;
        }
        const auto& mctpBinding = std::get<4>(epProps);
        const auto& mctpMedium = std::get<3>(epProps);
        const auto& mctpLocalEid = std::get<5>(epProps);
        auto mctpInfo = MctpInfo(std::get<eid>(epProps), uuid, mctpMedium,
                                 std::get<NetworkId>(epProps), std::nullopt,
                                 mctpBinding, mctpLocalEid);
        // Every path in this enumeration carries the configured_by
        // association (the subtree query above is filtered on it), so a
        // failed lookup after retries means the mapper is unhealthy — skip
        // the endpoint rather than create unnamed inventory; it is picked up
        // on the next daemon start.
        if (!searchConfigurationWithRetry(mctpInfo))
        {
            warning(
                "getMctpInfos: configuration unresolved for EID {EID} after retries; skipping endpoint",
                "EID", static_cast<unsigned>(std::get<eid>(epProps)));
            continue;
        }
        mctpInfoMap[std::move(mctpInfo)] = availability;

        // Watch for PropertiesChanged signal from
        // xyz.openbmc_project.Object.Enable PDI
        auto [_, inserted] = enableMatches.try_emplace(
            path, bus,
            sdbusplus::bus::match::rules::propertiesChanged(
                path.c_str(), "au.com.codeconstruct.MCTP.Endpoint1"),
            std::bind_front(&MctpDiscovery::refreshEndpoints, this));
        if (inserted)
        {
            info("register match_t path:{OBJPATH}", "OBJPATH", path);
        }
    }

    // configured_by is primary; bind any remaining StaticEID-declared devices
    // (statically-assigned, bridge-routed endpoints with no configured_by).
    bindStaticEidConfigurations(mctpInfoMap);
    return true;
}

void MctpDiscovery::bindStaticEidConfigurations(
    std::map<MctpInfo, Availability>& mctpInfoMap)
{
    constexpr auto pldmFwDeviceIntf =
        "xyz.openbmc_project.Configuration.PLDMFirmwareDevice";

    // 1. Collect StaticEID -> (MCTPTargetName, config object path) from EM.
    std::map<eid, std::pair<std::string, std::string>> staticEidToName;
    pldm::utils::GetSubTreeResponse fwSubtree;
    try
    {
        fwSubtree = dbusHandler.getSubtree("/xyz/openbmc_project/inventory", 0,
                                           {pldmFwDeviceIntf});
    }
    catch (const std::exception& e)
    {
        // No PLDMFirmwareDevice configs published; nothing to bind.
        return;
    }
    for (const auto& [objPath, serviceMap] : fwSubtree)
    {
        if (serviceMap.empty())
        {
            continue;
        }
        const std::string service = serviceMap.begin()->first;
        pldm::utils::PropertyMap props;
        try
        {
            props = dbusHandler.getDbusPropertiesVariant(
                service.c_str(), objPath.c_str(), pldmFwDeviceIntf);
        }
        catch (const std::exception& e)
        {
            warning(
                "bindStaticEidConfigurations: reading props at {PATH} failed, error - {ERROR}; skipping",
                "PATH", objPath, "ERROR", e);
            continue;
        }
        auto staticEid =
            pldm::utils::readOptionalEidProperty(props, "StaticEID");
        if (!staticEid)
        {
            continue; // device relies on configured_by
        }
        auto nameIt = props.find("MCTPTargetName");
        if (nameIt == props.end())
        {
            continue;
        }
        const auto* namePtr = std::get_if<std::string>(&nameIt->second);
        if (namePtr == nullptr)
        {
            warning(
                "bindStaticEidConfigurations: MCTPTargetName at {PATH} is not a string; skipping",
                "PATH", objPath);
            continue;
        }
        staticEidToName.emplace(*staticEid, std::make_pair(*namePtr, objPath));
    }
    if (staticEidToName.empty())
    {
        return;
    }

    // 2. Enumerate live mctpd endpoints; bind any whose EID matches a StaticEID
    //    and is not already resolved via configured_by.
    pldm::utils::GetSubTreeResponse epSubtree;
    try
    {
        epSubtree =
            dbusHandler.getSubtree(MCTPPath, 0, {std::string(MCTPInterface)});
    }
    catch (const std::exception& e)
    {
        warning(
            "bindStaticEidConfigurations: enumerating MCTP endpoints failed, error - {ERROR}",
            "ERROR", e);
        return;
    }
    for (const auto& [path, serviceMap] : epSubtree)
    {
        if (serviceMap.empty())
        {
            continue;
        }
        const std::string service = serviceMap.begin()->first;
        MctpEndpointProps epProps;
        try
        {
            epProps = getMctpEndpointProps(service, path);
        }
        catch (const std::exception&)
        {
            continue;
        }
        const auto types = std::get<MCTPMsgTypes>(epProps);
        if (std::find(types.begin(), types.end(), mctpTypePLDM) == types.end())
        {
            continue;
        }
        const auto epEid = std::get<eid>(epProps);
        auto bind = staticEidToName.find(epEid);
        if (bind == staticEidToName.end())
        {
            continue;
        }
        // configured_by is authoritative: skip if already resolved.
        const bool alreadyResolved =
            std::ranges::any_of(mctpInfoMap, [epEid](const auto& kv) {
                return std::get<pldm::eid>(kv.first) == epEid;
            });
        if (alreadyResolved)
        {
            continue;
        }
        const UUID& uuid = getEndpointUUIDProp(service, path);
        const Availability& availability = getEndpointConnectivityProp(path);
        const auto& mctpBinding = std::get<4>(epProps);
        const auto& mctpMedium = std::get<3>(epProps);
        const auto& mctpLocalEid = std::get<5>(epProps);
        MctpInfo mctpInfo(epEid, uuid, mctpMedium, std::get<NetworkId>(epProps),
                          bind->second.first, mctpBinding, mctpLocalEid);
        // Key the configurations map on the EM config path (as the
        // configured_by path does) so em_config::targetNameForEid resolves
        // the name.
        configurations.emplace(bind->second.second, mctpInfo);
        mctpInfoMap[std::move(mctpInfo)] = availability;
        info(
            "bindStaticEidConfigurations: bound EID {EID} to '{NAME}' via StaticEID",
            "EID", static_cast<unsigned>(epEid), "NAME", bind->second.first);
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

        // Typed accessor: returns nullopt if "UUID" is absent OR the
        // variant holds a non-string alternative. Preserves the prior
        // "log error, return emptyUUID" behaviour for both failure modes
        // while eliminating the raw std::get<>.
        //
        // Note: previously a wrong-type UUID variant threw
        // std::bad_variant_access which escaped past the catch and could
        // reach event-loop dispatch. The accessor-version is contractually
        // immune to that.
        if (const auto uuid =
                pldm::dbus_accessors::tryGetProp<UUID>(properties, "UUID"))
        {
            return *uuid;
        }
        error(
            "UUID property absent or wrong type for endpoint at path '{PATH}' and service '{SERVICE}'",
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
        // Typed accessor: returns nullopt instead of throwing if the
        // variant holds a non-string alternative. Same observable
        // behaviour as the old throw-and-catch path (return false).
        const auto conn =
            pldm::dbus_accessors::tryGet<std::string>(propertyValue);
        if (conn && *conn == "Available")
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
    using ObjectPath = sdbusplus::object_path;
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
            // Wrap std::get<std::string> against bad_variant_access — if the
            // variant arrives with a non-string alternative, default to
            // Unavailable rather than terminate. Typed-accessor migration in
            // a follow-up commit removes the raw std::get from this site.
            try
            {
                availability =
                    (std::get<std::string>(ccProps.at(MCTPConnectivityProp)) ==
                     "Available");
            }
            catch (const std::bad_variant_access& e)
            {
                warning(
                    "getAddedMctpInfos: {PATH} Connectivity variant wrong type, defaulting to Unavailable",
                    "PATH", objPath.str);
                availability = false;
            }
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
            // Wrap std::get<UUID> against bad_variant_access — if the
            // variant arrives with a non-string alternative, drop the
            // endpoint rather than terminate the daemon.
            try
            {
                uuid = std::get<UUID>(uuidProps.at("UUID"));
            }
            catch (const std::bad_variant_access& e)
            {
                error(
                    "getAddedMctpInfos: {PATH} UUID variant wrong type, dropping endpoint",
                    "PATH", objPath.str);
                return;
            }
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
        if (intfName == MCTPEndpoint::interface)
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
                    auto [_2, ins2] = enableMatches.try_emplace(
                        objPath.str, bus,
                        sdbusplus::bus::match::rules::propertiesChanged(
                            objPath.str, "au.com.codeconstruct.MCTP.Endpoint1"),
                        std::bind_front(&MctpDiscovery::refreshEndpoints,
                                        this));
                    if (ins2)
                    {
                        info("register match_t objectPath:{OBJPATH}", "OBJPATH",
                             objPath.str);
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

void MctpDiscovery::onMctpReactorConfigured(sdbusplus::message_t& msg)
{
    // The runtime discovery trigger: the identity publisher (mctpreactor) has
    // added the configured_by association on an mctpd endpoint. The
    // association is published only after mctpd's endpoint object exists, so
    // the endpoint properties can be read back from mctpd and the EM
    // configuration resolved here — an endpoint is never processed before its
    // identity exists. No-op if the endpoint was already
    // created with a resolved name.
    sdbusplus::object_path objPath;
    try
    {
        // InterfacesAdded payload is (object_path, a{sa{sv}}); we only need the
        // path, then read the endpoint's properties from mctpd directly.
        msg.read(objPath);
    }
    catch (const sdbusplus::exception_t& e)
    {
        error(
            "Error reading mctpreactor InterfacesAdded message, error - {ERROR}",
            "ERROR", e);
        return;
    }

    // mctpreactor publishes the association on the endpoint object path itself
    // (.../endpoints/<eid>). Ignore anything that is not a bare endpoint path
    // (e.g. an ObjectMapper-synthesised .../configured_by child object).
    const std::string& path = objPath.str;
    const std::string endpointsTag = "/endpoints/";
    const auto endpointsPos = path.find(endpointsTag);
    if (path.compare(0, std::string(MCTPPath).size(), MCTPPath) != 0 ||
        endpointsPos == std::string::npos)
    {
        return;
    }
    const auto eidStr = path.substr(endpointsPos + endpointsTag.size());
    if (eidStr.empty() ||
        eidStr.find_first_not_of("0123456789") != std::string::npos)
    {
        return;
    }

    try
    {
        const std::string service =
            dbusHandler.getService(path.c_str(), MCTPInterface);
        const MctpEndpointProps& epProps = getMctpEndpointProps(service, path);

        auto types = std::get<MCTPMsgTypes>(epProps);
        if (!std::ranges::contains(types, mctpTypePLDM))
        {
            return;
        }

        const UUID& uuid = getEndpointUUIDProp(service, path);
        const auto& mctpBinding = std::get<4>(epProps);
        const auto& mctpMedium = std::get<3>(epProps);
        const auto& mctpLocalEid = std::get<5>(epProps);

        MctpInfo mctpInfo(std::get<eid>(epProps), uuid, mctpMedium,
                          std::get<NetworkId>(epProps), std::nullopt,
                          mctpBinding, mctpLocalEid);

        // The startup enumeration and this signal can overlap for an endpoint
        // configured around daemon start, so dedup by endpoint identity
        // (network + EID, the D-Bus path key). The configured-name field is
        // deliberately excluded from the comparison: it is filled on the
        // stored entry but still empty on this probe.
        const auto probeEid = std::get<pldm::eid>(mctpInfo);
        const auto probeNet = std::get<NetworkId>(mctpInfo);
        if (std::ranges::any_of(
                existingMctpInfos, [probeEid, probeNet](const MctpInfo& info) {
                    return std::get<pldm::eid>(info) == probeEid &&
                           std::get<NetworkId>(info) == probeNet;
                }))
        {
            return;
        }

        if (!searchConfigurationWithRetry(mctpInfo))
        {
            return;
        }

        // Track this endpoint's Connectivity (availability) changes, matching
        // the per-endpoint watch the old mctpd-driven path used to register.
        enableMatches.try_emplace(
            path, bus,
            sdbusplus::bus::match::rules::propertiesChanged(path,
                                                            MCTPInterfaceCC),
            std::bind_front(&MctpDiscovery::refreshEndpoints, this));

        info(
            "configured_by published by mctpreactor for EID {EID}; creating named firmware inventory",
            "EID", static_cast<unsigned>(std::get<pldm::eid>(mctpInfo)));
        addToExistingMctpInfos(MctpInfos(1, mctpInfo));
        handleMctpEndpoints(MctpInfos(1, mctpInfo));
    }
    catch (const std::exception& e)
    {
        error(
            "onMctpReactorConfigured error for path '{PATH}', error - {ERROR}",
            "PATH", path, "ERROR", e.what());
    }
}

void MctpDiscovery::removeEndpoints(sdbusplus::message_t& msg)
{
    using ObjectPath = sdbusplus::object_path;
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

bool MctpDiscovery::searchConfigurationWithRetry(MctpInfo& mctpInfo)
{
    // pldmd and ObjectMapper consume the same identity-publication signal, so
    // the association can be queried here before the mapper has ingested it;
    // under boot-time load the lookup can also time out outright. Bounded
    // backoff covers both.
    if (searchConfigurationFor(mctpInfo))
    {
        return true;
    }
    for (const auto& delay : retryBackoff)
    {
        std::this_thread::sleep_for(delay);
        if (searchConfigurationFor(mctpInfo))
        {
            return true;
        }
    }
    warning(
        "searchConfigurationWithRetry: configuration unresolved for EID {EID} after {N} retries",
        "EID", static_cast<unsigned>(std::get<eid>(mctpInfo)), "N",
        retryBackoff.size());
    return false;
}

bool MctpDiscovery::searchConfigurationFor(MctpInfo& mctpInfo)
{
    const auto mctpReactorObjectPath = constructMctpReactorObjectPath(mctpInfo);
    try
    {
        std::string associatedObjPath;
        std::string associatedService;
        std::string associatedInterface;
        sdbusplus::object_path inventorySubtreePath(inventorySubtreePathStr);

        //"/{board or chassis type}/{board or chassis}/{device}"
        auto constexpr subTreeDepth = 3;
        auto response = dbusHandler.getAssociatedSubTree(
            mctpReactorObjectPath, inventorySubtreePath, subTreeDepth,
            interfaceFilter);
        if (response.empty())
        {
            warning("No associated subtree found for path {PATH}", "PATH",
                    mctpReactorObjectPath);
            return false;
        }
        // Assume the first entry is the one we want
        auto subTree = response.begin();
        associatedObjPath = subTree->first;
        auto associatedServiceProp = subTree->second;
        if (associatedServiceProp.empty())
        {
            warning("No associated service found for path {PATH}", "PATH",
                    mctpReactorObjectPath);
            return false;
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
            return false;
        }
        associatedInterface = *associatedInterfaceItr;
        auto mctpTargetProperties = dbusHandler.getDbusPropertiesVariant(
            associatedService.c_str(), associatedObjPath.c_str(),
            associatedInterface.c_str());
        auto name = getNameFromProperties(mctpTargetProperties);
        if (name.empty())
        {
            // Leave the map untouched so a later retry can record the
            // resolved entry; emplace would pin the nameless one forever.
            return false;
        }
        std::get<std::optional<std::string>>(mctpInfo) = name;
        configurations.insert_or_assign(associatedObjPath, mctpInfo);
        return true;
    }
    catch (const std::exception& e)
    {
        error(
            "Error getting associated subtree for path {PATH}, error - {ERROR}",
            "PATH", mctpReactorObjectPath, "ERROR", e);
        return false;
    }
}

void MctpDiscovery::removeConfigs(const MctpInfos& removedInfos)
{
    for (const auto& mctpInfo : removedInfos)
    {
        const auto eidToRemove = std::get<eid>(mctpInfo);
        const auto netToRemove = std::get<NetworkId>(mctpInfo);

        std::erase_if(configurations, [eidToRemove,
                                       netToRemove](const auto& config) {
            const auto& [__, mctpInfo] = config;
            const auto eidValue = std::get<eid>(mctpInfo);
            const auto netValue = std::get<NetworkId>(mctpInfo);

            return eidValue == eidToRemove && netValue == netToRemove;
        });
    }
}

void MctpDiscovery::refreshEndpoints(sdbusplus::message::message& msg)
{
    // Outer try-catch belt: msg.read() can throw sdbusplus::exception_t on
    // malformed payloads; std::get<std::string>(prop->second) can throw
    // std::bad_variant_access if the Connectivity variant arrives with a
    // non-string alternative. Per unify-mctp_discovery_guidelines.md § 2.2
    // mandatory item 5, no uncaught exception may escape this callback —
    // that would propagate to sd-event and cause std::terminate.
    try
    {
        std::string interface;
        pldm::dbus::PropertyMap properties;
        std::string objPath = msg.get_path();
        std::string service = msg.get_sender();

        msg.read(interface, properties);
        auto prop = properties.find("Connectivity");
        if (prop == properties.end())
        {
            return;
        }

        const auto* connPtr = std::get_if<std::string>(&prop->second);
        if (connPtr == nullptr)
        {
            error("refreshEndpoints: Connectivity property is not a string");
            return;
        }
        const auto& connectivity = *connPtr;
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
                    "refreshEndpoints: failed to get UUID/EID for {PATH}, error - {ERROR}",
                    "PATH", objPath, "ERROR", e);
            }
        }
    }
    catch (const sdbusplus::exception_t& e)
    {
        error("refreshEndpoints: bad signal payload, error - {ERROR}", "ERROR",
              e);
    }
    catch (const std::exception& e)
    {
        error("refreshEndpoints: unexpected error, error - {ERROR}", "ERROR",
              e.what());
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
