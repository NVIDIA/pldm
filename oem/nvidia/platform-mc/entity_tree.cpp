/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2025 NVIDIA CORPORATION &
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
#include "entity_tree.hpp"

#include "libpldm/platform.h"

#include <endian.h>

#include <vector>

namespace pldm::platform_mc::oem_nvidia
{

/** @brief Host-endian EntityId from a wire-format (little-endian) PDR field. */
static EntityId fromWire(const pldm_entity& wire)
{
    pldm_entity host{};
    host.entity_type = le16toh(wire.entity_type);
    host.entity_instance_num = le16toh(wire.entity_instance_num);
    host.entity_container_id = le16toh(wire.entity_container_id);
    return EntityId{host};
}

EntityTree::AddResult EntityTree::addPdr(std::span<const uint8_t> pdr)
{
    // Table 95: commonHeader, containerID(2), associationType(1),
    // container entity(6), containedEntityCount(1), N * contained entity(6).
    constexpr size_t fixedSize =
        sizeof(pldm_pdr_hdr) + sizeof(pldm_pdr_entity_association) -
        sizeof(pldm_entity); // struct carries one flexible child
    if (pdr.size() < fixedSize ||
        reinterpret_cast<const pldm_pdr_hdr*>(pdr.data())->type !=
            PLDM_PDR_ENTITY_ASSOCIATION)
    {
        return AddResult::malformed;
    }
    auto body = reinterpret_cast<const pldm_pdr_entity_association*>(
        pdr.data() + sizeof(pldm_pdr_hdr));
    if (pdr.size() < fixedSize + body->num_children * sizeof(pldm_entity))
    {
        return AddResult::malformed;
    }

    std::vector<EntityId> children;
    children.reserve(body->num_children);
    for (uint8_t i = 0; i < body->num_children; ++i)
    {
        children.emplace_back(fromWire(body->children[i]));
    }
    return add(le16toh(body->container_id), fromWire(body->container),
               children);
}

EntityTree::AddResult EntityTree::add(pdr::ContainerID containerId,
                                      const EntityId& container,
                                      std::span<const EntityId> children)
{
    if (containerId == PLDM_PLATFORM_ENTITY_SYSTEM_CONTAINER_ID)
    {
        // §9.5: the overall system has no Entity Association PDR; 0x0000 is
        // only ever a *reference* to it. Accepting one here would make
        // container(0x0000) non-null.
        return AddResult::reservedContainer;
    }
    auto [itr, inserted] =
        containers.try_emplace(containerId, Container{container, {}});
    if (!inserted && itr->second.entity != container)
    {
        // §11.3: linked PDRs must describe the same container entity.
        return AddResult::containerConflict;
    }
    link(itr->second, children);
    return inserted ? AddResult::added : AddResult::merged;
}

void EntityTree::link(Container& record, std::span<const EntityId> children)
{
    emplace(record.entity);
    for (const auto& child : children)
    {
        record.children.insert(child);
        emplace(child);
    }
}

Entity& EntityTree::emplace(const EntityId& id)
{
    return nodes.try_emplace(id, id).first->second;
}

Entity* EntityTree::find(const EntityId& id)
{
    auto itr = nodes.find(id);
    return itr == nodes.end() ? nullptr : &itr->second;
}

const Entity* EntityTree::find(const EntityId& id) const
{
    auto itr = nodes.find(id);
    return itr == nodes.end() ? nullptr : &itr->second;
}

Entity* EntityTree::container(pdr::ContainerID containerId)
{
    auto itr = containers.find(containerId);
    return itr == containers.end() ? nullptr : find(itr->second.entity);
}

const Entity* EntityTree::container(pdr::ContainerID containerId) const
{
    auto itr = containers.find(containerId);
    return itr == containers.end() ? nullptr : find(itr->second.entity);
}

const std::set<EntityId>* EntityTree::children(
    pdr::ContainerID containerId) const
{
    auto itr = containers.find(containerId);
    return itr == containers.end() ? nullptr : &itr->second.children;
}

Entity* EntityTree::parent(const Entity& entity)
{
    return container(entity.id.containerId);
}

const Entity* EntityTree::parent(const Entity& entity) const
{
    return container(entity.id.containerId);
}

const Entity* EntityTree::ancestor(const EntityId& id,
                                   pdr::EntityType baseType) const
{
    baseType &= pdr::entityTypeMask;
    // Bound the walk by the number of containers so a malformed repository
    // with a containerID cycle cannot loop forever.
    const Entity* node = container(id.containerId);
    for (size_t hops = 0; node != nullptr && hops <= containers.size(); ++hops)
    {
        if (node->id.baseType() == baseType)
        {
            return node;
        }
        node = parent(*node);
    }
    return nullptr;
}

Entity* EntityTree::ancestor(const EntityId& id, pdr::EntityType baseType)
{
    return const_cast<Entity*>(
        static_cast<const EntityTree&>(*this).ancestor(id, baseType));
}

void EntityTree::unbindAll()
{
    for (auto& [id, node] : nodes)
    {
        node.unbind();
    }
}

} // namespace pldm::platform_mc::oem_nvidia
