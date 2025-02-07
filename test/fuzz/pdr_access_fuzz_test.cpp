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

    auto repo = pldm_pdr_init();
    uint8_t count{1};
    uint32_t size{};
    uint32_t nextRecHdl{};
    uint8_t* outData = nullptr;
    uint8_t pdrSize{1};
    uint32_t rcrdHndl{0u};
    bool isRemote{false};
    count = getchar();

    for (uint8_t cnt = 0; cnt < count; ++cnt)
    {
        std::vector<uint8_t> in{0};

        isRemote = getBoolCin();
        getFromCin(&pdrSize, sizeof(uint8_t));
        getFromCin(&rcrdHndl, sizeof(uint32_t));

        in.resize(pdrSize);

        pldm_pdr_add(repo, reinterpret_cast<uint8_t*>(in.data()), sizeof(in),
                     rcrdHndl, isRemote);

        pldm_pdr_find_record(repo, 0, &outData, &size, &nextRecHdl);
        outData = nullptr;

        pldm_pdr_find_record(repo, 1, &outData, &size, &nextRecHdl);
        outData = nullptr;

        pldm_pdr_find_record(repo, htole32(0xdeaddead), &outData, &size,
                             &nextRecHdl);
        outData = nullptr;
    }

    std::vector<uint8_t> in2{0};
    pdrSize = 1;
    rcrdHndl = 0u;
    isRemote = getBoolCin();

    getFromCin(&pdrSize, sizeof(uint8_t));
    getFromCin(&rcrdHndl, sizeof(uint32_t));

    in2.resize(pdrSize);

    pldm_pdr_add(repo, reinterpret_cast<uint8_t*>(in2.data()), sizeof(in2),
                 pdrSize, isRemote);

    pldm_pdr_find_record(repo, 0, &outData, &size, &nextRecHdl);

    outData = nullptr;
    pldm_pdr_find_record(repo, 1, &outData, &size, &nextRecHdl);

    outData = nullptr;
    pldm_pdr_find_record(repo, 2, &outData, &size, &nextRecHdl);

    outData = nullptr;
    pldm_pdr_find_record(repo, 3, &outData, &size, &nextRecHdl);

    outData = nullptr;
    pldm_pdr_find_record(repo, 4, &outData, &size, &nextRecHdl);
    outData = nullptr;

    pldm_pdr_destroy(repo);
}
