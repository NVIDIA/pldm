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
#pragma once

#include "libpldm/firmware_update.h"

#include "common/types.hpp"
#include "requester/handler.hpp"
#include "requester/request.hpp"

#include <phosphor-logging/lg2.hpp>

namespace pldm
{

namespace fw_update
{

using RequesterHandler = requester::Handler<pldm::requester::Request>;
using ComponentIndex = size_t;

/**
 * @brief Print debug logs for firmware update when firmware debug option is
 enabled. This variant of printBuffer takes integer vector as an input
 *
 * @param[in] isTx - True if the buffer is an outgoing PLDM message, false
 if the buffer is an incoming PLDM message
 * @param[in] buffer - integer vector buffer to log
 * @param[in] message - Message string for logging
 * @param[in] fwDebug - firmware debug flag
 */
inline void printBuffer(bool isTx, const std::vector<uint8_t>& buffer,
                        const std::string& message, bool fwDebug)
{
    if (fwDebug)
    {
        lg2::info("{INFO_MESSAGE}", "INFO_MESSAGE", message);
        pldm::utils::printBuffer(isTx, buffer);
    }
}

/**
 * @brief Print debug logs for firmware update when firmware debug option is
 * enabled. This variant of printBuffer takes pldm_msg* buffer as an input.
 *
 * @param[in] isTx - True if the buffer is an outgoing PLDM message, false
 * if the buffer is an incoming PLDM message
 * @param[in] buffer - pldm message buffer to log
 * @param[in] bufferLen - pldm message buffer length
 * @param[in] message - Message string for logging
 * @param[in] fwDebug - firmware debug flag
 */
inline void printBuffer(bool isTx, const pldm_msg* buffer, size_t bufferLen,
                        const std::string& message, bool fwDebug)
{
    if (fwDebug)
    {
        lg2::info("{INFO_MESSAGE}", "INFO_MESSAGE", message);
        auto ptr = reinterpret_cast<const uint8_t*>(buffer);
        auto outBuffer =
            std::vector<uint8_t>(ptr, ptr + (sizeof(pldm_msg_hdr) + bufferLen));
        pldm::utils::printBuffer(isTx, outBuffer);
    }
}

/**
 * @brief send and receive pldm message over mctp coroutine
 *
 * @param[in] handle
 * @param[in] eid
 * @param[in] request
 * @param[out] responseMsg
 * @param[out] responseLen
 * @return requester::Coroutine
 */
inline requester::Coroutine
    SendRecvPldmMsgOverMctp(RequesterHandler& handle, mctp_eid_t eid,
                            Request& request, const pldm_msg** responseMsg,
                            size_t* responseLen)
{
    auto rc = co_await requester::SendRecvPldmMsg<RequesterHandler>(
        handle, eid, request, responseMsg, responseLen);
    if (rc)
    {
        lg2::error("sendRecvPldmMsgOverMctp failed. rc={RC}", "RC", rc);
    }
    co_return rc;
}

/** @brief Send COMMAND_NOT_EXPECTED response sent by DeviceUpdater when it
 * receives a command from the FD out of sequence from when it is expected.
 *
 *  @param[in] request - PLDM request message
 *  @param[in] requestLen - PLDM request message length
 */
inline Response sendCommandNotExpectedResponse(const pldm_msg* request,
                                               size_t /*requestLen*/)
{
    Response response(sizeof(pldm_msg), 0);
    auto ptr = reinterpret_cast<pldm_msg*>(response.data());
    auto rc = encode_cc_only_resp(request->hdr.instance_id, request->hdr.type,
                                  request->hdr.command,
                                  PLDM_FWUP_COMMAND_NOT_EXPECTED, ptr);
    assert(rc == PLDM_SUCCESS);
    return response;
}

} // namespace fw_update

} // namespace pldm