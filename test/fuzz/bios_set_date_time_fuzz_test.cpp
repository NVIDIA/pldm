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

    uint8_t seconds = getUint8t();
    uint8_t minutes = getUint8t();
    uint8_t hours = getUint8t();
    uint8_t day = getUint8t();
    uint8_t month = getUint8t();
    uint16_t year = 2023;

    auto reqSize = sizeof(pldm_msg_hdr) + PLDM_GET_DATE_TIME_RESP_BYTES;
    std::vector<uint8_t> requestMsg{};

    requestMsg.resize(reqSize);

    getFromCin(requestMsg.data(), reqSize);

    auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());

    encode_set_date_time_req(0, seconds, minutes, hours, day, month, year,
                             request, reqSize);

    uint8_t retSeconds{0};
    uint8_t retMinutes{0};
    uint8_t retHours{0};
    uint8_t retDay{0};
    uint8_t retMonth{0};
    uint16_t retYear{0};

    decode_set_date_time_req(request, requestMsg.size() - hdrSize, &retSeconds,
                             &retMinutes, &retHours, &retDay, &retMonth,
                             &retYear);
}
