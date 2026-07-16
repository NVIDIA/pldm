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

#include "fw_update_utility.hpp"

namespace pldm::fw_update
{

exec::task<int> sendRecvPldmMsgOverMctp(
    RequesterHandler& handle, mctp_eid_t eid, Request& request,
    const pldm_msg** responseMsg, size_t* responseLen)
{
    int rc = 0;
    try
    {
        std::tie(rc, *responseMsg, *responseLen) =
            co_await handle.sendRecvMsg(eid, std::move(request));
    }
    catch (const sdbusplus::exception_t& e)
    {
        error("Send and Receive PLDM message over MCTP throw error - {ERROR}.",
              "ERROR", e);
        co_return PLDM_ERROR;
    }
    catch (const int& e)
    {
        error(
            "Send and Receive PLDM message over MCTP throw int error - {ERROR}.",
            "ERROR", e);
        co_return PLDM_ERROR;
    }

    co_return rc;
}

void handleTransportError(RequesterHandler& handler, mctp_eid_t eid,
                          const std::string& commandName, uint8_t pldmType,
                          bool critical)
{
    auto errorInfo = handler.getTransportError(eid, pldmType);

    if (errorInfo)
    {
        pldm::transport::createMctpTransportRedfishEvent(
            eid, commandName, errorInfo->errorCode, errorInfo->binding,
            errorInfo->direction, "FWUpdate", critical);

        handler.clearTransportError(eid);
    }
}

} // namespace pldm::fw_update
