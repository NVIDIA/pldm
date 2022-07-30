#pragma once

#include "fw-update/manager.hpp"
#include "pldmd/socket_handler.hpp"

#include <sdbusplus/bus/match.hpp>

namespace pldm
{

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
     */
    explicit MctpDiscovery(sdbusplus::bus::bus& bus,
                           mctp_socket::Handler& handler,
                           fw_update::Manager* fwManager);

  private:
    /** @brief reference to the systemd bus */
    sdbusplus::bus::bus& bus;
    mctp_socket::Handler& handler;

    fw_update::Manager* fwManager;

    /** @brief Used to watch for new MCTP endpoints */
    sdbusplus::bus::match_t mctpEndpointSignal;

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
};

} // namespace pldm