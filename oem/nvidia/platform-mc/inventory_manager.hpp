/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "libpldm/entity.h"

#include "common/types.hpp"
#include "common/utils.hpp"
#include "entity_tree.hpp"

#include <exec/task.hpp>

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace pldm::platform_mc
{
class Terminus;
} // namespace pldm::platform_mc

namespace pldm::platform_mc::oem_nvidia
{

constexpr auto instanceInterface =
    "xyz.openbmc_project.Inventory.Decorator.Instance";
constexpr auto instanceProperty = "InstanceNumber";
constexpr auto overallSystemInterface =
    "xyz.openbmc_project.Inventory.Item.System";
constexpr auto chassisInterface = "xyz.openbmc_project.Inventory.Item.Chassis";
static const std::map<pdr::EntityType, std::string_view> entityInterfaces = {
    {PLDM_ENTITY_PHYSCIAL | PLDM_ENTITY_PROC_IO_MODULE,
     "xyz.openbmc_project.Inventory.Item.ProcessorModule"},
    {PLDM_ENTITY_LOGICAL | PLDM_ENTITY_PROC,
     "xyz.openbmc_project.Inventory.Item.Cpu"},
    {PLDM_ENTITY_PHYSCIAL | PLDM_ENTITY_ADD_IN_CARD,
     "xyz.openbmc_project.Inventory.Item.Board"}};

/** Entity topology and device inventory of every terminus, and the binding
 *  between the two.
 *
 *  A process-wide singleton reached through get(). Each terminus registers
 *  its configured instance and name, feeds its Entity Association PDRs and
 *  scans the inventory objects that belong to it; sensors and effecters are
 *  then associated with the inventory their entity binds to. EntityIds are
 *  only unique within a terminus, so all of it is kept per TID. */
class InventoryManager
{
  public:
    InventoryManager(const InventoryManager&) = delete;
    InventoryManager& operator=(const InventoryManager&) = delete;

    /** @brief The process-wide instance. */
    static InventoryManager& get()
    {
        static InventoryManager instance;
        return instance;
    }

    /** @brief Forget every terminus and the scanned system inventory
     *         (test setup). */
    void reset();

    /** @name Terminus registration
     *  @{ */
    /** @brief Configured instance the terminus's CPU and ProcessorModule
     *         entities match inventory with, in place of the PDR instance.
     *         Disengaged clears it. */
    void setInstance(tid_t tid, std::optional<uint16_t> instance);
    std::optional<uint16_t> instance(tid_t tid) const;
    /** @brief Configured terminus name. A MemoryController whose module has
     *         no inventory binds to the ProcessorModule named after it. */
    void setTerminusName(tid_t tid, const std::string& name);
    /** @brief Forget everything about a terminus: topology, inventory,
     *         instance and name. For a terminus re-created at a reused TID. */
    void forget(tid_t tid);
    /** @} */

    /** @name Entity topology
     *  @{ */
    /** @brief Decode an Entity Association PDR into the terminus's tree.
     *
     *  @param[in] tid - terminus the PDR belongs to
     *  @param[in] pdr - raw PDR bytes starting at pldm_pdr_hdr
     */
    EntityTree::AddResult addEntityAssociation(tid_t tid,
                                               std::span<const uint8_t> pdr);
    /** @brief The terminus's entity tree; empty when it reported no Entity
     *         Association PDRs. */
    EntityTree& entityTree(tid_t tid);
    /** @brief Drop the terminus's inventory bindings, keeping the topology. */
    void unbindAll(tid_t tid);
    /** @} */

    /** @name Device inventory
     *  @{ */
    using Inventory =
        std::tuple<dbus::ObjectPath, pdr::EntityType, pdr::EntityInstance>;
    using ParentMap = std::map<dbus::ObjectPath, dbus::ObjectPath>;

    /** @brief Re-scan the inventory objects of the terminus.
     *
     *  Every System, ProcessorModule, Cpu and Board object is offered to
     *  Terminus::checkDeviceInventory(), which loads the device's Entity
     *  Manager configuration; the ones it accepts, with their all_processors
     *  twins and parent_chassis, become the terminus's device inventory.
     *  The result is published only once the scan completes.
     */
    exec::task<int> scan(Terminus& terminus);

    /** @brief The platform's system inventory, the default parent of every
     *         sensor and effecter. */
    std::string systemInventoryPath = PLATFORM_CHASSIS_PATH;
    /** @} */

    /** @name Entity-to-inventory resolution
     *  @{ */
    /** @brief Re-bind every container entity and child of the terminus. */
    void bindAll(tid_t tid);

    /** @brief The inventory @a id associates with: its own, else the closest
     *         bound ancestor's, else the system inventory. Binds the entity
     *         and its unbound ancestors on demand, adding it to the tree when
     *         no association PDR lists it, because sensors may reference
     *         entities no PDR lists.
     *
     *  @param[in] tid - terminus the entity belongs to
     *  @param[in] id - the entity
     */
    std::vector<dbus::ObjectPath> findInventory(tid_t tid, const EntityId& id);

    /** @name CPU redirections
     *  Sensors on entities with no inventory of their own that belong to a
     *  CPU. Each returns the CPU's inventory as findInventory() does, or
     *  nothing when the topology has no such CPU.
     *  @{ */
    /** @brief The CPU that defines @a containerId. */
    std::vector<dbus::ObjectPath> containerProcessorInventory(
        tid_t tid, pdr::ContainerID containerId);
    /** @brief The CPU that defines @a containerId, or the first CPU in it
     *         when it is a ProcessorModule. */
    std::vector<dbus::ObjectPath> processorInventory(
        tid_t tid, pdr::ContainerID containerId);
    /** @brief The first CPU of the terminus's first ProcessorModule (in
     *         containerID order). */
    std::vector<dbus::ObjectPath> moduleProcessorInventory(tid_t tid);
    /** @} */

    /** @brief Whether @a inventoryPaths is a degraded placeholder for an
     *         entity of @a entityType that must resolve to dedicated
     *         inventory (an ancestor or the system chassis while Entity
     *         Manager is still settling). Callers gate metric-report updates
     *         on it so the first shared-memory insert cannot freeze a wrong
     *         metricProperty URI. */
    bool isFallbackAssociation(tid_t tid,
                               const std::vector<std::string>& inventoryPaths,
                               pdr::EntityType entityType) const;
    /** @} */

  private:
    /** @brief What InventoryManager keeps for one terminus. */
    struct TerminusData
    {
        EntityTree entities;
        std::optional<uint16_t> instance;
        std::string name;
        /** @brief Inventory objects the last scan matched to the terminus. */
        std::vector<Inventory> inventories;
        ParentMap inventoryParents;
    };

    InventoryManager() = default;

    /** @brief Bind an entity and its ancestors to the inventory snapshot. */
    Entity& bindInventory(TerminusData& data, const EntityId& id);

    /** @brief First own inventory (or scope) up the bound parent chain,
     *         otherwise the platform's system inventory. */
    std::vector<dbus::ObjectPath> closestInventory(const TerminusData& data,
                                                   const Entity& entity) const;

    /** @brief The entity and its unbound ancestors, nearest first, up to the
     *         first bound ancestor, the system or an unknown container. */
    static std::vector<Entity*> unboundChain(EntityTree& entityTree,
                                             Entity& entity);

    /** @brief Bind one entity whose ancestors are already bound. */
    void bindOne(TerminusData& data, Entity& entity);

    /** @brief Closest inventory of the entity that defines @a containerId;
     *         the system inventory for the system or an unknown container. */
    std::vector<dbus::ObjectPath> containerInventory(
        const TerminusData& data, pdr::ContainerID containerId) const;

    /** @brief Inventory an entity with no own inventory associates with in
     *         place of its parent's closest inventory, if its type has such
     *         an override: a MemoryController belongs to the ProcessorModule
     *         above its CPU, a power supply (VREG) to the CPU it sits next
     *         to. */
    std::optional<std::vector<dbus::ObjectPath>> scopeOverride(
        TerminusData& data, const Entity& entity);

    /** @brief A MemoryController's scope: the inventory above the CPU it
     *         sits under, else the ProcessorModule named after the
     *         terminus. None when its parent is the module itself. */
    std::optional<std::vector<dbus::ObjectPath>> moduleScope(
        const TerminusData& data, const Entity& parent) const;

    /** @brief A power supply's scope: the inventory of the first CPU in its
     *         container. */
    std::optional<std::vector<dbus::ObjectPath>> processorScope(
        TerminusData& data, pdr::ContainerID containerId);

    /** @brief Closest inventory of the first CPU among the children listed
     *         for @a containerId; nothing when there is none. */
    std::vector<dbus::ObjectPath> firstProcessorInventory(
        TerminusData& data, pdr::ContainerID containerId);

    /** @brief Instance number to match against inventory. CPU and
     *         ProcessorModule entities take the configured instance when
     *         present, else the owning ProcessorModule's instance from the
     *         hierarchy (a single terminus may report both CPUs with the
     *         same local instance), else the entity's own. */
    static pdr::EntityInstance resolvedInstance(const TerminusData& data,
                                                const Entity& entity);

    /** @brief Inventory objects of @a baseType / @a wantedInstance. */
    static std::vector<dbus::ObjectPath> matchingInventory(
        const TerminusData& data, pdr::EntityType baseType,
        pdr::EntityInstance wantedInstance);

    /** @brief Pick the entity's own inventory from @a candidates: all of them
     *         for CPU/ProcessorModule, else the ones under the parent's
     *         inventory, else the first. */
    static std::vector<dbus::ObjectPath> selectInventory(
        const TerminusData& data, pdr::EntityType baseType,
        const std::vector<dbus::ObjectPath>& candidates,
        const std::vector<dbus::ObjectPath>& parentPaths);

    static exec::task<int> getInventoryParent(const std::string objPath,
                                              ParentMap& parents);
    static exec::task<void> collectInterface(
        Inventory& inventory, const std::string& service,
        const dbus::Interfaces& interfaces, const std::string& interface,
        std::string& systemPath);
    static exec::task<void> collectProcessors(
        const Inventory& inventory, std::vector<Inventory>& inventories,
        ParentMap& parents);

    std::map<tid_t, TerminusData> terminusData;
};

} // namespace pldm::platform_mc::oem_nvidia
