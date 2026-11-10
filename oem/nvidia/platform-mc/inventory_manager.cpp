/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "inventory_manager.hpp"

#include "libpldm/platform.h"

#include "common/dBusAsyncUtils.hpp"
#include "platform-mc/terminus.hpp"

#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <filesystem>

namespace pldm::platform_mc::oem_nvidia
{

using pdr::baseEntityType;
using pdr::EntityInstance;
using pdr::EntityType;

PhysicalContextType toPhysicalContextType(const EntityType entityType)
{
    static const std::map<EntityType, PhysicalContextType> contexts = {
        {PLDM_ENTITY_MEMORY_CONTROLLER, PhysicalContextType::Memory},
        {PLDM_ENTITY_PROC, PhysicalContextType::CPU},
        {PLDM_ENTITY_PROC_MODULE, PhysicalContextType::CPU},
        {PLDM_ENTITY_PROC_IO_MODULE, PhysicalContextType::CPU},
        {PLDM_ENTITY_DC_DC_CONVERTER, PhysicalContextType::VoltageRegulator},
        {PLDM_ENTITY_POWER_CONVERTER, PhysicalContextType::VoltageRegulator},
        {PLDM_ENTITY_POWER_SUPPLY, PhysicalContextType::VoltageRegulator},
        {PLDM_ENTITY_NETWORK_CONTROLLER, PhysicalContextType::NetworkingDevice},
    };
    std::map<EntityType, PhysicalContextType>::const_iterator itr =
        contexts.find(baseEntityType(entityType));
    return itr == contexts.end() ? PhysicalContextType::SystemBoard
                                 : itr->second;
}

const dbus::Interfaces& InventoryManager::interestingInterfaces()
{
    static const dbus::Interfaces interfaces = [] {
        dbus::Interfaces result{overallSystemInterface};
        for (const auto& [type, interface] : entityInterfaces)
        {
            result.emplace_back(interface);
        }
        return result;
    }();
    return interfaces;
}

static bool isProcessorType(EntityType baseType)
{
    return baseType == PLDM_ENTITY_PROC ||
           baseType == PLDM_ENTITY_PROC_IO_MODULE;
}

static bool isLinkedProcessor(const std::string& reverse,
                              const std::string& endpoint,
                              const std::string& path)
{
    return reverse == "all_processors" && endpoint != path;
}

void InventoryManager::reset()
{
    terminusData.clear();
    systemInventoryPath.clear();
}

std::vector<dbus::ObjectPath> InventoryManager::systemInventory() const
{
    if (systemInventoryPath.empty())
    {
        return {};
    }
    return {systemInventoryPath};
}

bool InventoryManager::updateSystemInventory(tid_t tid, std::string path)
{
    if (!path.empty() && path != systemInventoryPath)
    {
        lg2::info("scanInventories: TID {TID} found system inventory {PATH}",
                  "TID", tid, "PATH", path);
        systemInventoryPath = std::move(path);
        return true;
    }
    if (systemInventoryPath.empty())
    {
        lg2::error("scanInventories: TID {TID} found no TopChassis in the "
                   "platform topology; sensors and effecters without a "
                   "closer inventory stay unassociated",
                   "TID", tid);
    }
    return false;
}

void InventoryManager::setInstance(tid_t tid, std::optional<uint16_t> instance)
{
    terminusData[tid].instance = instance;
}

std::optional<uint16_t> InventoryManager::instance(tid_t tid) const
{
    auto itr = terminusData.find(tid);
    if (itr == terminusData.end())
    {
        return std::nullopt;
    }
    return itr->second.instance;
}

void InventoryManager::setTerminusName(tid_t tid, const std::string& name)
{
    terminusData[tid].name = name;
}

void InventoryManager::forget(tid_t tid)
{
    terminusData.erase(tid);
}

EntityTree::AddResult InventoryManager::addEntityAssociation(
    tid_t tid, std::span<const uint8_t> pdr)
{
    return terminusData[tid].entities.addPdr(pdr);
}

EntityTree& InventoryManager::entityTree(tid_t tid)
{
    return terminusData[tid].entities;
}

void InventoryManager::unbindAll(tid_t tid)
{
    auto itr = terminusData.find(tid);
    if (itr != terminusData.end())
    {
        itr->second.entities.unbindAll();
    }
}

void InventoryManager::clearTopology(tid_t tid)
{
    auto itr = terminusData.find(tid);
    if (itr != terminusData.end())
    {
        itr->second.entities.clear();
    }
}

exec::task<int> InventoryManager::getInventoryParent(const std::string objPath,
                                                     ParentMap& parents)
{
    const std::string assocPath = objPath + "/parent_chassis";
    const dbus::Interfaces assocIfaces = {"xyz.openbmc_project.Association"};

    // Association objects are owned by entity-manager or inventory services,
    // not the mapper; ask the mapper which service it is.
    auto serviceMap = co_await utils::coGetServiceMap(assocPath, assocIfaces);
    if (serviceMap.empty())
    {
        co_return PLDM_SUCCESS;
    }

    const auto& service = serviceMap.begin()->first;
    auto endpoints =
        co_await utils::coGetDbusProperty<std::vector<std::string>>(
            assocPath, "endpoints", "xyz.openbmc_project.Association", service);
    if (!endpoints.empty())
    {
        parents[objPath] = endpoints[0];
    }
    co_return PLDM_SUCCESS;
}

exec::task<void> InventoryManager::collectInterface(
    Inventory& inventory, const std::string& service,
    const std::string& interface)
{
    auto& [path, type, instance] = inventory;
    if (interface == instanceInterface)
    {
        instance = co_await utils::coGetDbusProperty<uint64_t>(
            path, instanceProperty, instanceInterface, service);
        co_return;
    }
    auto entity = std::find_if(
        entityInterfaces.begin(), entityInterfaces.end(),
        [&interface](const auto& entry) { return interface == entry.second; });
    if (entity != entityInterfaces.end())
    {
        type = entity->first;
    }
}

exec::task<void> InventoryManager::collectProcessors(
    const Inventory& inventory, std::vector<Inventory>& inventories,
    ParentMap& parents)
{
    const auto& [path, type, instance] = inventory;
    try
    {
        auto services = co_await utils::coGetServiceMap(
            path, {"xyz.openbmc_project.Association.Definitions"});
        if (services.empty())
        {
            co_return;
        }
        const auto associations = co_await utils::coGetDbusProperty<
            std::vector<std::tuple<std::string, std::string, std::string>>>(
            path, "Associations", "xyz.openbmc_project.Association.Definitions",
            services.begin()->first);
        for (const auto& [forward, reverse, endpoint] : associations)
        {
            if (!isLinkedProcessor(reverse, endpoint, path))
            {
                continue;
            }
            inventories.emplace_back(endpoint, type, instance);
            co_await getInventoryParent(endpoint, parents);
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("failed to query chassis cpu: {P} Error: {ERR}", "P", path,
                   "ERR", e);
    }
}

exec::task<int> InventoryManager::scan(Terminus& terminus)
{
    const auto tid = terminus.getTid();
    std::vector<Inventory> newInventories;
    ParentMap newInventoryParents;
    try
    {
        auto subtree = co_await utils::coGetSubTree(
            "/xyz/openbmc_project/inventory", 0, interestingInterfaces());
        for (const auto& [path, services] : subtree)
        {
            Inventory inventory{path, 0, 0xFFFF};
            for (const auto& [service, interfaces] : services)
            {
                for (const auto& interface : interfaces)
                {
                    co_await collectInterface(inventory, service, interface);
                }
            }
            if (co_await terminus.checkDeviceInventory(path) != PLDM_SUCCESS)
            {
                continue;
            }
            newInventories.push_back(inventory);
            co_await getInventoryParent(path, newInventoryParents);
            const EntityType baseType = baseEntityType(std::get<1>(inventory));
            if (isProcessorType(baseType))
            {
                co_await collectProcessors(inventory, newInventories,
                                           newInventoryParents);
            }
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to scan inventories Error: {ERROR}", "ERROR", e);
        co_return PLDM_FAILED;
    }
    // Publish the complete snapshot without suspending.
    auto& data = terminusData[tid];
    data.inventories.swap(newInventories);
    data.inventoryParents.swap(newInventoryParents);
    lg2::debug(
        "scanInventories: completed for TID {TID}; {COUNT} device inventories matched",
        "TID", tid, "COUNT", data.inventories.size());
    co_return PLDM_SUCCESS;
}

void InventoryManager::bindAll(tid_t tid)
{
    auto& data = terminusData[tid];
    data.entities.unbindAll();
    for (const auto& [containerId, container] : data.entities)
    {
        bindInventory(data, container.entity);
        for (const auto& child : container.children)
        {
            bindInventory(data, child);
        }
    }
}

std::vector<dbus::ObjectPath> InventoryManager::findInventory(
    tid_t tid, const EntityId& id)
{
    // Sensors may reference entities no association PDR lists, so the entity
    // is emplaced into the tree on demand.
    auto& data = terminusData[tid];
    const auto& entity = bindInventory(data, id);
    return closestInventory(data, entity);
}

std::vector<dbus::ObjectPath> InventoryManager::containerProcessorInventory(
    tid_t tid, pdr::ContainerID containerId)
{
    auto& data = terminusData[tid];
    const auto* container = data.entities.container(containerId);
    if (!container || container->id.baseType() != PLDM_ENTITY_PROC)
    {
        return {};
    }
    return closestInventory(data, bindInventory(data, container->id));
}

std::vector<dbus::ObjectPath> InventoryManager::processorInventory(
    tid_t tid, pdr::ContainerID containerId)
{
    auto& data = terminusData[tid];
    const auto* container = data.entities.container(containerId);
    if (!container)
    {
        return {};
    }
    if (container->id.baseType() == PLDM_ENTITY_PROC)
    {
        return closestInventory(data, bindInventory(data, container->id));
    }
    if (container->id.baseType() != PLDM_ENTITY_PROC_IO_MODULE)
    {
        return {};
    }
    return firstProcessorInventory(data, containerId);
}

std::vector<dbus::ObjectPath> InventoryManager::moduleProcessorInventory(
    tid_t tid)
{
    auto& data = terminusData[tid];
    for (const auto& [containerId, container] : data.entities)
    {
        if (container.entity.baseType() == PLDM_ENTITY_PROC_IO_MODULE)
        {
            return firstProcessorInventory(data, containerId);
        }
    }
    return {};
}

std::vector<dbus::ObjectPath> InventoryManager::firstProcessorInventory(
    TerminusData& data, pdr::ContainerID containerId)
{
    const auto* children = data.entities.children(containerId);
    if (!children)
    {
        return {};
    }
    for (const auto& child : *children)
    {
        if (child.baseType() == PLDM_ENTITY_PROC)
        {
            return closestInventory(data, bindInventory(data, child));
        }
    }
    return {};
}

Entity& InventoryManager::bindInventory(TerminusData& data, const EntityId& id)
{
    // Bind top-down from the first bound ancestor (or the system / an unknown
    // container) so every entity sees its parent's inventory.
    auto& entity = data.entities.emplace(id);
    auto chain = unboundChain(data.entities, entity);
    for (auto itr = chain.rbegin(); itr != chain.rend(); ++itr)
    {
        bindOne(data, **itr);
    }
    return entity;
}

std::vector<Entity*> InventoryManager::unboundChain(EntityTree& entityTree,
                                                    Entity& entity)
{
    // A containerID cycle in a malformed repository ends the chain at the
    // repeated entity.
    std::vector<Entity*> chain;
    for (Entity* node = &entity;
         node && !node->isBound() &&
         std::find(chain.begin(), chain.end(), node) == chain.end();
         node = entityTree.parent(*node))
    {
        chain.push_back(node);
    }
    return chain;
}

void InventoryManager::bindOne(TerminusData& data, Entity& entity)
{
    const auto baseType = entity.id.baseType();
    auto own = selectInventory(
        data, baseType,
        matchingInventory(data, baseType, resolvedInstance(data, entity)),
        containerInventory(data, entity.id.containerId));
    const bool hasOwn = !own.empty();
    // Bound before the override is looked up, so that a malformed tree
    // leading back to this entity cannot bind it again.
    entity.bind(std::move(own));
    if (hasOwn)
    {
        return;
    }
    if (auto scope = scopeOverride(data, entity))
    {
        entity.setScope(std::move(*scope));
    }
}

std::vector<dbus::ObjectPath> InventoryManager::containerInventory(
    const TerminusData& data, pdr::ContainerID containerId) const
{
    if (containerId == PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID)
    {
        return systemInventory();
    }
    const auto* container = data.entities.container(containerId);
    if (!container)
    {
        lg2::error(
            "cannot find containerId:{CID}, returning systemInventoryPath",
            "CID", containerId);
        return systemInventory();
    }
    return closestInventory(data, *container);
}

std::optional<std::vector<dbus::ObjectPath>> InventoryManager::scopeOverride(
    TerminusData& data, const Entity& entity)
{
    const auto* parent = data.entities.parent(entity);
    if (!parent)
    {
        return std::nullopt;
    }
    switch (entity.id.baseType())
    {
        case PLDM_ENTITY_MEMORY_CONTROLLER:
            return moduleScope(data, *parent);
        case PLDM_ENTITY_POWER_SUPPLY:
            return processorScope(data, entity.id.containerId);
        default:
            return std::nullopt;
    }
}

std::optional<std::vector<dbus::ObjectPath>> InventoryManager::moduleScope(
    const TerminusData& data, const Entity& parent) const
{
    // DRAM sits under the CPU but belongs to the ProcessorModule: walk up
    // from the CPU unless the parent already is the module.
    if (parent.id.baseType() == PLDM_ENTITY_PROC_IO_MODULE)
    {
        return std::nullopt;
    }
    auto modulePaths = containerInventory(data, parent.id.containerId);
    if (modulePaths != systemInventory() || data.name.empty())
    {
        return modulePaths;
    }
    // No module above the CPU: take the ProcessorModule named after the
    // terminus.
    for (const auto& [path, type, instance] : data.inventories)
    {
        if (baseEntityType(type) == PLDM_ENTITY_PROC_IO_MODULE &&
            std::filesystem::path(path).filename().string() == data.name)
        {
            return std::vector<dbus::ObjectPath>{path};
        }
    }
    return modulePaths;
}

std::optional<std::vector<dbus::ObjectPath>> InventoryManager::processorScope(
    TerminusData& data, pdr::ContainerID containerId)
{
    // A VREG sits under the ProcessorModule but powers the CPU next to it.
    auto paths = firstProcessorInventory(data, containerId);
    if (paths.empty())
    {
        return std::nullopt;
    }
    return paths;
}

std::vector<dbus::ObjectPath> InventoryManager::closestInventory(
    const TerminusData& data, const Entity& entity) const
{
    // Bound the walk by the node count so containerID cycles terminate.
    const auto& entityTree = data.entities;
    const Entity* node = &entity;
    for (size_t hops = 0; node && node->isBound() && hops <= entityTree.size();
         ++hops, node = entityTree.parent(*node))
    {
        if (!node->inventory().empty())
        {
            return node->inventory();
        }
        if (node->scope())
        {
            return *node->scope();
        }
    }
    return systemInventory();
}

EntityInstance InventoryManager::resolvedInstance(const TerminusData& data,
                                                  const Entity& entity)
{
    if (!isProcessorType(entity.id.baseType()))
    {
        return entity.id.instance;
    }
    if (data.instance)
    {
        return *data.instance;
    }
    // The ProcessorModule instance tells CPU_0 from CPU_1 even when a single
    // terminus reports both CPUs: it is the container, or the container of
    // the CPU the entity sits under.
    const auto* container = data.entities.container(entity.id.containerId);
    if (!container)
    {
        return entity.id.instance;
    }
    if (container->id.baseType() == PLDM_ENTITY_PROC_IO_MODULE)
    {
        return container->id.instance;
    }
    if (container->id.baseType() == PLDM_ENTITY_PROC)
    {
        const auto* module = data.entities.container(container->id.containerId);
        if (module && module->id.baseType() == PLDM_ENTITY_PROC_IO_MODULE)
        {
            return module->id.instance;
        }
    }
    return entity.id.instance;
}

std::vector<dbus::ObjectPath> InventoryManager::matchingInventory(
    const TerminusData& data, EntityType baseType,
    EntityInstance wantedInstance)
{
    std::vector<dbus::ObjectPath> candidates;
    for (const auto& [path, type, inst] : data.inventories)
    {
        // Inventory may use raw type where PDRs use LOGICAL|type, or vice
        // versa; compare with bit 15 stripped.
        if (baseEntityType(type) == baseType && inst == wantedInstance)
        {
            candidates.push_back(path);
        }
    }
    return candidates;
}

std::vector<dbus::ObjectPath> InventoryManager::selectInventory(
    const TerminusData& data, EntityType baseType,
    const std::vector<dbus::ObjectPath>& candidates,
    const std::vector<dbus::ObjectPath>& parentPaths)
{
    if (candidates.empty())
    {
        return {};
    }
    if (isProcessorType(baseType))
    {
        // CPU and ProcessorModule inventory may be linked by all_processors
        // associations; keep every match.
        return candidates;
    }
    std::vector<dbus::ObjectPath> underParent;
    for (const auto& candidate : candidates)
    {
        auto itr = data.inventoryParents.find(candidate);
        if (itr != data.inventoryParents.end() &&
            std::find(parentPaths.begin(), parentPaths.end(), itr->second) !=
                parentPaths.end())
        {
            underParent.push_back(candidate);
        }
    }
    if (underParent.empty())
    {
        underParent.push_back(candidates.front());
    }
    return underParent;
}

bool InventoryManager::isFallbackAssociation(
    tid_t tid, const std::vector<std::string>& inventoryPaths,
    EntityType entityType) const
{
    // Entity types that must resolve to dedicated inventory, mapped to the
    // entity types of the inventory objects that can serve as their Redfish
    // home. findInventory() degrades to the closest ancestor (module, board,
    // system chassis) while EM inventory is still settling; any resolution
    // that is not one of the home types below is a placeholder and
    // metric-report updates must stay gated so the first shmem insert cannot
    // freeze a wrong metricProperty URI.
    // Cross-reference: Terminus::buildEntityTypeTag() names these same
    // terminus-local sub-entities; entityInterfaces lists the inventory
    // interfaces scan() collects. Keep the three in sync.
    // Types absent from this map (e.g. baseboard sensors, and — pending
    // platform validation — ETHERNET/INFINIBAND/ADD_IN_CARD/OPERATING_SYS)
    // may legitimately live on the system chassis and are never gated.
    static const std::map<EntityType, std::vector<EntityType>> dedicatedHomes =
        {
            {PLDM_ENTITY_PROC, {PLDM_ENTITY_PROC}},
            {PLDM_ENTITY_PROC_IO_MODULE,
             {PLDM_ENTITY_PROC, PLDM_ENTITY_PROC_IO_MODULE}},
            {PLDM_ENTITY_MEMORY_CONTROLLER,
             {PLDM_ENTITY_PROC_IO_MODULE, PLDM_ENTITY_PROC}},
            {PLDM_ENTITY_POWER_SUPPLY, {PLDM_ENTITY_PROC}},
            {PLDM_ENTITY_SYS_BUS, {PLDM_ENTITY_PROC}},
            {PLDM_ENTITY_PCI_EXPRESS_BUS, {PLDM_ENTITY_PROC}},
        };

    // Strip LOGICAL flag for comparison, matching matchingInventory()
    std::map<EntityType, std::vector<EntityType>>::const_iterator itr =
        dedicatedHomes.find(baseEntityType(entityType));
    if (itr == dedicatedHomes.end())
    {
        return false;
    }
    if (inventoryPaths.empty())
    {
        return true;
    }
    static const std::vector<Inventory> none;
    auto data = terminusData.find(tid);
    const auto& inventories =
        data == terminusData.end() ? none : data->second.inventories;
    const auto& homeTypes = itr->second;
    auto isHome = [&](const std::string& path) {
        return std::any_of(
            inventories.begin(), inventories.end(), [&](const auto& inv) {
                const auto& [invPath, invType, invInstance] = inv;
                return invPath == path &&
                       std::find(homeTypes.begin(), homeTypes.end(),
                                 baseEntityType(invType)) != homeTypes.end();
            });
    };
    return !std::all_of(inventoryPaths.begin(), inventoryPaths.end(), isHome);
}

} // namespace pldm::platform_mc::oem_nvidia
