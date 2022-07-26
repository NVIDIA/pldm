#pragma once

#include "common/types.hpp"

#include <sys/socket.h>

#include <iostream>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_map>

namespace pldm::mctp_socket
{

using FileDesc = int;
using SendBufferSize = int;
using SocketInfo = std::tuple<FileDesc, SendBufferSize>;

/** @class Manager
 *
 *  The Manager class provides API to register MCTP endpoints and the socket to
 *  communicate with the endpoint. The lookup APIs are used when processing PLDM
 *  Rx messages and when sending PLDM Tx messages.
 */
class Manager
{
  public:
    Manager() = default;
    Manager(const Manager&) = delete;
    Manager(Manager&&) = delete;
    Manager& operator=(const Manager&) = delete;
    Manager& operator=(Manager&&) = delete;
    ~Manager() = default;

    /** @brief Register MCTP endpoint
     *
     *  @param[in] eid - MCTP endpoint ID
     *  @param[in] fd - File descriptor of MCTP demux daemon socket to do Tx/Rx
     *                  with the MCTP endpoint ID
     *  @param[in] sendBufferSize - MCTP demux daemon's socket send buffer size
     */
    void registerEndpoint(EID eid, FileDesc fd, int sendBufferSize)
    {
        if (!socketInfo.contains(fd))
        {
            socketInfo[fd] = sendBufferSize;
        }
        eidToFd[eid] = fd;
    }

    /** @brief Get the MCTP demux daemon socket file descriptor associated with
     *         the EID
     *
     *  @param[in] eid - MCTP endpoint ID
     *
     *  @return MCTP demux daemon's file descriptor
     */
    int getSocket(EID eid)
    {
        return eidToFd[eid];
    }

    /** @brief Get the MCTP demux daemon socket's send buffer size associated
     *         with the EID
     *
     *  @param[in] eid - MCTP endpoint ID
     *
     *  @return MCTP demux daemon socket's send buffer size
     */
    int getSendBufferSize(EID eid)
    {
        return socketInfo[eidToFd[eid]];
    }

    /** @brief Set the MCTP demux daemon socket's send buffer size
     *
     *  @param[in] fd - File descriptor of MCTP demux daemon socket
     *  @param[in] sendBufferSize - Socket send buffer size
     *
     */
    void setSendBufferSize(int fd, int sendBufferSize)
    {
        if (!socketInfo.contains(fd))
        {
            int rc = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sendBufferSize,
                                sizeof(sendBufferSize));
            if (rc == -1)
            {
                rc = -errno;
                std::cerr << "setsockopt call failed, RC= " << strerror(-rc)
                          << "\n";
            }
            else
            {

                socketInfo[fd] = sendBufferSize;
            }
        }
    }

  private:
    /** @brief Map of endpoint IDs to socket fd*/
    std::unordered_map<EID, FileDesc> eidToFd;

    /** @brief Map of file descriptor to socket's send buffer size*/
    std::unordered_map<FileDesc, SendBufferSize> socketInfo;
};

} // namespace pldm::mctp_socket