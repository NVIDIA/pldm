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
#include "fw-update/device_updater.hpp"
#include "fw-update/package_parser.hpp"
#include "fw-update/update_manager.hpp"
#include "pldmd/dbus_impl_requester.hpp"
#include "requester/handler.hpp"

#include <vector>

using namespace pldm;
using namespace pldm::fw_update;

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    constexpr auto hdrSize = sizeof(pldm_msg_hdr);
    auto reqLen = hdrSize + sizeof(pldm_apply_complete_req);

    std::vector<uint8_t> applyCompleteReq(reqLen);

    getFromCin(reinterpret_cast<char*>(applyCompleteReq.data()), reqLen);

    auto requestMsg =
        reinterpret_cast<const pldm_msg*>(applyCompleteReq.data());
    uint8_t outApplyResult = 0;

    bitfield16_t outCompActivationModification{};

    decode_apply_complete_req(requestMsg, sizeof(pldm_apply_complete_req),
                              &outApplyResult, &outCompActivationModification);

    uint8_t instanceId = 6;
    uint8_t completionCode = PLDM_SUCCESS;
    auto respLen = hdrSize + sizeof(completionCode);

    std::vector<uint8_t> applyCompleteResponse1(respLen);

    getFromCin(reinterpret_cast<char*>(applyCompleteResponse1.data()), respLen);
    getFromCin(reinterpret_cast<char*>(&instanceId), sizeof(uint8_t));
    getFromCin(reinterpret_cast<char*>(&completionCode), sizeof(uint8_t));

    auto responseMsg =
        reinterpret_cast<pldm_msg*>((applyCompleteResponse1.data()));

    encode_apply_complete_resp(instanceId, completionCode, responseMsg,
                               sizeof(completionCode));
}
