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

    auto msgSize = hdrSize + PLDM_PLATFORM_EVENT_MESSAGE_MIN_REQ_BYTES +
                   PLDM_PLATFORM_EVENT_MESSAGE_STATE_SENSOR_STATE_REQ_BYTES;
    std::vector<uint8_t> requestMsg(msgSize);

    getFromCin(requestMsg.data(), msgSize);

    uint8_t retFormatVersion = 0;
    uint8_t retTid = 0;
    uint8_t retEventClass = 0;
    size_t retEventDataOffset = 0;

    auto req = reinterpret_cast<pldm_msg*>(requestMsg.data());
    struct pldm_platform_event_message_req* request =
        reinterpret_cast<struct pldm_platform_event_message_req*>(req->payload);

    uint8_t formatVersion = getUint8t();
    uint8_t tid = getUint8t();
    // Sensor Event
    uint8_t eventClass = getUint8t();

    request->format_version = formatVersion;
    request->tid = tid;
    request->event_class = eventClass;

    decode_platform_event_message_req(req, requestMsg.size() - hdrSize,
                                      &retFormatVersion, &retTid,
                                      &retEventClass, &retEventDataOffset);
}
