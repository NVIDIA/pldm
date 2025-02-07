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
    unsigned short count{1};
    getFromCin(reinterpret_cast<char*>(&count), sizeof(short));

    for (int i = 0; i < count; ++i)
    {
        pldm_pdr_get_record_count(repo);

        pldm_pdr_get_repo_size(repo);

        std::vector<uint8_t> data{0};
        uint8_t pdrSize{1};
        uint32_t rcrdHndl{0u};
        bool isRemote = getBoolCin();

        getFromCin(&pdrSize, sizeof(uint8_t));
        getFromCin(&rcrdHndl, sizeof(uint32_t));

        data.resize(pdrSize);
        getFromCin(reinterpret_cast<char*>(data.data()), pdrSize);

        pldm_pdr_add(repo, data.data(), data.size(), rcrdHndl, isRemote);

        pldm_pdr_get_record_count(repo);

        pldm_pdr_get_repo_size(repo);
    }

    pldm_pdr_remove_remote_pdrs(repo);

    pldm_pdr_destroy(repo);
}
