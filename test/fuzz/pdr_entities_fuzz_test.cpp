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

entity_association_containment_type getRandomEacType()
{
    return getBoolCin() ? PLDM_ENTITY_ASSOCIAION_PHYSICAL
                        : PLDM_ENTITY_ASSOCIAION_LOGICAL;
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    uint16_t entityInstanceNb{0};

    getFromCin(&entityInstanceNb, sizeof(uint16_t));

    pldm_entity entities[11]{};

    entities[0].entity_type = 1;
    entities[1].entity_type = 2;
    entities[2].entity_type = 3;
    entities[3].entity_type = 2;
    entities[4].entity_type = 3;
    entities[5].entity_type = 4;
    entities[6].entity_type = 5;
    entities[7].entity_type = 5;
    entities[8].entity_type = 5;
    entities[9].entity_type = 6;
    entities[10].entity_type = 7;

    auto tree = pldm_entity_association_tree_init();

    auto l1 = pldm_entity_association_tree_add(
        tree, &entities[0], entityInstanceNb, nullptr, getRandomEacType());

    pldm_entity_get_num_children(l1, getRandomEacType());

    auto repo = pldm_pdr_init();

    uint32_t currRecHandle{};
    uint32_t nextRecHandle{};

    std::vector<pldm_entity_node*> nodes{l1};

    uint8_t count = getUint8t();
    for (uint8_t cntr = 0; cntr < count; ++cntr)
    {
        auto randomNode = nodes[(nodes.size() - 1) % (getUint8t() + 1)];
        nodes.push_back(pldm_entity_association_tree_add(
            tree, &entities[10 % (getUint8t() + 1)], 89, randomNode,
            getRandomEacType()));

        pldm_pdr_get_record_count(repo);

        uint8_t* data = nullptr;
        uint32_t size{};

        pldm_pdr_find_record(repo, currRecHandle, &data, &size, &nextRecHandle);

        randomNode = nodes[(nodes.size() - 1) % (getUint8t() + 1)];
        pldm_entity_get_num_children(randomNode, getRandomEacType());

        currRecHandle = nextRecHandle;
    }

    pldm_pdr_destroy(repo);
    pldm_entity_association_tree_destroy(tree);
}
