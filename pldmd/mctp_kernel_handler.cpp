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
#include "socket_handler.hpp"
#include "socket_manager.hpp"

#include <linux/if_arp.h>
#include <linux/mctp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include <phosphor-logging/lg2.hpp>

namespace pldm::mctp_socket
{

int InKernelHandler::registerMctpEndpoint(
    EID eid, [[maybe_unused]] int type, [[maybe_unused]] int protocol,
    [[maybe_unused]] const std::vector<uint8_t>& pathName)
{
    if (isFdValid)
    {
        manager.registerEndpoint(eid, fd, sendBufferSize);
        return PLDM_SUCCESS;
    }

    fd = socket(AF_MCTP, SOCK_DGRAM, 0);

    int rc = 0;
    if (fd == -1)
    {
        rc = -errno;
        lg2::error("Failed to create the socket, RC={RC}, EID={ED}", "RC",
                   strerror(-rc), "ED", eid);
        return rc;
    }

    socklen_t optlen;
    optlen = sizeof(sendBufferSize);
    rc = getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sendBufferSize, &optlen);
    if (rc == -1)
    {
        rc = -errno;
        lg2::error(
            "Error getting the default socket send buffer size, RC={RC}, EID={ED}",
            "RC", strerror(-rc), "ED", eid);
        return rc;
    }

    struct sockaddr_mctp addr;
    memset(&addr, 0, sizeof(addr));

    addr.smctp_family = AF_MCTP;
    addr.smctp_network = MCTP_NET_ANY;
    addr.smctp_addr.s_addr = MCTP_ADDR_ANY;
    addr.smctp_tag = MCTP_TAG_OWNER;
    addr.smctp_type = MCTP_MSG_TYPE_PLDM;

    rc = bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc == -1)
    {
        rc = -errno;
        lg2::error(
            "Error while binding the socket to PLDM Msg Type, RC={RC}, EID={ED}",
            "RC", strerror(-rc), "ED", eid);
        return rc;
    }

    io = std::make_unique<IO>(
        event, fd, EPOLLIN,
        std::bind_front(&InKernelHandler::handleReceivedMsg, this));

    manager.registerEndpoint(eid, fd, sendBufferSize);

    isFdValid = true;

    return PLDM_SUCCESS;
}

int InKernelHandler::sendMsg(EID eid, int mctpFd, const uint8_t* pldmMsg,
                             size_t pldmMsgLen) const
{
    struct sockaddr_mctp addr;
    memset(&addr, 0, sizeof(addr));

    addr.smctp_family = AF_MCTP;
    addr.smctp_network = MCTP_NET_ANY;
    addr.smctp_addr.s_addr = eid;
    addr.smctp_tag = MCTP_TAG_OWNER;
    addr.smctp_type = MCTP_MSG_TYPE_PLDM;

    ssize_t rc =
        sendto(mctpFd, pldmMsg, pldmMsgLen, 0,
               reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc == -1)
    {
        int error = -errno;
        lg2::error("Error while sending the message. RC={RC}, EID={ED}", "RC",
                   strerror(-error), "ED", eid);
        return PLDM_ERROR;
    }

    return PLDM_SUCCESS;
}

void InKernelHandler::handleReceivedMsg(IO& io, int fd,
                                        [[maybe_unused]] uint32_t revents)
{
    int returnCode{0};

    ssize_t peekedLength = recv(fd, nullptr, 0, MSG_PEEK | MSG_TRUNC);
    if (peekedLength == 0)
    {
        // This may or may not be an error scenario, in either case the
        // recovery mechanism for this daemon is to restart, and hence
        // exit the event loop, that will cause this daemon to exit with a
        // failure code.
        returnCode = -errno;
        lg2::error("recv system call failed. Terminating. RC={RC}", "RC",
                   returnCode);
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

        struct sockaddr_mctp addr;
        memset(&addr, 0, sizeof(addr));

        socklen_t addrlen = sizeof(addr);

        ssize_t recvDataLength = recvfrom(
            fd, static_cast<void*>(requestMsg.data()), peekedLength, MSG_TRUNC,
            reinterpret_cast<struct sockaddr*>(&addr), &addrlen);

        if (recvDataLength == peekedLength)
        {
            if (verbose)
            {
                utils::printBuffer(utils::Rx, requestMsg);
            }

            if (MCTP_MSG_TYPE_PLDM != addr.smctp_type)
            {
                // Skip this message and continue.
            }
            else
            {
                // process message and send response
                auto response =
                    processRxMsg(addr.smctp_tag, addr.smctp_addr.s_addr,
                                 addr.smctp_type, requestMsg);
                if (response.has_value())
                {
                    if (verbose)
                    {
                        utils::printBuffer(utils::Tx, *response);
                    }

                    struct sockaddr_mctp destAddr;
                    memset(&destAddr, 0, sizeof(destAddr));

                    destAddr.smctp_family = AF_MCTP;
                    destAddr.smctp_network = MCTP_NET_ANY;
                    destAddr.smctp_addr.s_addr = addr.smctp_addr.s_addr;
                    destAddr.smctp_type = MCTP_MSG_TYPE_PLDM;

                    constexpr uint8_t tagOwnerBitPos = 3;
                    constexpr uint8_t tagOwnerMask = ~(1 << tagOwnerBitPos);
                    destAddr.smctp_tag = addr.smctp_tag & tagOwnerMask;

                    ssize_t rc =
                        sendto(fd, response->data(), response->size(), 0,
                               reinterpret_cast<struct sockaddr*>(&destAddr),
                               sizeof(destAddr));
                    if (rc == -1)
                    {
                        returnCode = -errno;
                        lg2::error("sendmsg system call failed, RC={RC}", "RC",
                                   returnCode);
                    }
                }
            }
        }
        else
        {
            returnCode = -errno;
            lg2::error(
                "Failure to read peeked length packet. peekedLength="
                "{PEEKEDLENGTH} recvDataLength={RECVDATALENGTH} ErrorNo={ERROR}",
                "PEEKEDLENGTH", peekedLength, "RECVDATALENGTH", recvDataLength,
                "ERROR", returnCode);
        }
    }
}
} // namespace pldm::mctp_socket