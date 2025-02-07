
/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION &
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
#include "libpldm/firmware_update.h"

#include "common/utils.hpp"
#include "common_utils.hpp"
#include "pldmd/dbus_impl_requester.hpp"
#include "requester/handler.hpp"

#include <vector>

using namespace pldm;

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    constexpr auto hdrSize = sizeof(pldm_msg_hdr);

    auto respSize = hdrSize + PLDM_EVENT_MESSAGE_SUPPORTED_MIN_RESP_BYTES + 2;
    std::vector<uint8_t> responseMsg(respSize);

    auto response = reinterpret_cast<pldm_msg*>(responseMsg.data());

    struct pldm_event_message_supported_resp* resp =
        reinterpret_cast<struct pldm_event_message_supported_resp*>(
            response->payload);

    uint8_t completionCode =
        PLDM_INVALID_TRANSFER_OPERATION_FLAG % (getUint8t() + 1);
    uint8_t synchronyConfiguration = getUint8t();
    uint8_t synchronyConfigurationSupported = getUint8t();
    uint8_t numberEventClassReturned = getUint8t();
    uint8_t eventClass0 = getUint8t();
    uint8_t eventClass1 = getUint8t();

    resp->completion_code = completionCode;
    resp->synchrony_configuration = synchronyConfiguration;
    resp->synchrony_configuration_supported = synchronyConfigurationSupported;
    resp->number_event_class_returned = numberEventClassReturned;
    resp->event_class[0] = eventClass0;
    resp->event_class[1] = eventClass1;

    uint8_t retCompletionCode = 0;
    uint8_t retSynchronyConfiguration = 0;
    uint8_t retSynchronyConfigurationSupported = 0;
    uint8_t retNumberEventClassReturned = 0;
    uint8_t* retEventClasses = 0;

    decode_event_message_supported_resp(
        response, responseMsg.size() - sizeof(pldm_msg_hdr), &retCompletionCode,
        &retSynchronyConfiguration, &retSynchronyConfigurationSupported,
        &retNumberEventClassReturned, &retEventClasses);
}
