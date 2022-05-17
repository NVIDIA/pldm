#pragma once

#include "fw-update/manager.hpp"
#include "pldmd/socket_handler.hpp"
#include "config.h"

#include "libpldm/requester/pldm.h"

#include <sdbusplus/bus/match.hpp>

#include <filesystem>
#include <initializer_list>
#include <vector>

namespace pldm
{

using EID = uint8_t;
using UUID = std::string;
using MctpInfo = std::pair<EID, UUID>;
using MctpInfos = std::vector<MctpInfo>;

/** @class MctpDiscoveryHandlerIntf
 *
 * This abstract class defines the APIs for MctpDiscovery class has common
 * interface to execute function from different Manager Classes
 */
class MctpDiscoveryHandlerIntf
{
  public:
    virtual void handleMCTPEndpoints(const MctpInfos& mctpInfos) = 0;
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
     *  @param[in] fwManager - pointer to the firmware manager
     *  @param[in] list - initializer list to the MctpDiscoveryHandlerIntf
     */
    explicit MctpDiscovery(
        sdbusplus::bus::bus& bus,
         mctp_socket::Handler& handler,
        std::initializer_list<MctpDiscoveryHandlerIntf*> list,
        const std::filesystem::path& staticEidTablePath =
            STATIC_EID_TABLE_PATH);

  private:
    /** @brief reference to the systemd bus */
    sdbusplus::bus::bus& bus;
    mctp_socket::Handler& handler;

    /** @brief Used to watch for new MCTP endpoints */
    sdbusplus::bus::match_t mctpEndpointAddedSignal;

    /** @brief Used to watch for the removed MCTP endpoints */
    sdbusplus::bus::match_t mctpEndpointRemovedSignal;

    void discoverEndpoints(sdbusplus::message::message& msg);

    /** @brief Process the D-Bus MCTP endpoint info and prepare data to be used
     *         for PLDM discovery.
     *
     *  @param[in] interfaces - MCTP D-Bus information
     *  @param[out] mctpInfos - MCTP info for PLDM discovery
     */
    void populateMctpInfo(const dbus::InterfaceMap& interfaces,
                          MctpInfos& mctpInfos);

    static constexpr uint8_t mctpTypePLDM = 1;

    /** @brief MCTP endpoint interface name */
    const std::string mctpEndpointIntfName{"xyz.openbmc_project.MCTP.Endpoint"};

    /** @brief UUID interface name */
    static constexpr std::string_view uuidEndpointIntfName{
        "xyz.openbmc_project.Common.UUID"};

    /** @brief Unix Socket interface name */
    static constexpr std::string_view unixSocketIntfName{
        "xyz.openbmc_project.Common.UnixSocket"};

    std::vector<MctpDiscoveryHandlerIntf*> handlers;

    std::filesystem::path staticEidTablePath;

    void handleMCTPEndpoints(const MctpInfos& mctpInfos);

    void loadStaticEndpoints(MctpInfos& mctpInfos);
};

} // namespace pldm
