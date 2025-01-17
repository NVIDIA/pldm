#pragma once

#include "libpldm/base.h"
#include "libpldm/bios.h"
#include "libpldm/firmware_update.h"
#include "libpldm/fru.h"
#include "libpldm/platform.h"

#include "common/utils.hpp"

#include <err.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <cstring>
#include <iomanip>
#include <iostream>
#include <utility>

namespace pldmtool
{

namespace helper
{

constexpr static uint8_t PLDM_ENTITY_ID = 8;
constexpr static uint8_t MCTP_MSG_TYPE_PLDM = 1;
constexpr static auto mctpEndpointIntfName{"xyz.openbmc_project.MCTP.Endpoint"};
constexpr static auto unixSocketIntfName{
    "xyz.openbmc_project.Common.UnixSocket"};
constexpr static auto objectEnableIntfName{"xyz.openbmc_project.Object.Enable"};
using ordered_json = nlohmann::ordered_json;

/** @brief print the input message if pldmverbose is enabled
 *
 *  @param[in]  pldmVerbose - verbosity flag - true/false
 *  @param[in]  msg         - message to print
 *  @param[in]  data        - data to print
 *
 *  @return - None
 */

template <class T>
void Logger(bool pldmverbose, const char* msg, const T& data)
{
    if (pldmverbose)
    {
        std::stringstream s;
        s << data;
        std::cout << msg << s.str() << std::endl;
    }
}

/** @brief Display in JSON format.
 *
 *  @param[in]  data - data to print in json
 *
 *  @return - None
 */
static inline void DisplayInJson(const ordered_json& data)
{
    std::cout << data.dump(4) << std::endl;
}

/** @brief MCTP socket read/recieve
 *
 *  @param[in]  socketName - Socket name
 *  @param[in]  requestMsg - Request message to compare against loopback
 *              message recieved from mctp socket
 *  @param[out] responseMsg - Response buffer recieved from mctp socket
 *  @param[in]  pldmVerbose - verbosity flag - true/false
 *
 *  @return -   0 on success.
 *             -1 or -errno on failure.
 */
int mctpSockSendRecv(std::string socketName,
                     const std::vector<uint8_t>& requestMsg,
                     std::vector<uint8_t>& responseMsg, bool pldmVerbose);

/**
 *  @brief Translate PLDM completion code as human-readable string
 *
 *  @param[in] completionCode - PLDM completion code
 *  @param[in] data - The JSON data to which the completion code is added.
 *
 *  @return - None.
 */
void fillCompletionCode(uint8_t completionCode, ordered_json& data);

class CommandInterface
{

  public:
    explicit CommandInterface(const char* type, const char* name,
                              CLI::App* app) :
        pldmType(type),
        commandName(name), mctp_eid(PLDM_ENTITY_ID), pldmVerbose(false),
        instanceId(0)
    {
        app->add_option("-m,--mctp_eid", mctp_eid, "MCTP endpoint ID");
        app->add_option("-n,--socket_name", socketName, "Socket Name");
        app->add_flag("-v, --verbose", pldmVerbose);
        app->callback([&]() { exec(); });
    }

    virtual ~CommandInterface() = default;

    virtual std::pair<int, std::vector<uint8_t>> createRequestMsg() = 0;

    virtual void parseResponseMsg(struct pldm_msg* responsePtr,
                                  size_t payloadLength) = 0;

    virtual void exec();

    int pldmSendRecv(std::vector<uint8_t>& requestMsg,
                     std::vector<uint8_t>& responseMsg);

    /**
     * @brief get MCTP endpoint ID
     *
     * @return uint8_t - MCTP endpoint ID
     */
    inline uint8_t getMCTPEID()
    {
        return mctp_eid;
    }

  private:
    /** @brief Get Managed Objects for an MCTP service
     *
     *  @param[in]  service - Service to fetch objects for
     *
     *  @return On success return the objects managed by the service
     *          on error return empty
     */
    pldm::dbus::ObjectValueTree
        getMctpManagedObjects(const std::string& service) const noexcept;

    /** @brief Get set of MCTP services
     *
     * getmctpservices does an objectmapper for objects implementing
     * mctp remote endpoint and collects the service names from the objects
     *
     *
     *  @return On success return the set of MCTP services, on dbus error
     *          the set will be empty
     */
    std::set<pldm::dbus::Service> getMctpServices() const;

    /** @brief Get MCTP demux daemon socket address
     *
     *  getMctpSockAddr does a D-Bus lookup for MCTP remote endpoint and return
     *  the unix socket info to be used for Tx/Rx
     *
     *  @param[in]  eid - Request MCTP endpoint
     *
     *  @return On success return the type, protocol and unit socket address, on
     *          failure the address will be empty
     */
    std::tuple<bool, int, int, std::vector<uint8_t>>
        getMctpSockInfo(uint8_t remoteEID);
    const std::string pldmType;
    const std::string commandName;
    uint8_t mctp_eid;
    bool pldmVerbose;

  protected:
    uint8_t instanceId;
    std::optional<std::string> socketName;
};

} // namespace helper
} // namespace pldmtool
