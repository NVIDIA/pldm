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

    uint8_t repositoryState = getUint8t();
    uint8_t updateTime[PLDM_TIMESTAMP104_SIZE] = {0};
    uint8_t oemUpdateTime[PLDM_TIMESTAMP104_SIZE] = {0};
    uint32_t recordCount = getUint8t();
    uint32_t repositorySize = getUint8t();
    uint32_t largestRecordSize = UINT32_MAX;
    uint8_t dataTransferHandleTimeout = PLDM_NO_TIMEOUT;

    for (int i = 0; i < PLDM_TIMESTAMP104_SIZE; ++i)
    {
        updateTime[i] = getUint8t();
        oemUpdateTime[i] = getUint8t();
    }

    std::vector<uint8_t> responseMsg(hdrSize +
                                     PLDM_GET_PDR_REPOSITORY_INFO_RESP_BYTES);

    auto response = reinterpret_cast<pldm_msg*>(responseMsg.data());

    encode_get_pdr_repository_info_resp(0, PLDM_SUCCESS, repositoryState,
                                        updateTime, oemUpdateTime, recordCount,
                                        repositorySize, largestRecordSize,
                                        dataTransferHandleTimeout, response);
}
