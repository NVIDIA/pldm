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

    uint8_t count = getUint8t();

    auto respSize = hdrSize + PLDM_GET_STATE_SENSOR_READINGS_MIN_RESP_BYTES +
                    sizeof(get_sensor_state_field) * count;

    std::vector<uint8_t> responseMsg(respSize);

    getFromCin(responseMsg.data(), respSize);

    auto response = reinterpret_cast<pldm_msg*>(responseMsg.data());
    uint8_t comp_sensorCnt = count;

    std::vector<get_sensor_state_field> stateField(count);

    for (int i = 0; i < count; ++i)
    {
        stateField[i] = {getUint8t(), getUint8t(), getUint8t(), getUint8t()};
    }

    encode_get_state_sensor_readings_resp(0, PLDM_SUCCESS, comp_sensorCnt,
                                          stateField.data(), response);
}
