#pragma once

#include "common/types.hpp"
#include "common/utils.hpp"

#include <libpldm/pldm.h>

#include <sdbusplus/bus/match.hpp>
#include <xyz/openbmc_project/Common/UUID/common.hpp>
#include <xyz/openbmc_project/MCTP/Endpoint/client.hpp>

#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <vector>

using MCTPEndpoint = sdbusplus::common::xyz::openbmc_project::mctp::Endpoint;
using CommonUUID = sdbusplus::common::xyz::openbmc_project::common::UUID;

class TestMctpDiscovery;

namespace pldm
{

const std::string emptyUUID = "00000000-0000-0000-0000-000000000000";
constexpr const char* MCTPService = "au.com.codeconstruct.MCTP1";
constexpr const char* MCTPReactorService = "xyz.openbmc_project.MCTPReactor";
/** @brief Interface mctpreactor publishes on an endpoint to assert
 *         configured_by; used to enumerate mctpreactor-configured endpoints. */
constexpr const char* MCTPReactorConfiguredInterface =
    "xyz.openbmc_project.Association.Definitions";
constexpr const char* MCTPInterface = MCTPEndpoint::interface;
constexpr const char* MCTPBindingInterface = "xyz.openbmc_project.MCTP.Binding";
constexpr const char* EndpointUUID = CommonUUID::interface;
constexpr const char* MCTPPath = "/au/com/codeconstruct/mctp1";
constexpr const char* MCTPNetworksPath =
    "/au/com/codeconstruct/mctp1/networks/";
constexpr const char* MCTPInterfaceCC = "au.com.codeconstruct.MCTP.Endpoint1";
constexpr const char* MCTPConnectivityProp = "Connectivity";
constexpr const char* inventorySubtreePathStr =
    "/xyz/openbmc_project/inventory/system";

const std::vector<std::string> interfaceFilter = {
    "xyz.openbmc_project.Configuration.MCTPI2CTarget",
    "xyz.openbmc_project.Configuration.MCTPI3CTarget",
    "xyz.openbmc_project.Configuration.MCTPUSBDevice",
    "xyz.openbmc_project.Configuration.MCTPSPIDevice",
    "xyz.openbmc_project.Configuration.MCTPBridgePoolDevice"};

/** @class MctpDiscoveryHandlerIntf
 *
 * This abstract class defines the APIs for MctpDiscovery class has common
 * interface to execute function from different Manager Classes
 */
class MctpDiscoveryHandlerIntf
{
  public:
    virtual void handleMctpEndpoints(
        const MctpInfos& mctpInfos,
        const dbus::MctpInterfaces& mctpInterfaces) = 0;
    virtual void handleRemovedMctpEndpoints(const MctpInfos& mctpInfos) = 0;
    virtual void updateMctpEndpointAvailability(const MctpInfo& mctpInfo,
                                                Availability availability) = 0;
    /** @brief Get Active EIDs.
     *
     *  @param[in] addr - MCTP address of terminus
     *  @param[in] terminiNames - MCTP terminus name
     */
    virtual std::optional<mctp_eid_t> getActiveEidByName(
        const std::string& terminusName) = 0;

    virtual void handleConfigurations(const Configurations& /*configurations*/)
    {}
    virtual ~MctpDiscoveryHandlerIntf() {}
    virtual void onlineMctpEndpoint([[maybe_unused]] const UUID& uuid,
                                    [[maybe_unused]] const eid& eid)
    {}
    virtual void offlineMctpEndpoint([[maybe_unused]] const UUID& uuid,
                                     [[maybe_unused]] const eid& eid)
    {}
};

class MctpDiscovery
{
  public:
    MctpDiscovery() = delete;
    MctpDiscovery(const MctpDiscovery&) = delete;
    MctpDiscovery(MctpDiscovery&&) = delete;
    MctpDiscovery& operator=(const MctpDiscovery&) = delete;
    MctpDiscovery& operator=(MctpDiscovery&&) = delete;
    virtual ~MctpDiscovery() = default;

    /** @brief Constructs the MCTP Discovery object to handle discovery of
     *         MCTP enabled devices
     *
     *  @param[in] bus - reference to systemd bus
     *  @param[in] list - initializer list to the MctpDiscoveryHandlerIntf
     *  @param[in] staticEidTablePath - Path to Static EID Table file
     *  @param[in] dbusHandler - D-Bus handler for discovery lookups
     *  @param[in] retryBackoffOverride - optional mapper-retry backoff
     *             schedule. When non-empty, replaces the production
     *             retryBackoff member before any mapper call is made by the
     *             constructor. Used by tests to shrink retry latency. An
     *             empty vector means "use the default production schedule".
     */
    explicit MctpDiscovery(
        sdbusplus::bus_t& bus,
        std::initializer_list<MctpDiscoveryHandlerIntf*> list,
        const std::filesystem::path& staticEidTablePath = STATIC_EID_TABLE_PATH,
        pldm::utils::DBusHandlerInterface& dbusHandler = defaultDbusHandler(),
        const std::vector<std::chrono::milliseconds>& retryBackoffOverride =
            {});

    /** @brief reference to the systemd bus */
    sdbusplus::bus_t& bus;

    /** @brief Cached bus-owner service name resolved from
     *         ObjectMapper.GetSubTree(MCTPPath, [MCTPInterface]) at
     *         construction. Used as the `sender=` filter for the
     *         InterfacesRemoved match rule so the daemon does NOT
     *         hardcode `au.com.codeconstruct.MCTP1` (or any future
     *         alternative bus-owner name) in its source.
     *
     *         If the mapper lookup fails or returns no matching service the
     *         fallback is the legacy `MCTPService` constant — see
     *         resolveBusOwner() in the .cpp.
     *
     *         Declared BEFORE the match members so it is initialised first
     *         in the constructor initialiser list. */
    const std::string resolvedMctpService;

    /** @brief Used to watch for the removed MCTP endpoints */
    sdbusplus::bus::match_t mctpEndpointRemovedSignal;

    /** @brief Cached service name of the endpoint-identity publisher — the
     *         owner of the Association.Definitions (configured_by) objects
     *         under the MCTP subtree (today mctpreactor) — resolved at
     *         construction via resolveIdentityOwner() in the .cpp; falls
     *         back to the MCTPReactorService constant when no endpoint is
     *         configured yet. Used as the `sender=` filter for the discovery
     *         match below.
     *
     *         Declared BEFORE the match member so it is initialised first
     *         in the constructor initialiser list. */
    const std::string resolvedIdentityService;

    /** @brief The runtime endpoint-discovery trigger: the identity publisher
     *         (mctpreactor) adding the configured_by association on an
     *         endpoint. The association is published only after mctpd's
     *         endpoint object exists, so when this fires both the endpoint
     *         properties (read back from mctpd) and the EM identity are
     *         available — endpoints are never processed before their
     *         identity exists (DGXOPENBMC-25121). */
    sdbusplus::bus::match_t mctpReactorConfiguredSignal;

    /** @brief List of handlers need to notify when new MCTP
     * Endpoint is Added/Removed */
    std::vector<MctpDiscoveryHandlerIntf*> handlers;

    /** @brief The existing MCTP endpoints */
    MctpInfos existingMctpInfos;

    /** @brief Cache of UUID → InterfaceMap built from InterfacesAdded signal
     *         payloads. Used to avoid ObjectMapper calls in handleMctpEndpoints
     *         during boot (when ObjectMapper may not have processed the signal
     *         yet). Populated by getAddedMctpInfos(). */
    dbus::MctpInterfaces signalMctpInterfaces;

    /** @brief Path of static eid table config file */
    std::filesystem::path staticEidTablePath;

    /** @brief D-Bus handler used for endpoint discovery lookups */
    pldm::utils::DBusHandlerInterface& dbusHandler;
    /**
     * @brief matcher rule for property changes of
     * xyz.openbmc_project.Object.Enable dbus object
     */
    std::map<std::string, sdbusplus::bus::match_t> enableMatches;

    /** @brief Process an mctpd InterfacesAdded payload: parse the endpoint
     * properties out of the message and register the endpoints. Runtime
     * discovery is driven by onMctpReactorConfigured (identity-publisher
     * signal); this entry point processes mctpd-shaped payloads directly
     * (unit tests, fuzz harness).
     *
     *  @param[in] msg - mctpd InterfacesAdded message
     */
    void discoverEndpoints(sdbusplus::message_t& msg);

    /** @brief The runtime endpoint-discovery callback: the identity publisher
     * (mctpreactor) added the configured_by association on an endpoint. Reads
     * the endpoint properties back from mctpd, resolves the EM configuration
     * (with bounded retry) and creates the named inventory. No-op when the
     * endpoint is already tracked (startup enumeration overlap).
     *
     *  @param[in] msg - Data associated with the subscribed signal
     */
    void onMctpReactorConfigured(sdbusplus::message_t& msg);

    /** @brief Callback function when MCTP endpoint removedInterface
     * D-Bus signal raised.
     *
     *  @param[in] msg - Data associated with subscribed signal
     */
    void removeEndpoints(sdbusplus::message_t& msg);

    /** @brief Helper function to invoke registered handlers for
     *  the added MCTP endpoints
     *
     *  @param[in] mctpInfos - information of discovered MCTP endpoints
     */
    void handleMctpEndpoints(const MctpInfos& mctpInfos);

    /** @brief Helper function to invoke registered handlers for
     *  the removed MCTP endpoints
     *
     *  @param[in] mctpInfos - information of removed MCTP endpoints
     */
    void handleRemovedMctpEndpoints(const MctpInfos& mctpInfos);

    /** @brief Helper function to invoke registered handlers for
     *  updating the availability status of the MCTP endpoint
     *
     *  @param[in] mctpInfo - information of the target endpoint
     *  @param[in] availability - new availability status
     */
    void updateMctpEndpointAvailability(const MctpInfo& mctpInfo,
                                        Availability availability);

    /** @brief Get list of MctpInfos in MCTP control interface.
     *
     *  Per unify-mctp_discovery_guidelines.md § 2.2 mandatory items 6 + 7,
     *  this function retries on ObjectMapper.GetSubTree failure with bounded
     *  backoff and reports the mapper-health outcome to the caller via the
     *  return value. The constructor uses the return value to suppress an
     *  empty-inventory publication when the mapper was unhealthy.
     *
     *  @param[in] mctpInfoMap - information of discovered MCTP endpoints
     *  and the availability status of each endpoint
     *  @return true if the mapper call (eventually) succeeded; false if all
     *          retries failed. On false, mctpInfoMap is unchanged.
     */
    bool getMctpInfos(std::map<MctpInfo, Availability>& mctpInfoMap);

    /** @brief Mapper-retry backoff schedule (cumulative bound ~9.25s).
     *
     *  Used by getMctpInfos and the bus-owner resolve helper. Mutable so the
     *  TestMctpDiscovery test fixture can substitute a small schedule (e.g.
     *  five 1ms entries) for fast unit tests. Defaults to the production
     *  schedule. */
    std::vector<std::chrono::milliseconds> retryBackoff{
        std::chrono::milliseconds(50), std::chrono::milliseconds(200),
        std::chrono::milliseconds(1000), std::chrono::milliseconds(3000),
        std::chrono::milliseconds(5000)};

    /** @brief Get list of new MctpInfos in addedInterace D-Bus signal message.
     *
     *  @param[in] msg - addedInterace D-Bus signal message
     *  @param[in] mctpInfos - information of added MCTP endpoints
     */
    void getAddedMctpInfos(sdbusplus::message_t& msg, MctpInfos& mctpInfos);

    /** @brief Add new MctpInfos to existingMctpInfos.
     *
     *  @param[in] mctpInfos - information of new MCTP endpoints
     */
    void addToExistingMctpInfos(const MctpInfos& mctpInfos);

    /**
     * @brief A callback for propertiesChanges signal enabledMatches matcher
     * rule to invoke registered handlers.
     * e.g. the platform-mc manager handler is registered for update sensor
     * state accordingly.
     */
    void refreshEndpoints(sdbusplus::message::message& msg);

    /** @brief Loading the static MCTP endpoints to mctpInfos.
     *
     *  @param[in] mctpInfos - information of discovered MCTP endpoints
     */
    void loadStaticEndpoints(MctpInfos& mctpInfos);

    /** @brief EID-keyed identity for statically-assigned, bridge-routed
     *         endpoints that never receive a per-device configured_by
     *         association.
     *
     *  PLDM FW Update Config Migration (DGXOPENBMC-25121). The primary identity
     *  path is configured_by (searchConfigurationFor). Some platforms assign a
     *  device's MCTP EID out of band and route it through a bridge that is set
     *  up by a codeconstruct MCTP interface rather than a per-device
     *  entity-manager transport config (e.g. the GB200 HMC, whose FPGA-bridged
     *  ERoT/GPU/CPU endpoints get fixed EIDs from device_mctp_eid.csv). Such
     *  endpoints carry no configured_by, so configured_by resolution leaves
     *  them nameless and they are dropped.
     *
     *  For each Configuration.PLDMFirmwareDevice that declares a StaticEID,
     *  bind the live mctpd endpoint at that EID to the entry's MCTPTargetName,
     *  populating both the configurations map (the EID->Name resolution
     *  consumed by getTargetNameForEid) and mctpInfoMap (so the endpoint is
     *  processed). An EID already resolved via configured_by is left untouched
     *  — configured_by remains authoritative.
     *
     *  @param[in,out] mctpInfoMap - discovered endpoint -> availability map
     */
    void bindStaticEidConfigurations(
        std::map<MctpInfo, Availability>& mctpInfoMap);

    friend class ::TestMctpDiscovery;

  private:
    /** @brief Get MCTP Endpoint D-Bus Properties in the
     *         `xyz.openbmc_project.MCTP.Endpoint` D-Bus interface
     *
     *  @param[in] service - the MCTP service name
     *  @param[in] path - the MCTP endpoints object path
     *
     *  @return tuple of Network Index, Endpoint ID and MCTP message types
     */
    MctpEndpointProps getMctpEndpointProps(const std::string& service,
                                           const std::string& path);

    /** @brief Get Endpoint UUID from `UUID` D-Bus property in the
     *         `xyz.openbmc_project.Common.UUID` D-Bus interface.
     *
     *  @param[in] service - the MCTP service name
     *  @param[in] path - the MCTP endpoints object path
     *
     *  @return Endpoint UUID
     */
    UUID getEndpointUUIDProp(const std::string& service,
                             const std::string& path);

    /** @brief Get Endpoint Availability status from `Connectivity` D-Bus
     *         property in the `au.com.codeconstruct.MCTP.Endpoint1` D-Bus
     *         interface.
     *
     *  @param[in] path - the MCTP endpoints object path
     *
     *  @return Availability status: true if active false if inactive
     */
    Availability getEndpointConnectivityProp(const std::string& path);

    static pldm::utils::DBusHandlerInterface& defaultDbusHandler();

    static constexpr uint8_t mctpTypePLDM = 1;

    /** @brief Construct the MCTP reactor object path
     *
     *  @param[in] mctpInfo - information of discovered MCTP endpoint
     *
     *  @return the MCTP reactor object path
     */
    std::string constructMctpReactorObjectPath(const MctpInfo& mctpInfo);

    /** @brief Search for associated configuration for the MctpInfo.
     *
     *  @param[in] mctpInfo - information of discovered MCTP endpoint
     */
    bool searchConfigurationFor(MctpInfo& mctpInfo);

    /** @brief searchConfigurationFor with bounded retry/backoff.
     *
     *  Both production lookup sites query ObjectMapper for an association
     *  that is known to exist (the startup enumeration is filtered on it;
     *  the runtime trigger IS its publication signal), so a miss means the
     *  mapper has not ingested it yet or is too loaded to answer within the
     *  D-Bus timeout. Retry on the retryBackoff schedule before giving up.
     *
     *  @param[in] mctpInfo - information of discovered MCTP endpoint
     */
    bool searchConfigurationWithRetry(MctpInfo& mctpInfo);

    /** @brief Remove configuration associated with the removed MCTP endpoint.
     *
     *  @param[in] removedInfos - the removed MCTP endpoints
     */
    void removeConfigs(const MctpInfos& removedInfos);

    /** @brief An internal helper function to get the name property from the
     * properties
     * @param[in] properties - the properties of the D-Bus object
     * @return the name property
     */
    std::string getNameFromProperties(const utils::PropertyMap& properties);

    /** @brief The configuration contains D-Bus path and the MCTP endpoint
     * information.
     */
    Configurations configurations;
};

} // namespace pldm
