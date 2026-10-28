/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "libpldm/entity.h"
#include "libpldm/platform.h"

#include <endian.h>

// Everything inventory_manager.hpp includes is pulled in first, so that only
// InventoryManager itself is compiled with its private members exposed.
#include "common/types.hpp"
#include "oem/nvidia/platform-mc/entity_tree.hpp"

#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "oem/nvidia/platform-mc/inventory_manager.hpp"
#undef private
#ifdef __clang__
#pragma clang diagnostic pop
#endif

using namespace pldm::pdr;
using namespace pldm::platform_mc;
using namespace pldm::platform_mc::oem_nvidia;
using pldm::tid_t;

namespace
{

constexpr tid_t tid = 1;

/** The singleton InventoryManager, reset per test. No bus or terminus is
 *  needed. */
class InventoryManagerTest : public testing::Test
{
  protected:
    InventoryManagerTest()
    {
        inventory.reset();
    }

    ~InventoryManagerTest() override
    {
        inventory.reset();
    }

    EntityTree& tree()
    {
        return inventory.entityTree(tid);
    }

    InventoryManager& inventory = InventoryManager::get();
};

EntityId id(ContainerID containerId, EntityType type, EntityInstance instance)
{
    return EntityId{EntityInfo{containerId, type, instance}};
}

} // namespace

TEST_F(InventoryManagerTest, registrationIsPerTerminus)
{
    EXPECT_EQ(std::nullopt, inventory.instance(tid));
    inventory.setInstance(tid, 3);
    inventory.setTerminusName(tid, "ProcessorModule_1");
    EXPECT_EQ(3, inventory.instance(tid));
    EXPECT_EQ(std::nullopt, inventory.instance(tid + 1));
    EXPECT_EQ("ProcessorModule_1", inventory.terminusData[tid].name);

    inventory.setInstance(tid, std::nullopt);
    EXPECT_EQ(std::nullopt, inventory.instance(tid));

    inventory.setInstance(tid, 2);
    tree().add(10, id(0, PLDM_ENTITY_PROC_IO_MODULE, 0), {});
    inventory.forget(tid);
    EXPECT_EQ(std::nullopt, inventory.instance(tid));
    EXPECT_TRUE(tree().empty());
}

TEST_F(InventoryManagerTest, addEntityAssociationFeedsTheTerminusTree)
{
    // Table 95: header, containerID 10, physical, PIOM(0,0) with CPU(10,0).
    std::vector<uint8_t> pdr(
        sizeof(pldm_pdr_hdr) + sizeof(pldm_pdr_entity_association), 0);
    auto* hdr = reinterpret_cast<pldm_pdr_hdr*>(pdr.data());
    hdr->type = PLDM_PDR_ENTITY_ASSOCIATION;
    auto* body = reinterpret_cast<pldm_pdr_entity_association*>(
        pdr.data() + sizeof(pldm_pdr_hdr));
    body->container_id = htole16(10);
    body->association_type = PLDM_ENTITY_ASSOCIAION_PHYSICAL;
    body->container.entity_type = htole16(PLDM_ENTITY_PROC_IO_MODULE);
    body->num_children = 1;
    body->children[0].entity_type = htole16(PLDM_ENTITY_PROC);
    body->children[0].entity_container_id = htole16(10);

    EXPECT_EQ(EntityTree::AddResult::added,
              inventory.addEntityAssociation(tid, pdr));
    ASSERT_NE(nullptr, tree().container(10));
    EXPECT_EQ(PLDM_ENTITY_PROC_IO_MODULE, tree().container(10)->id.type);
    EXPECT_TRUE(inventory.entityTree(tid + 1).empty());
}
