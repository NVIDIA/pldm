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
    uint32_t transferHandle{0};
    uint8_t transferOpFlag = PLDM_ACKNOWLEDGEMENT_ONLY % (getUint8t() + 1);

    std::cout << (int)transferOpFlag << std::endl;
    uint16_t attributehandle{0};
    uint32_t retTransferHandle{0};
    uint8_t retTransferOpFlag = getUint8t();
    uint16_t retattributehandle{0};
    getFromCin(&transferHandle, sizeof(uint32_t));
    getFromCin(&attributehandle, sizeof(uint16_t));

    auto size = hdrSize + sizeof(transferHandle) + sizeof(transferOpFlag) +
                sizeof(attributehandle);
    std::vector<uint8_t> requestMsg{};
    requestMsg.resize(size);

    auto req = reinterpret_cast<pldm_msg*>(requestMsg.data());
    struct pldm_get_bios_attribute_current_value_by_handle_req* request =
        reinterpret_cast<
            struct pldm_get_bios_attribute_current_value_by_handle_req*>(
            req->payload);

    request->transfer_handle = htole32(transferHandle);
    request->transfer_op_flag = transferOpFlag;
    request->attribute_handle = htole16(attributehandle);

    decode_get_bios_attribute_current_value_by_handle_req(
        req, requestMsg.size() - hdrSize, &retTransferHandle,
        &retTransferOpFlag, &retattributehandle);
    uint8_t instanceId = getUint8t();
    uint8_t transferFlag = PLDM_START_AND_END % (getUint8t() + 1);
    uint32_t attributeData{0};
    getFromCin(&transferHandle, sizeof(uint32_t));
    getFromCin(&attributeData, sizeof(uint32_t));
    auto setReqSize = hdrSize + PLDM_SET_BIOS_ATTR_CURR_VAL_MIN_REQ_BYTES +
                      sizeof(attributeData);
    std::vector<uint8_t> requestMsg2{};
    requestMsg2.resize(setReqSize);

    auto request2 = reinterpret_cast<pldm_msg*>(requestMsg2.data());
    encode_set_bios_attribute_current_value_req(
        instanceId, transferHandle, transferFlag,
        reinterpret_cast<uint8_t*>(&attributeData), sizeof(attributeData),
        request2, requestMsg2.size() - hdrSize);
}
