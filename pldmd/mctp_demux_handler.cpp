/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "pldm.h"

#include "common/flight_recorder.hpp"
#include "socket_handler.hpp"
#include "socket_manager.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include <phosphor-logging/lg2.hpp>

namespace pldm::mctp_socket
{
int DaemonHandler::registerMctpEndpoint(EID eid, int type, int protocol,
                                        const std::vector<uint8_t>& pathName)
{
    auto entry = socketInfoMap.find(pathName);
    if (entry == socketInfoMap.end())
    {
        auto [fd, currentSendBufferSize] =
            initSocket(eid, type, protocol, pathName);
        if (fd < 0)
        {
            return fd;
        }
        else
        {
            manager.registerEndpoint(eid, fd, currentSendBufferSize);
        }
    }
    else
    {
        manager.registerEndpoint(eid, (*(std::get<0>(entry->second)).get())(),
                                 std::get<1>(entry->second));
    }

    return 0;
}
SocketInfo DaemonHandler::initSocket([[maybe_unused]] EID eid, int type,
                                     int protocol,
                                     const std::vector<uint8_t>& pathName)
{
    /* Create socket */
    int rc = 0;
    int sendBufferSize = 0;
    int sockFd = socket(AF_UNIX, type, protocol);
    if (sockFd == -1)
    {
        rc = -errno;
        lg2::error("Failed to create the socket, RC={RC}", "RC", strerror(-rc));
        return {rc, sendBufferSize};
    }

    auto fd = std::make_unique<pldm::utils::CustomFD>(sockFd);

    /* Get socket current buffer size */
    socklen_t optlen;
    optlen = sizeof(sendBufferSize);
    rc = getsockopt(sockFd, SOL_SOCKET, SO_SNDBUF, &sendBufferSize, &optlen);
    if (rc == -1)
    {
        rc = -errno;
        lg2::error("Error getting the default socket send buffer size, RC={RC}",
                   "RC", strerror(-rc));
        return {rc, sendBufferSize};
    }

    // /* Initiate a connection to the socket */
    struct sockaddr_un addr
    {};
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, pathName.data(), pathName.size());
    rc = connect(sockFd, reinterpret_cast<struct sockaddr*>(&addr),
                 pathName.size() + sizeof(addr.sun_family));
    if (rc == -1)
    {
        rc = -errno;
        lg2::error("Failed to connect to the socket, RC={RC}", "RC",
                   strerror(-rc));
        return {rc, sendBufferSize};
    }

    /* Register for PLDM message type */
    ssize_t result =
        write(sockFd, &MCTP_MSG_TYPE_PLDM, sizeof(MCTP_MSG_TYPE_PLDM));
    if (result == -1)
    {
        rc = -errno;
        lg2::error(
            "Failed to send message type as PLDM to demux daemon, RC={RC}",
            "RC", strerror(-rc));
        return {rc, sendBufferSize};
    }

    auto io = std::make_unique<IO>(
        event, sockFd, EPOLLIN,
        std::bind_front(&DaemonHandler::handleReceivedMsg, this));
    socketInfoMap[pathName] =
        std::tuple(std::move(fd), sendBufferSize, std::move(io));

    return {sockFd, sendBufferSize};
}
int DaemonHandler::sendMsg(EID eid, int mctpFd, const uint8_t* pldmMsg,
                           size_t pldmMsgLen) const
{
    return pldm_send(eid, mctpFd, pldmMsg, pldmMsgLen);
}

void DaemonHandler::handleReceivedMsg(IO& io, int fd, uint32_t revents)
{
    using namespace pldm::flightrecorder;
    using namespace pldm::utils;
    if (!(revents & EPOLLIN))
    {
        return;
    }

    // Outgoing message.
    struct iovec iov[2]{};

    // This structure contains the parameter information for the response
    // message.
    struct msghdr msg
    {};

    int returnCode = 0;
    ssize_t peekedLength = recv(fd, nullptr, 0, MSG_PEEK | MSG_TRUNC);
    if (peekedLength == 0)
    {
        // MCTP daemon has closed the socket this daemon is connected to.
        // This may or may not be an error scenario, in either case the
        // recovery mechanism for this daemon is to restart, and hence
        // exit the event loop, that will cause this daemon to exit with a
        // failure code.
        io.get_event().exit(0);
    }
    else if (peekedLength <= -1)
    {
        returnCode = -errno;
        lg2::error("recv system call failed, RC={RC}", "RC", returnCode);
    }
    else
    {
        std::vector<uint8_t> requestMsg(peekedLength);
        auto recvDataLength =
            recv(fd, static_cast<void*>(requestMsg.data()), peekedLength, 0);
        if (recvDataLength == peekedLength)
        {
            FlightRecorder::GetInstance().saveRecord(requestMsg, false);
            if (verbose)
            {
                printBuffer(Rx, requestMsg);
            }

            if (MCTP_MSG_TYPE_PLDM != requestMsg[2])
            {
                // Skip this message and continue.
            }
            else
            {
                // process message and send response
                auto response =
                    processRxMsg(requestMsg[0], requestMsg[1], requestMsg[2],
                                 std::vector<uint8_t>(requestMsg.begin() + 3,
                                                      requestMsg.end()));
                if (response.has_value())
                {
                    FlightRecorder::GetInstance().saveRecord(*response, true);
                    if (verbose)
                    {
                        printBuffer(Tx, *response);
                    }

                    constexpr uint8_t tagOwnerBitPos = 3;
                    constexpr uint8_t tagOwnerMask = ~(1 << tagOwnerBitPos);
                    // Set tag owner bit to 0 for PLDM responses
                    requestMsg[0] = requestMsg[0] & tagOwnerMask;
                    iov[0].iov_base = &requestMsg[0];
                    iov[0].iov_len = sizeof(requestMsg[0]) +
                                     sizeof(requestMsg[1]) +
                                     sizeof(requestMsg[2]);
                    iov[1].iov_base = (*response).data();
                    iov[1].iov_len = (*response).size();

                    msg.msg_iov = iov;
                    msg.msg_iovlen = sizeof(iov) / sizeof(iov[0]);

                    int result = sendmsg(fd, &msg, 0);
                    if (-1 == result)
                    {
                        returnCode = -errno;
                        lg2::error("sendto system call failed, RC={RC}", "RC",
                                   returnCode);
                    }
                }
            }
        }
        else
        {
            lg2::error("Failure to read peeked length packet. peekedLength="
                       "{PEEKEDLENGTH} recvDataLength={RECVDATALENGTH}",
                       "PEEKEDLENGTH", peekedLength, "RECVDATALENGTH",
                       recvDataLength);
        }
    }
}
} // namespace pldm::mctp_socket