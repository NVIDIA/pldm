#pragma once

#include "config.h"

#include "libpldm/requester/pldm.h"

#include "common/types.hpp"
#include "pldmd/socket_handler.hpp"

#include <sdbusplus/bus/match.hpp>

#include <filesystem>
#include <initializer_list>
#include <vector>

namespace pldm
{

/** @class MctpDiscoveryHandlerIntf
 *
 * This abstract class defines the APIs for MctpDiscovery class has common
 * interface to execute function from different Manager Classes
 */
class MctpDiscoveryHandlerIntf
{
  public:
    virtual void handleMctpEndpoints(const MctpInfos& mctpInfos,
                                     dbus::MctpInterfaces& mctpInterfaces) = 0;

    virtual void onlineMctpEndpoint([[maybe_unused]] const UUID& uuid,
                                    [[maybe_unused]] const EID& eid)
    {}
    virtual void offlineMctpEndpoint([[maybe_unused]] const UUID& uuid,
                                     [[maybe_unused]] const EID& eid)
    {}
    virtual ~MctpDiscoveryHandlerIntf()
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
    ~MctpDiscovery() = default;

    /** @brief Constructs the MCTP Discovery object to handle discovery of
     *         MCTP enabled devices
     *
     *  @param[in] bus - reference to systemd bus
     *  @param[in] list - initializer list to the MctpDiscoveryHandlerIntf
     */
    explicit MctpDiscovery(
        sdbusplus::bus::bus& bus, mctp_socket::Handler& handler,
        std::initializer_list<MctpDiscoveryHandlerIntf*> list,
        const std::filesystem::path& staticEidTablePath =
            STATIC_EID_TABLE_PATH);

  private:
    /** @brief reference to the systemd bus */
    sdbusplus::bus::bus& bus;
    mctp_socket::Handler& handler;

    /** @brief Used to watch for new MCTP endpoints */
    sdbusplus::bus::match_t mctpEndpointAddedSignal;

    /** @brief handler for mctpEndpointAddedSignal */
    void discoverEndpoints(sdbusplus::message::message& msg);

    /** @brief Used to watch for the removed MCTP endpoints */
    sdbusplus::bus::match_t mctpEndpointRemovedSignal;

    /** @brief handler for mctpEndpointRemovedSignal */
    void cleanEndpoints(sdbusplus::message::message& msg);

    /**
     * @brief matcher rule for property changes of
     * xyz.openbmc_project.Object.Enable dbus object
     */
    std::map<std::string, sdbusplus::bus::match_t> enableMatches;

    /**
     * @brief A callback for propertiesChanges signal enabledMatches matcher
     * rule to invoke registered handlers.
     * e.g. the platform-mc manager handler is registered for update sensor
     * state accordingly.
     */
    void refreshEndpoints(sdbusplus::message::message& msg);

    /** @brief Process the D-Bus MCTP endpoint info and prepare data to be used
     *         for PLDM discovery.
     *
     *  @param[in] interfaces - MCTP D-Bus information
     *  @param[out] mctpInfos - MCTP info for PLDM discovery
     */
    void populateMctpInfo(const dbus::InterfaceMap& interfaces,
                          MctpInfos& mctpInfos,
                          dbus::MctpInterfaces& mctpInterfaces);

    static constexpr uint8_t mctpTypePLDM = 1;

    /** @brief MCTP endpoint interface name */
    const std::string mctpEndpointIntfName{"xyz.openbmc_project.MCTP.Endpoint"};

    const std::string mctpBindingIntfName{"xyz.openbmc_project.MCTP.Binding"};

    /** @brief UUID interface name */
    static constexpr std::string_view uuidEndpointIntfName{
        "xyz.openbmc_project.Common.UUID"};

    /** @brief Unix Socket interface name */
    static constexpr std::string_view unixSocketIntfName{
        "xyz.openbmc_project.Common.UnixSocket"};

    std::vector<MctpDiscoveryHandlerIntf*> handlers;

    /** @brief Path of static EID table config file */
    std::filesystem::path staticEidTablePath;

    /** @brief Helper function to invoke registered handlers
     *
     *  @param[in] mctpInfos - information of discovered MCTP endpoints
     */
    void handleMctpEndpoints(const MctpInfos& mctpInfos,
                             dbus::MctpInterfaces& mctpInterfaces);

    /** @brief Loading the static MCTP endpoints to mctpInfos.
     *
     *  @param[in] mctpInfos - information of discovered MCTP endpoints
     */
    void loadStaticEndpoints(MctpInfos& mctpInfos);
};

} // namespace pldm
