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

#include <cassert>

// #include <sdeventplus/test/sdevent.hpp>

using namespace pldm;
using namespace pldm::fw_update;

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    constexpr auto hdrSize = sizeof(pldm_msg_hdr);
    uint8_t instanceId = 1;
    uint32_t maxTransferSize = 512;
    uint16_t numOfComp = 3;
    uint8_t maxOutstandingTransferReq = 2;
    uint16_t pkgDataLen = 0x1234;
    std::vector<char> compImgSetVerStr(16);
    variable_field compImgSetVerStrInfo{};

    getFromCin(reinterpret_cast<char*>(&instanceId), 1);
    getFromCin(reinterpret_cast<char*>(&maxTransferSize), 4);
    getFromCin(reinterpret_cast<char*>(&numOfComp), 2);
    getFromCin(reinterpret_cast<char*>(&pkgDataLen), 2);
    getFromCin(compImgSetVerStr.data(), 16);

    compImgSetVerStrInfo.ptr =
        reinterpret_cast<const uint8_t*>(compImgSetVerStr.data());
    compImgSetVerStrInfo.length = 16;

    std::array<uint8_t, hdrSize + sizeof(struct pldm_request_update_req) + 16>
        request{};
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    encode_request_update_req(
        instanceId, maxTransferSize, numOfComp, maxOutstandingTransferReq,
        pkgDataLen, PLDM_STR_TYPE_ASCII, 16, &compImgSetVerStrInfo, requestMsg,
        sizeof(struct pldm_request_update_req) + 16);
}
