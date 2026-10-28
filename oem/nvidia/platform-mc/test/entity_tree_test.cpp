/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION &
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
#include "libpldm/entity.h"
#include "libpldm/platform.h"

#include "oem/nvidia/platform-mc/entity_tree.hpp"

#include <endian.h>

#include <gtest/gtest.h>

using namespace pldm::pdr;
using namespace pldm::platform_mc::oem_nvidia;
using AddResult = EntityTree::AddResult;

namespace
{

// EntityInfo tuple order is (containerId, type, instance).
EntityId id(ContainerID containerId, EntityType type, EntityInstance instance)
{
    return EntityId{EntityInfo{containerId, type, instance}};
}

// Wire-format (little-endian) pldm_entity for `id`.
pldm_entity toWire(const EntityId& id)
{
    pldm_entity wire{};
    wire.entity_type = htole16(id.type);
    wire.entity_instance_num = htole16(id.instance);
    wire.entity_container_id = htole16(id.containerId);
    return wire;
}

// Builds a Table 95 PDR (header included) for `containerId` with the given
// container entity and children.
std::vector<uint8_t> makePdr(ContainerID containerId, uint8_t associationType,
                             const EntityId& container,
                             const std::vector<EntityId>& children)
{
    const size_t bodySize =
        sizeof(pldm_pdr_entity_association) - sizeof(pldm_entity) +
        children.size() * sizeof(pldm_entity);
    std::vector<uint8_t> pdr(sizeof(pldm_pdr_hdr) + bodySize, 0);

    // Multi-byte fields are written in wire (little-endian) order so the
    // bytes are the same on any build host.
    auto* hdr = reinterpret_cast<pldm_pdr_hdr*>(pdr.data());
    hdr->type = PLDM_PDR_ENTITY_ASSOCIATION;
    hdr->length = htole16(static_cast<uint16_t>(bodySize));

    auto* body = reinterpret_cast<pldm_pdr_entity_association*>(
        pdr.data() + sizeof(pldm_pdr_hdr));
    body->container_id = htole16(containerId);
    body->association_type = associationType;
    body->container = toWire(container);
    body->num_children = children.size();
    for (size_t i = 0; i < children.size(); ++i)
    {
        body->children[i] = toWire(children[i]);
    }
    return pdr;
}

} // namespace

// The children of a PDR containerID via the container iteration; nullptr if
// the containerID is unknown.
const std::set<EntityId>* childrenOf(const EntityTree& tree,
                                     ContainerID containerId)
{
    for (const auto& [id, record] : tree)
    {
        if (id == containerId)
        {
            return &record.children;
        }
    }
    return nullptr;
}

TEST(EntityIdTest, convertsFromEntityInfoAndPldmEntity)
{
    EntityId fromInfo{EntityInfo{7, PLDM_ENTITY_LOGICAL | PLDM_ENTITY_PROC, 3}};
    EXPECT_EQ(PLDM_ENTITY_LOGICAL | PLDM_ENTITY_PROC, fromInfo.type);
    EXPECT_EQ(3, fromInfo.instance);
    EXPECT_EQ(7, fromInfo.containerId);
    EXPECT_EQ(PLDM_ENTITY_PROC, fromInfo.baseType());
    EXPECT_TRUE(fromInfo.isLogical());
    EXPECT_EQ((EntityInfo{7, PLDM_ENTITY_LOGICAL | PLDM_ENTITY_PROC, 3}),
              fromInfo.toEntityInfo());

    pldm_entity wire{.entity_type = PLDM_ENTITY_PROC,
                     .entity_instance_num = 3,
                     .entity_container_id = 7};
    EntityId fromWire{wire};
    EXPECT_FALSE(fromWire.isLogical());
    EXPECT_NE(fromInfo, fromWire); // bit 15 is part of the identity
    EXPECT_EQ(fromInfo.baseType(), fromWire.baseType());
    EXPECT_LT(fromWire, fromInfo);
}

TEST(EntityTest, bindAndUnbind)
{
    const std::string chassis{"/xyz/openbmc_project/inventory/system/chassis"};
    auto chassisId = id(PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID,
                        PLDM_ENTITY_SYSTEM_CHASSIS, 1);

    Entity entity(chassisId);
    EXPECT_EQ(chassisId, entity.id);
    EXPECT_FALSE(entity.isBound());
    EXPECT_TRUE(entity.inventory().empty());

    entity.bind({chassis});
    EXPECT_TRUE(entity.isBound());
    EXPECT_EQ((std::vector<std::string>{chassis}), entity.inventory());

    // Bound to nothing is still bound: the platform has no object for it.
    entity.bind({});
    EXPECT_TRUE(entity.isBound());
    EXPECT_TRUE(entity.inventory().empty());

    entity.unbind();
    EXPECT_FALSE(entity.isBound());
    EXPECT_TRUE(entity.inventory().empty());
}

TEST(EntityTreeTest, addPdrDecodesTable95)
{
    EntityTree tree;
    auto module = id(PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID,
                     PLDM_ENTITY_PROC_IO_MODULE, 1);
    auto cpu = id(10, PLDM_ENTITY_PROC, 1);
    auto vreg = id(10, PLDM_ENTITY_POWER_SUPPLY, 1);

    auto pdr =
        makePdr(10, PLDM_ENTITY_ASSOCIAION_PHYSICAL, module, {cpu, vreg});
    EXPECT_EQ(AddResult::added, tree.addPdr(pdr));

    ASSERT_NE(nullptr, tree.container(10));
    EXPECT_EQ(module, tree.container(10)->id);
    // One node per entity involved: module, cpu, vreg.
    EXPECT_NE(nullptr, tree.find(module));
    EXPECT_NE(nullptr, tree.find(cpu));
    EXPECT_NE(nullptr, tree.find(vreg));
    ASSERT_NE(nullptr, childrenOf(tree, 10));
    EXPECT_EQ((std::set<EntityId>{cpu, vreg}), *childrenOf(tree, 10));
    EXPECT_EQ(childrenOf(tree, 10), tree.children(10));
    EXPECT_EQ(nullptr, tree.children(99));

    EXPECT_EQ(nullptr, tree.container(99));
    EXPECT_EQ(nullptr, childrenOf(tree, 99));
    EXPECT_EQ(nullptr,
              tree.container(PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID));
}

TEST(EntityTreeTest, addPdrRejectsShortPdr)
{
    EntityTree tree;
    auto module = id(PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID,
                     PLDM_ENTITY_PROC_IO_MODULE, 1);
    auto cpu = id(10, PLDM_ENTITY_PROC, 1);

    auto pdr = makePdr(10, PLDM_ENTITY_ASSOCIAION_PHYSICAL, module, {cpu});

    // Truncated inside the fixed fields.
    std::vector<uint8_t> tooShort(pdr.begin(), pdr.begin() + 8);
    EXPECT_EQ(AddResult::malformed, tree.addPdr(tooShort));

    // containedEntityCount promises more children than the buffer holds.
    auto lying = pdr;
    reinterpret_cast<pldm_pdr_entity_association*>(
        lying.data() + sizeof(pldm_pdr_hdr))
        ->num_children = 2;
    EXPECT_EQ(AddResult::malformed, tree.addPdr(lying));

    // Truncated inside the single child's body.
    for (size_t size = pdr.size() - sizeof(pldm_entity) + 1; size < pdr.size();
         ++size)
    {
        std::vector<uint8_t> cut(pdr.begin(), pdr.begin() + size);
        EXPECT_EQ(AddResult::malformed, tree.addPdr(cut)) << size;
    }

    // Not an Entity Association PDR at all.
    auto wrongType = pdr;
    reinterpret_cast<pldm_pdr_hdr*>(wrongType.data())->type =
        PLDM_STATE_SENSOR_PDR;
    EXPECT_EQ(AddResult::malformed, tree.addPdr(wrongType));
    EXPECT_TRUE(tree.empty());
}

TEST(EntityTreeTest, systemContainerIdIsReserved)
{
    // §9.5: 0x0000 names the overall system, which has no PDR of its own.
    EntityTree tree;
    auto chassis = id(PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID,
                      PLDM_ENTITY_SYSTEM_CHASSIS, 1);
    auto board =
        id(PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID, PLDM_ENTITY_SYS_BOARD, 1);

    EXPECT_EQ(AddResult::reservedContainer,
              tree.add(PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID, chassis,
                       {board}));
    EXPECT_EQ(AddResult::reservedContainer,
              tree.addPdr(
                  makePdr(PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID,
                          PLDM_ENTITY_ASSOCIAION_PHYSICAL, chassis, {board})));
    EXPECT_TRUE(tree.empty());
    EXPECT_EQ(nullptr,
              tree.container(PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID));
}

TEST(EntityTreeTest, addPdrAcceptsZeroChildrenAndTrailingBytes)
{
    EntityTree tree;
    auto module = id(PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID,
                     PLDM_ENTITY_PROC_IO_MODULE, 1);

    // Exactly the Table 95 fixed size: containedEntityCount == 0.
    auto pdr = makePdr(10, PLDM_ENTITY_ASSOCIAION_PHYSICAL, module, {});
    EXPECT_EQ(AddResult::added, tree.addPdr(pdr));
    EXPECT_EQ(module, tree.container(10)->id);
    EXPECT_TRUE(childrenOf(tree, 10)->empty());

    // Extra bytes after the record are tolerated.
    auto cpu = id(20, PLDM_ENTITY_PROC, 1);
    auto padded = makePdr(20, PLDM_ENTITY_ASSOCIAION_PHYSICAL, module, {cpu});
    padded.resize(padded.size() + 4, 0xAA);
    EXPECT_EQ(AddResult::added, tree.addPdr(padded));
    EXPECT_EQ((std::set<EntityId>{cpu}), *childrenOf(tree, 20));
}

TEST(EntityTreeTest, linkedPdrsMergeChildren)
{
    // DSP0248 §11.3: PDRs sharing a containerID form one association.
    EntityTree tree;
    auto chassis = id(PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID,
                      PLDM_ENTITY_SYSTEM_CHASSIS, 1);
    auto psu1 = id(123, PLDM_ENTITY_POWER_SUPPLY, 1);
    auto psu2 = id(123, PLDM_ENTITY_POWER_SUPPLY, 2);
    auto fan1 = id(123, PLDM_ENTITY_FAN, 1);

    EXPECT_EQ(AddResult::added,
              tree.addPdr(makePdr(123, PLDM_ENTITY_ASSOCIAION_PHYSICAL, chassis,
                                  {psu1, psu2})));
    EXPECT_EQ(AddResult::merged,
              tree.addPdr(makePdr(123, PLDM_ENTITY_ASSOCIAION_LOGICAL, chassis,
                                  {psu2, fan1})));

    EXPECT_EQ(4u, tree.size());
    EXPECT_EQ((std::set<EntityId>{psu1, psu2, fan1}), *childrenOf(tree, 123));
}

TEST(EntityTreeTest, conflictingContainerEntityIsRejected)
{
    EntityTree tree;
    auto chassis = id(PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID,
                      PLDM_ENTITY_SYSTEM_CHASSIS, 1);
    auto board =
        id(PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID, PLDM_ENTITY_SYS_BOARD, 1);
    auto psu1 = id(123, PLDM_ENTITY_POWER_SUPPLY, 1);
    auto fan1 = id(123, PLDM_ENTITY_FAN, 1);

    EXPECT_EQ(AddResult::added, tree.add(123, chassis, {psu1}));
    EXPECT_EQ(AddResult::containerConflict, tree.add(123, board, {fan1}));

    // The conflicting PDR leaves the tree untouched.
    EXPECT_EQ(chassis, tree.container(123)->id);
    EXPECT_EQ((std::set<EntityId>{psu1}), *childrenOf(tree, 123));
    EXPECT_EQ(nullptr, tree.find(board));
    EXPECT_EQ(nullptr, tree.find(fan1));

    // Container entity compare is exact (raw type), so a logical spelling of
    // the same entity is a conflict.
    auto logicalChassis =
        id(PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID,
           PLDM_ENTITY_LOGICAL | PLDM_ENTITY_SYSTEM_CHASSIS, 1);
    EXPECT_EQ(AddResult::containerConflict,
              tree.addPdr(makePdr(123, PLDM_ENTITY_ASSOCIAION_PHYSICAL,
                                  logicalChassis, {psu1})));
}

TEST(EntityTreeTest, oneNodePerEntityAcrossPdrs)
{
    // The CPU is a child of the module PDR and the container entity of its
    // own PDR: both refer to the same node.
    EntityTree tree;
    auto module = id(PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID,
                     PLDM_ENTITY_PROC_IO_MODULE, 1);
    auto cpu = id(10, PLDM_ENTITY_PROC, 1);
    auto memctl = id(20, PLDM_ENTITY_MEMORY_CONTROLLER, 1);
    tree.add(10, module, {cpu});
    tree.add(20, cpu, {memctl});

    EXPECT_EQ(3u, tree.size());
    // One shared node: the CPU as child of PDR 10 and container of PDR 20.
    EXPECT_EQ(tree.find(cpu), tree.container(20));

    // An entity may define several containerIDs (seen from real firmware).
    tree.add(30, module, {});
    EXPECT_EQ(3u, tree.size());
    EXPECT_EQ(tree.container(10), tree.container(30));
}

TEST(EntityTreeTest, parentAndAncestorFollowPhysicalChain)
{
    // system -> module(cid 10) -> cpu(cid 20) -> memctl(cid 30)
    EntityTree tree;
    auto module = id(PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID,
                     PLDM_ENTITY_PROC_IO_MODULE, 4);
    auto cpu = id(10, PLDM_ENTITY_PROC, 1);
    auto memctl = id(20, PLDM_ENTITY_MEMORY_CONTROLLER, 1);
    auto dimm = id(30, PLDM_ENTITY_MEMORY_MODULE, 1);
    tree.add(10, module, {cpu});
    tree.add(20, cpu, {memctl});
    tree.add(30, memctl, {dimm});

    const auto& dimmNode = *tree.find(dimm);
    EXPECT_EQ(tree.find(memctl), tree.parent(dimmNode));
    EXPECT_EQ(nullptr, tree.parent(*tree.find(module)));

    // ancestor() is strict: starts at the parent.
    EXPECT_EQ(tree.find(memctl),
              tree.ancestor(dimmNode, PLDM_ENTITY_MEMORY_CONTROLLER));
    EXPECT_EQ(tree.find(cpu), tree.ancestor(dimmNode, PLDM_ENTITY_PROC));
    EXPECT_EQ(tree.find(module),
              tree.ancestor(dimmNode, PLDM_ENTITY_PROC_IO_MODULE));
    EXPECT_EQ(nullptr, tree.ancestor(dimmNode, PLDM_ENTITY_MEMORY_MODULE));
    EXPECT_EQ(nullptr, tree.ancestor(dimmNode, PLDM_ENTITY_FAN));
    EXPECT_EQ(4,
              tree.ancestor(dimmNode, PLDM_ENTITY_PROC_IO_MODULE)->id.instance);

    // Logical bit is ignored when matching the type.
    EXPECT_EQ(tree.find(cpu),
              tree.ancestor(dimmNode, PLDM_ENTITY_LOGICAL | PLDM_ENTITY_PROC));

    // The EntityId overload works for entities not in the tree.
    auto stray = id(20, PLDM_ENTITY_SYS_BUS, 9);
    EXPECT_EQ(nullptr, tree.find(stray));
    EXPECT_EQ(tree.find(module),
              tree.ancestor(stray, PLDM_ENTITY_PROC_IO_MODULE));
    EXPECT_EQ(nullptr,
              tree.ancestor(id(99, PLDM_ENTITY_SYS_BUS, 9), PLDM_ENTITY_PROC));
    EXPECT_EQ(nullptr,
              tree.ancestor(id(PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID,
                               PLDM_ENTITY_PROC, 1),
                            PLDM_ENTITY_PROC));
}

TEST(EntityTreeTest, ancestorTerminatesOnCycle)
{
    EntityTree tree;
    // Malformed repository: 10's container lives in 20, 20's lives in 10.
    tree.add(10, id(20, PLDM_ENTITY_SYS_BOARD, 1), {});
    tree.add(20, id(10, PLDM_ENTITY_ADD_IN_CARD, 1), {});

    EXPECT_EQ(nullptr,
              tree.ancestor(id(10, PLDM_ENTITY_PROC, 1), PLDM_ENTITY_PROC));
}

TEST(EntityTreeTest, logicalContainerKeepsPhysicalChildIds)
{
    // DSP0248 §11.4 / Fig. 12: contained entities of a logical container
    // keep the containerID of their physical parent.
    EntityTree tree;
    auto chassis = id(PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID,
                      PLDM_ENTITY_SYSTEM_CHASSIS, 1);
    auto psu1 = id(123, PLDM_ENTITY_POWER_SUPPLY, 1);
    auto psu2 = id(123, PLDM_ENTITY_POWER_SUPPLY, 2);
    auto logicalPsu =
        id(123, PLDM_ENTITY_LOGICAL | PLDM_ENTITY_POWER_SUPPLY, 1);
    tree.add(123, chassis, {psu1, psu2, logicalPsu});
    tree.add(828, logicalPsu, {psu1, psu2});

    for (const auto& child : *childrenOf(tree, 828))
    {
        EXPECT_EQ(123, child.containerId);
        // parent() follows the physical chain, not the logical container.
        EXPECT_EQ(tree.find(chassis), tree.parent(*tree.find(child)));
    }
    EXPECT_EQ(tree.find(chassis), tree.ancestor(*tree.find(logicalPsu),
                                                PLDM_ENTITY_SYSTEM_CHASSIS));
}

TEST(EntityTreeTest, inventoryBindings)
{
    EntityTree tree;
    auto module = id(PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID,
                     PLDM_ENTITY_PROC_IO_MODULE, 1);
    auto cpu = id(10, PLDM_ENTITY_PROC, 1);
    auto bus = id(20, PLDM_ENTITY_SYS_BUS, 1);
    tree.add(10, module, {cpu});
    tree.add(20, cpu, {bus});

    // Nothing bound yet: each container entity's own inventory is empty.
    EXPECT_FALSE(tree.find(cpu)->isBound());
    EXPECT_TRUE(tree.container(10)->inventory().empty());
    EXPECT_TRUE(tree.container(20)->inventory().empty());

    tree.find(module)->bind({"/inv/module0"});
    tree.find(cpu)->bind({"/inv/cpu0"});
    tree.find(bus)->bind({}); // no own inventory

    const auto& busNode = *tree.find(bus);
    EXPECT_TRUE(busNode.isBound());
    EXPECT_TRUE(busNode.inventory().empty());
    EXPECT_EQ((std::vector<std::string>{"/inv/module0"}),
              tree.container(10)->inventory());
    EXPECT_EQ((std::vector<std::string>{"/inv/cpu0"}),
              tree.container(20)->inventory());

    tree.unbindAll();
    EXPECT_FALSE(tree.find(cpu)->isBound());
    EXPECT_TRUE(tree.find(cpu)->inventory().empty());
    EXPECT_TRUE(tree.container(20)->inventory().empty());
    // Topology survives an unbind.
    EXPECT_NE(nullptr, tree.find(bus));
    EXPECT_EQ(tree.find(cpu), tree.parent(*tree.find(bus)));
}

TEST(EntityTreeTest, emplaceCreatesStrayEntities)
{
    EntityTree tree;
    auto stray = id(10, PLDM_ENTITY_SYS_BUS, 1);
    EXPECT_EQ(nullptr, tree.find(stray));

    auto& node = tree.emplace(stray);
    EXPECT_EQ(stray, node.id);
    EXPECT_FALSE(node.isBound());
    EXPECT_EQ(&node, tree.find(stray));
    EXPECT_EQ(&node, &tree.emplace(stray)); // idempotent
    EXPECT_EQ(tree.begin(), tree.end());    // no containers created

    tree.clear();
    EXPECT_TRUE(tree.empty());
    EXPECT_EQ(tree.begin(), tree.end());
}

TEST(EntityTreeTest, iteratesContainersInIdOrder)
{
    EntityTree tree;
    tree.add(20,
             id(PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID, PLDM_ENTITY_SYS_BOARD,
                2),
             {});
    tree.add(10,
             id(PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID, PLDM_ENTITY_SYS_BOARD,
                1),
             {id(10, PLDM_ENTITY_PROC, 1)});

    std::vector<ContainerID> seen;
    for (const auto& [containerId, container] : tree)
    {
        seen.push_back(containerId);
        EXPECT_EQ(PLDM_ENTITY_SYS_BOARD, container.entity.baseType());
        EXPECT_EQ(containerId == 10 ? 1u : 0u, container.children.size());
    }
    EXPECT_EQ((std::vector<ContainerID>{10, 20}), seen);
}
