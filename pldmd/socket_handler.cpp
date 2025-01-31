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

#include "fw-update/manager.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include <phosphor-logging/lg2.hpp>

namespace pldm::mctp_socket
{

std::optional<Response>
    Handler::processRxMsg([[maybe_unused]] uint8_t tag, uint8_t eid,
                          [[maybe_unused]] uint8_t type,
                          const std::vector<uint8_t>& requestMsg)
{
    pldm_header_info hdrFields{};
    auto hdr = reinterpret_cast<const pldm_msg_hdr*>(requestMsg.data());
    if (PLDM_SUCCESS != unpack_pldm_header(hdr, &hdrFields))
    {
        lg2::error("Empty PLDM request header");
        return std::nullopt;
    }

    if (PLDM_RESPONSE != hdrFields.msg_type)
    {
        Response response;
        auto request = reinterpret_cast<const pldm_msg*>(hdr);
        size_t requestLen = requestMsg.size() - sizeof(struct pldm_msg_hdr);
        try
        {
            if (hdrFields.pldm_type != PLDM_FWUP)
            {
                response =
                    invoker.handle(hdrFields.pldm_type, hdrFields.command,
                                   request, requestLen);
            }
            else
            {
                response = fwManager.handleRequest(eid, hdrFields.command,
                                                   request, requestLen);
            }
        }
        catch (const std::out_of_range& e)
        {
            uint8_t completion_code = PLDM_ERROR_UNSUPPORTED_PLDM_CMD;
            response.resize(sizeof(pldm_msg_hdr));
            auto responseHdr = reinterpret_cast<pldm_msg_hdr*>(response.data());
            pldm_header_info header{};
            header.msg_type = PLDM_RESPONSE;
            header.instance = hdrFields.instance;
            header.pldm_type = hdrFields.pldm_type;
            header.command = hdrFields.command;
            if (PLDM_SUCCESS != pack_pldm_header(&header, responseHdr))
            {
                lg2::error("Failed adding response header");
                return std::nullopt;
            }
            response.insert(response.end(), completion_code);
        }
        return response;
    }
    else if (PLDM_RESPONSE == hdrFields.msg_type)
    {
        auto response = reinterpret_cast<const pldm_msg*>(hdr);
        size_t responseLen = requestMsg.size() - sizeof(struct pldm_msg_hdr);
        handler.handleResponse(eid, hdrFields.instance, hdrFields.pldm_type,
                               hdrFields.command, response, responseLen);
    }
    return std::nullopt;
}

} // namespace pldm::mctp_socket