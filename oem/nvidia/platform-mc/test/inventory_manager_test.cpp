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
#include "common/utils.hpp"
#include "oem/nvidia/platform-mc/entity_tree.hpp"

#include <exec/task.hpp>

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
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
using Paths = std::vector<std::string>;

namespace
{

constexpr tid_t tid = 1;
constexpr auto systemPath = "/inv/system";

/** The singleton InventoryManager, reset per test and fed an in-memory
 *  inventory snapshot. The binding walks only read that snapshot, so no bus
 *  or terminus is needed. */
class InventoryManagerTest : public testing::Test
{
  protected:
    InventoryManagerTest()
    {
        inventory.reset();
        inventory.systemInventoryPath = systemPath;
    }

    ~InventoryManagerTest() override
    {
        inventory.reset();
    }

    EntityTree& tree()
    {
        return inventory.entityTree(tid);
    }

    void addInventory(const std::string& path, EntityType type,
                      EntityInstance instance,
                      const std::string& parent = std::string{})
    {
        auto& data = inventory.terminusData[tid];
        data.inventories.emplace_back(path, type, instance);
        if (!parent.empty())
        {
            data.inventoryParents[path] = parent;
        }
    }

    /** The entity's own inventory once bound, without the ancestor walk. */
    Paths own(const EntityId& id)
    {
        inventory.findInventory(tid, id);
        return tree().emplace(id).inventory();
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

TEST_F(InventoryManagerTest, ownInventoryAndClosestFallback)
{
    // Board(0,1) contains an add-in card and a system bus.
    auto board = id(0, PLDM_ENTITY_ADD_IN_CARD, 1);
    auto card = id(10, PLDM_ENTITY_ADD_IN_CARD, 2);
    auto bus = id(10, PLDM_ENTITY_SYS_BUS, 1);
    tree().add(10, board, {card, bus});
    addInventory("/inv/board1", PLDM_ENTITY_ADD_IN_CARD, 1);
    addInventory("/inv/other/card2", PLDM_ENTITY_ADD_IN_CARD, 2, "/inv/other");
    addInventory("/inv/board1/card2", PLDM_ENTITY_ADD_IN_CARD, 2,
                 "/inv/board1");
    inventory.bindAll(tid);

    EXPECT_EQ(Paths{"/inv/board1"}, own(board));
    // Of two candidates the one under the parent's inventory wins.
    EXPECT_EQ(Paths{"/inv/board1/card2"}, own(card));
    // No inventory of its own: the closest ancestor's.
    EXPECT_TRUE(own(bus).empty());
    EXPECT_EQ(Paths{"/inv/board1"}, inventory.findInventory(tid, bus));

    // Without a candidate under the parent, the first candidate.
    inventory.terminusData[tid].inventoryParents.clear();
    inventory.bindAll(tid);
    EXPECT_EQ(Paths{"/inv/other/card2"}, own(card));

    // Entities no PDR lists, and unknown containers, end at the system.
    EXPECT_EQ(Paths{systemPath},
              inventory.findInventory(tid, id(0, PLDM_ENTITY_SYS_BUS, 9)));
    EXPECT_EQ(Paths{systemPath},
              inventory.findInventory(tid, id(99, PLDM_ENTITY_SYS_BUS, 9)));
    EXPECT_EQ(Paths{systemPath},
              inventory.containerInventory(inventory.terminusData[tid], 99));
}

TEST_F(InventoryManagerTest, processorInstanceResolution)
{
    // PIOM(0,1) > CPU(10,0) > SysBus(20,0) > CPU(30,0).
    auto module = id(0, PLDM_ENTITY_PROC_IO_MODULE, 1);
    auto cpu = id(10, PLDM_ENTITY_PROC, 0);
    tree().add(10, module, {cpu});
    tree().add(20, cpu, {id(20, PLDM_ENTITY_SYS_BUS, 0)});
    tree().add(30, id(20, PLDM_ENTITY_SYS_BUS, 0),
               {id(30, PLDM_ENTITY_PROC, 0)});
    addInventory("/inv/cpu0", PLDM_ENTITY_PROC, 0);
    addInventory("/inv/cpu1", PLDM_ENTITY_PROC, 1);
    addInventory("/inv/cpu1_twin", PLDM_ENTITY_PROC, 1);

    // The owning module's instance picks CPU_1, with every twin.
    EXPECT_EQ((Paths{"/inv/cpu1", "/inv/cpu1_twin"}), own(cpu));
    // A CPU under a CPU's container takes the module above that CPU.
    EXPECT_EQ((Paths{"/inv/cpu1", "/inv/cpu1_twin"}),
              own(id(20, PLDM_ENTITY_PROC, 0)));
    // Deeper than that the PDR instance stands.
    EXPECT_EQ(Paths{"/inv/cpu0"}, own(id(30, PLDM_ENTITY_PROC, 0)));

    // A configured instance wins over the hierarchy.
    inventory.setInstance(tid, 0);
    inventory.bindAll(tid);
    EXPECT_EQ(Paths{"/inv/cpu0"}, own(cpu));
}

TEST_F(InventoryManagerTest, memoryControllerBindsToTheModule)
{
    // PIOM(0,0) > CPU(10,0) > MemCtrl(20,0); MemCtrl(10,1) under the module.
    auto module = id(0, PLDM_ENTITY_PROC_IO_MODULE, 0);
    auto cpu = id(10, PLDM_ENTITY_PROC, 0);
    auto dram = id(20, PLDM_ENTITY_MEMORY_CONTROLLER, 0);
    auto moduleDram = id(10, PLDM_ENTITY_MEMORY_CONTROLLER, 1);
    tree().add(10, module, {cpu, moduleDram});
    tree().add(20, cpu, {dram});
    tree().add(40, dram, {id(40, PLDM_ENTITY_SYS_BUS, 5)});
    addInventory("/inv/module0", PLDM_ENTITY_PROC_IO_MODULE, 0);
    addInventory("/inv/cpu0", PLDM_ENTITY_PROC, 0);
    inventory.bindAll(tid);

    EXPECT_EQ(Paths{"/inv/module0"}, inventory.findInventory(tid, dram));
    // Directly under the module there is nothing to skip.
    EXPECT_EQ(Paths{"/inv/module0"}, inventory.findInventory(tid, moduleDram));
    // Children of the MemoryController inherit the module.
    EXPECT_EQ(Paths{"/inv/module0"},
              inventory.findInventory(tid, id(40, PLDM_ENTITY_SYS_BUS, 5)));

    // Module not in the inventory: the ProcessorModule named after the
    // terminus.
    inventory.reset();
    inventory.systemInventoryPath = systemPath;
    tree().add(10, module, {cpu});
    tree().add(20, cpu, {dram});
    addInventory("/inv/cpu0", PLDM_ENTITY_PROC, 0);
    addInventory("/inv/ProcessorModule_7", PLDM_ENTITY_PROC_IO_MODULE, 7);
    inventory.bindAll(tid);
    EXPECT_EQ(Paths{systemPath}, inventory.findInventory(tid, dram));
    inventory.setTerminusName(tid, "ProcessorModule_7");
    inventory.bindAll(tid);
    EXPECT_EQ(Paths{"/inv/ProcessorModule_7"},
              inventory.findInventory(tid, dram));
}

TEST_F(InventoryManagerTest, powerSupplyBindsToTheSiblingCpu)
{
    auto module = id(0, PLDM_ENTITY_PROC_IO_MODULE, 0);
    auto cpu = id(10, PLDM_ENTITY_PROC, 0);
    auto vreg = id(10, PLDM_ENTITY_POWER_SUPPLY, 0);
    tree().add(10, module, {cpu, vreg});
    addInventory("/inv/module0", PLDM_ENTITY_PROC_IO_MODULE, 0);
    addInventory("/inv/cpu0", PLDM_ENTITY_PROC, 0);
    inventory.bindAll(tid);
    EXPECT_EQ(Paths{"/inv/cpu0"}, inventory.findInventory(tid, vreg));

    // Without a CPU sibling the module's.
    inventory.reset();
    inventory.systemInventoryPath = systemPath;
    tree().add(10, module, {vreg});
    addInventory("/inv/module0", PLDM_ENTITY_PROC_IO_MODULE, 0);
    inventory.bindAll(tid);
    EXPECT_EQ(Paths{"/inv/module0"}, inventory.findInventory(tid, vreg));
}

TEST_F(InventoryManagerTest, cpuRedirections)
{
    // Module(0,0) > CPU(10,0) > SysBus(20,0); board(0,1) > bus(30,0).
    auto module = id(0, PLDM_ENTITY_PROC_IO_MODULE, 0);
    auto cpu = id(10, PLDM_ENTITY_PROC, 0);
    tree().add(10, module, {cpu});
    tree().add(20, cpu, {id(20, PLDM_ENTITY_SYS_BUS, 0)});
    tree().add(30, id(0, PLDM_ENTITY_ADD_IN_CARD, 1),
               {id(30, PLDM_ENTITY_SYS_BUS, 0)});
    addInventory("/inv/cpu0", PLDM_ENTITY_PROC, 0);

    const Paths cpuPaths{"/inv/cpu0"};
    EXPECT_EQ(cpuPaths, inventory.containerProcessorInventory(tid, 20));
    EXPECT_TRUE(inventory.containerProcessorInventory(tid, 10).empty());
    EXPECT_EQ(cpuPaths, inventory.processorInventory(tid, 20));
    EXPECT_EQ(cpuPaths, inventory.processorInventory(tid, 10));
    EXPECT_TRUE(inventory.processorInventory(tid, 30).empty());
    EXPECT_TRUE(inventory.processorInventory(tid, 99).empty());
    EXPECT_EQ(cpuPaths, inventory.moduleProcessorInventory(tid));

    inventory.forget(tid);
    EXPECT_TRUE(inventory.moduleProcessorInventory(tid).empty());
}

TEST_F(InventoryManagerTest, fallbackAssociation)
{
    addInventory("/inv/module0", PLDM_ENTITY_PROC_IO_MODULE, 0);
    addInventory("/inv/cpu0", PLDM_ENTITY_PROC, 0);

    // Types without a dedicated home are never a fallback.
    EXPECT_FALSE(inventory.isFallbackAssociation(tid, {systemPath},
                                                 PLDM_ENTITY_ADD_IN_CARD));
    EXPECT_FALSE(
        inventory.isFallbackAssociation(tid, {}, PLDM_ENTITY_ADD_IN_CARD));
    // Dedicated types: nothing, the system or a non-home is a fallback.
    EXPECT_TRUE(inventory.isFallbackAssociation(tid, {}, PLDM_ENTITY_PROC));
    EXPECT_TRUE(
        inventory.isFallbackAssociation(tid, {systemPath}, PLDM_ENTITY_PROC));
    EXPECT_TRUE(inventory.isFallbackAssociation(tid, {"/inv/module0"},
                                                PLDM_ENTITY_SYS_BUS));
    EXPECT_FALSE(inventory.isFallbackAssociation(tid, {"/inv/cpu0"},
                                                 PLDM_ENTITY_SYS_BUS));
    EXPECT_FALSE(inventory.isFallbackAssociation(
        tid, {"/inv/module0"}, PLDM_ENTITY_MEMORY_CONTROLLER));
    // The logical bit is ignored; all paths must be homes.
    EXPECT_FALSE(inventory.isFallbackAssociation(
        tid, {"/inv/cpu0"}, PLDM_ENTITY_LOGICAL | PLDM_ENTITY_PROC));
    EXPECT_TRUE(inventory.isFallbackAssociation(tid, {"/inv/cpu0", systemPath},
                                                PLDM_ENTITY_PROC));
    // Homes are looked up in the terminus's own inventory.
    EXPECT_TRUE(inventory.isFallbackAssociation(tid + 1, {"/inv/cpu0"},
                                                PLDM_ENTITY_SYS_BUS));
}

TEST_F(InventoryManagerTest, closestInventoryWalk)
{
    auto cpu =
        id(PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID, PLDM_ENTITY_PROC, 1);
    auto bus = id(10, PLDM_ENTITY_SYS_BUS, 1);
    auto leaf = id(20, PLDM_ENTITY_PCI_EXPRESS_BUS, 1);
    tree().add(10, cpu, {bus});
    tree().add(20, bus, {leaf});
    auto& data = inventory.terminusData[tid];
    inventory.bindInventory(data, leaf);
    const Paths cpuPaths{"/inv/cpu0", "/inv/cpu1"};
    tree().find(cpu)->bind(cpuPaths);
    EXPECT_EQ(cpuPaths, inventory.closestInventory(data, *tree().find(leaf)));
    EXPECT_TRUE(tree().find(bus)->inventory().empty());
    EXPECT_TRUE(tree().find(leaf)->inventory().empty());

    // A scope stands in for the ancestors' inventory.
    tree().find(bus)->setScope({"/inv/scope"});
    EXPECT_EQ(Paths{"/inv/scope"},
              inventory.closestInventory(data, *tree().find(leaf)));

    // An unbound parent stops the walk even if a higher ancestor has
    // inventory.
    tree().find(bus)->unbind();
    EXPECT_FALSE(tree().find(bus)->scope());
    EXPECT_EQ(Paths{systemPath},
              inventory.closestInventory(data, *tree().find(leaf)));

    // A fully bound, empty cycle also resolves to the platform default.
    tree().clear();
    tree().add(10, id(20, PLDM_ENTITY_SYS_BUS, 1), {});
    tree().add(20, id(10, PLDM_ENTITY_SYS_BUS, 2), {});
    inventory.bindAll(tid);
    EXPECT_EQ(Paths{systemPath},
              inventory.closestInventory(data, *tree().container(10)));
}
