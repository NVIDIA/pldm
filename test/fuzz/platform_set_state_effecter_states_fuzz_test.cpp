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

    auto reqSize = hdrSize + PLDM_SET_STATE_EFFECTER_STATES_REQ_BYTES;
    std::vector<uint8_t> requestMsg{};

    requestMsg.resize(reqSize);

    uint16_t effecterId = {0x32};

    getFromCin(&effecterId, sizeof(uint16_t));

    uint16_t effecterIdLE = htole16(effecterId);

    uint8_t compEffecterCnt = getUint8t();

    std::vector<set_effecter_state_field> stateField{};

    stateField.resize(compEffecterCnt);

    for (int i = 0; i < compEffecterCnt; ++i)
        stateField[i] = {PLDM_REQUEST_SET, (uint8_t)getUint8t()};

    uint16_t retEffecterId = 0;
    uint8_t retCompEffecterCnt = 0;

    std::vector<set_effecter_state_field> retStateField;
    retStateField.resize(compEffecterCnt);

    memcpy(requestMsg.data() + hdrSize, &effecterIdLE, sizeof(effecterIdLE));
    memcpy(requestMsg.data() + sizeof(effecterIdLE) + hdrSize, &compEffecterCnt,
           sizeof(compEffecterCnt));
    memcpy(requestMsg.data() + sizeof(effecterIdLE) + sizeof(compEffecterCnt) +
               hdrSize,
           stateField.data(), compEffecterCnt);

    auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());

    decode_set_state_effecter_states_req(
        request, requestMsg.size() - hdrSize, &retEffecterId,
        &retCompEffecterCnt, retStateField.data());
}
