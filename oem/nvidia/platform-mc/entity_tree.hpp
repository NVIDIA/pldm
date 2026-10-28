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
#pragma once

#include "entity.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <set>
#include <span>

namespace pldm::platform_mc::oem_nvidia
{

/**
 * @brief EntityTree
 *
 * The entities of one terminus and how they contain each other, built from
 * its Entity Association PDRs (DSP0248 §11, Table 95), plus the platform
 * inventory each entity owns.
 *
 * Two indexes are kept:
 *  - one Entity node per EntityId (the same entity may be listed as a child
 *    in one PDR and as the container entity in another; both refer to the
 *    same node);
 *  - one Container record per PDR containerID: the container entity plus the
 *    union of children from every PDR sharing that containerID (§11.3).
 *
 * Navigation follows the spec:
 *  - parent(e) is container(e.id.containerId): an entity's own containerID
 *    names its single physical parent (§9.5). ancestor() walks that chain.
 *  - Container::children may hold entities whose id.containerId != cid: in a
 *    logical containment PDR the children keep their physical containerID
 *    (§11.4).
 *
 * The tree stores inventory bindings but does not compute them; that needs
 * the D-Bus inventory scan. InventoryManager owns one tree per terminus,
 * feeds it from the terminus's PDRs and binds it against the scan.
 */
class EntityTree
{
  public:
    /** @brief One PDR containerID: its container entity and children. */
    struct Container
    {
        EntityId entity;
        std::set<EntityId> children;
    };

    /** @brief Result of adding a PDR / association. */
    enum class AddResult
    {
        added,             //!< new container created
        merged,            //!< children merged into an existing container
        malformed,         //!< not an Entity Association PDR, or shorter
                           //!< than Table 95 requires
        reservedContainer, //!< containerID is the SYSTEM value 0x0000, which
                           //!< no PDR may define (§9.5)
        containerConflict, //!< containerID already bound to a different entity
    };

    /** @brief Decode an Entity Association PDR (header included) and merge it.
     *         Bytes after the record are ignored.
     *
     *  @param[in] pdr - raw PDR bytes starting at pldm_pdr_hdr
     */
    AddResult addPdr(std::span<const uint8_t> pdr);

    /** @brief Merge a pre-decoded association. When the containerID is
     *         already known the children are unioned.
     *
     *  @param[in] containerId - PDR containerID
     *  @param[in] container - container Entity Identification Information
     *  @param[in] children - contained entities
     */
    AddResult add(pdr::ContainerID containerId, const EntityId& container,
                  std::span<const EntityId> children);

    /** @brief Convenience overload for brace-initialised children. */
    AddResult add(pdr::ContainerID containerId, const EntityId& container,
                  std::initializer_list<EntityId> children = {})
    {
        return add(
            containerId, container,
            std::span<const EntityId>(children.begin(), children.size()));
    }

    /** @brief Find-or-create the node for an entity. Used for entities that
     *         sensors reference but no association PDR lists. */
    Entity& emplace(const EntityId& id);

    /** @name Navigation (nullptr when unknown)
     *  @{ */
    Entity* find(const EntityId& id);
    const Entity* find(const EntityId& id) const;

    /** @brief The entity that defines @a containerId; nullptr for the system
     *         container or an unknown ID. */
    Entity* container(pdr::ContainerID containerId);
    const Entity* container(pdr::ContainerID containerId) const;

    /** @brief The children every PDR for @a containerId lists; nullptr for
     *         the system container or an unknown ID. */
    const std::set<EntityId>* children(pdr::ContainerID containerId) const;

    /** @brief The entity's physical parent (§9.5). */
    Entity* parent(const Entity& entity);
    const Entity* parent(const Entity& entity) const;

    /** @brief Nearest strict ancestor of @a baseType (bit 15 ignored),
     *         starting from the entity's parent. Stops at the system
     *         container or on a containerID cycle. The EntityId overload
     *         does not need the entity itself to be in the tree. */
    Entity* ancestor(const EntityId& id, pdr::EntityType baseType);
    const Entity* ancestor(const EntityId& id, pdr::EntityType baseType) const;
    Entity* ancestor(const Entity& entity, pdr::EntityType baseType)
    {
        return ancestor(entity.id, baseType);
    }
    const Entity* ancestor(const Entity& entity, pdr::EntityType baseType) const
    {
        return ancestor(entity.id, baseType);
    }

    /** @} */

    /** @name Inventory
     *  @{ */
    /** @brief Drop every inventory binding, keeping the topology. */
    void unbindAll();
    /** @} */

    /** @brief Forget everything (topology and bindings). */
    void clear()
    {
        containers.clear();
        nodes.clear();
    }

    /** @brief Number of distinct entity nodes. */
    size_t size() const
    {
        return nodes.size();
    }

    bool empty() const
    {
        return containers.empty() && nodes.empty();
    }

    /** @brief Iterate the Container records in containerID order. */
    auto begin() const
    {
        return containers.begin();
    }

    auto end() const
    {
        return containers.end();
    }

  private:
    /** @brief Record @a children under @a record and make sure every entity
     *         involved has a node. */
    void link(Container& record, std::span<const EntityId> children);

    std::map<pdr::ContainerID, Container> containers;
    std::map<EntityId, Entity> nodes;
};

} // namespace pldm::platform_mc::oem_nvidia
