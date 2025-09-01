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

    auto respSize = sizeof(pldm_msg_hdr) + PLDM_GET_BIOS_TABLE_MIN_RESP_BYTES +
                    4;
    std::vector<uint8_t> responseMsg{};

    responseMsg.resize(respSize);
    auto response = reinterpret_cast<pldm_msg*>(responseMsg.data());

    uint8_t instanceId = getUint8t();
    uint32_t transferHandle = getUint8t();
    uint32_t nextTransferHandle = getUint8t();
    uint8_t transferFlag = PLDM_START_AND_END % (getUint8t() + 1);
    std::vector<uint8_t> tableData;
    tableData.resize(4);

    getFromCin(tableData.data(), 4);

    encode_get_bios_table_resp(
        0, PLDM_SUCCESS, nextTransferHandle, transferFlag, tableData.data(),
        sizeof(pldm_msg_hdr) + PLDM_GET_BIOS_TABLE_MIN_RESP_BYTES + 4,
        response);

    uint8_t tableType = PLDM_BIOS_STRING_TABLE % (getUint8t() + 1);

    std::vector<uint8_t> requestMsg{};
    requestMsg.resize(
        hdrSize + PLDM_SET_BIOS_TABLE_MIN_REQ_BYTES + sizeof(tableData));

    auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());

    encode_set_bios_table_req(
        instanceId, transferHandle, transferFlag, tableType,
        reinterpret_cast<uint8_t*>(&tableData), sizeof(tableData), request,
        requestMsg.size() - hdrSize);
}
