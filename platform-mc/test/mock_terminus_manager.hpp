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

#include "platform-mc/terminus_manager.hpp"

#include <queue>

#include <gmock/gmock.h>

namespace pldm
{
namespace platform_mc
{

class MockTerminusManager : public TerminusManager
{
  public:
    MockTerminusManager(sdeventplus::Event& event,
                        requester::Handler<requester::Request>& handler,
                        dbus_api::Requester& requester,
                        std::map<tid_t, std::shared_ptr<Terminus>>& termini,
                        mctp_eid_t localEid, Manager* manager) :
        TerminusManager(event, handler, requester, termini, localEid, manager,
                        true)
    {}

    requester::Coroutine SendRecvPldmMsgOverMctp(mctp_eid_t /*eid*/,
                                                 Request& /*request*/,
                                                 const pldm_msg** responseMsg,
                                                 size_t* responseLen) override
    {

        if (responseMsgs.empty() || responseMsg == nullptr ||
            responseLen == nullptr)
        {
            co_return PLDM_ERROR;
        }

        *responseMsg = (pldm_msg*)responseMsgs.front();
        *responseLen = responseLens.front() - sizeof(pldm_msg_hdr);

        responseMsgs.pop();
        responseLens.pop();
        co_return PLDM_SUCCESS;
    }

    int enqueueResponse(std::vector<uint8_t>& response)
    {
        if (response.size() <= sizeof(pldm_msg_hdr))
        {
            return PLDM_ERROR_INVALID_LENGTH;
        }

        responses.push(response);
        responseMsgs.push(responses.back().data());
        responseLens.push(response.size());
        return PLDM_SUCCESS;
    }

    int enqueueResponse(pldm_msg* responseMsg, size_t responseLen)
    {
        if (responseMsg == nullptr)
        {
            return PLDM_ERROR_INVALID_DATA;
        }

        if (responseLen <= sizeof(pldm_msg_hdr))
        {
            return PLDM_ERROR_INVALID_LENGTH;
        }

        uint8_t* ptr = (uint8_t*)responseMsg;
        std::vector<uint8_t> response(ptr, ptr + responseLen);
        return enqueueResponse(response);
    }

    int clearQueuedResponses()
    {
        while (!responseMsgs.empty())
        {
            responseMsgs.pop();
            responseLens.pop();
            responses.pop();
        }
        return PLDM_SUCCESS;
    }

    std::queue<uint8_t*> responseMsgs;
    std::queue<size_t> responseLens;
    std::queue<std::vector<uint8_t>> responses;
};

} // namespace platform_mc
} // namespace pldm