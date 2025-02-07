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

    uint32_t nextRecordHndl{0x12};

    getFromCin(&nextRecordHndl, sizeof(uint32_t));

    uint32_t nextDataTransferHndl{0x13};

    getFromCin(&nextDataTransferHndl, sizeof(uint32_t));

    uint8_t transferFlag = PLDM_END;
    uint16_t respCnt{0x5};

    getFromCin(&respCnt, sizeof(uint16_t));

    std::vector<uint8_t> recordData{};

    recordData.resize(respCnt);

    for (int i = 0; i < respCnt; ++i)
        recordData[i] = getchar();

    uint8_t transferCRC = getchar();

    // + size of record data and transfer CRC
    std::vector<uint8_t> responseMsg(hdrSize + PLDM_GET_PDR_MIN_RESP_BYTES +
                                     recordData.size() + 1);

    auto response = reinterpret_cast<pldm_msg*>(responseMsg.data());

    encode_get_pdr_resp(0, PLDM_SUCCESS, nextRecordHndl, nextDataTransferHndl,
                        transferFlag, respCnt, recordData.data(), transferCRC,
                        response);

    transferFlag = PLDM_START_AND_END; // No CRC in this case
    responseMsg.resize(responseMsg.size() - sizeof(transferCRC));
    encode_get_pdr_resp(0, PLDM_SUCCESS, nextRecordHndl, nextDataTransferHndl,
                        transferFlag, respCnt, recordData.data(), transferCRC,
                        response);
}
