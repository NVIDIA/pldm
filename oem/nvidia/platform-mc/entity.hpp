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

#include "libpldm/pdr.h"

#include "common/types.hpp"

#include <compare>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace pldm::platform_mc::oem_nvidia
{

/**
 * @brief EntityId
 *
 * DSP0248 "Entity Identification Information" (§9.1): the triple that
 * uniquely names an entity within a terminus. Field order follows the spec
 * and libpldm's pldm_entity. Constructed only from spec/wire representations
 * so the field order can never be mixed up at a call site.
 */
struct EntityId
{
    pdr::EntityType type;         //!< as reported; bit 15 set = logical
    pdr::EntityInstance instance; //!< numbered relative to containerId
    pdr::ContainerID containerId; //!< §9.4: the entity's (physical) parent

    /** @brief From the legacy tuple (containerId, type, instance) used by
     *         sensors, StateSetInfo and host-bmc. */
    constexpr EntityId(const pdr::EntityInfo& info) :
        type(std::get<1>(info)), instance(std::get<2>(info)),
        containerId(std::get<0>(info))
    {}

    /** @brief From a host-endian libpldm entity, as libpldm's decode and
     *         tree APIs return. A pldm_entity read straight from a PDR buffer
     *         is wire (little-endian) format and must be converted first. */
    constexpr EntityId(const pldm_entity& e) :
        type(e.entity_type), instance(e.entity_instance_num),
        containerId(e.entity_container_id)
    {}

    /** @brief Entity type with the physical/logical bit stripped. */
    constexpr pdr::EntityType baseType() const
    {
        return type & pdr::entityTypeMask;
    }

    constexpr bool isLogical() const
    {
        return (type & ~pdr::entityTypeMask) != 0;
    }

    /** @brief Back to the legacy tuple for APIs that still take it. */
    constexpr pdr::EntityInfo toEntityInfo() const
    {
        return {containerId, type, instance};
    }

    /** @brief Ordered as the legacy tuple (containerId, type, instance),
     *         so a sorted set of entities visits them container by
     *         container as before. */
    constexpr auto operator<=>(const EntityId& other) const
    {
        return toEntityInfo() <=> other.toEntityInfo();
    }
    constexpr bool operator==(const EntityId&) const = default;
};

/**
 * @brief Entity
 *
 * PDR identity plus this entity's own platform inventory. A disengaged
 * binding is unresolved; an engaged but empty binding means the platform
 * has no object for this entity (e.g. a SYS_BUS). An entity with no object
 * of its own may carry a scope: the inventory it and its descendants
 * associate with instead of the closest ancestor's.
 */
class Entity
{
  public:
    explicit Entity(const EntityId& id) : id(id) {}

    const EntityId id;

    /** @brief Whether bind() has run since the last unbind(). */
    bool isBound() const
    {
        return inventoryPaths.has_value();
    }

    void bind(std::vector<dbus::ObjectPath> own)
    {
        inventoryPaths = std::move(own);
        scopePaths.reset();
    }

    void unbind()
    {
        inventoryPaths.reset();
        scopePaths.reset();
    }

    /** @brief Set the inventory that stands in for the closest ancestor's;
     *         cleared by bind() and unbind(). */
    void setScope(std::vector<dbus::ObjectPath> paths)
    {
        scopePaths = std::move(paths);
    }

    const std::optional<std::vector<dbus::ObjectPath>>& scope() const
    {
        return scopePaths;
    }

    /** @brief Inventory objects that represent this entity. */
    const std::vector<dbus::ObjectPath>& inventory() const
    {
        return inventoryPaths ? *inventoryPaths : none;
    }

  private:
    static inline const std::vector<dbus::ObjectPath> none;
    std::optional<std::vector<dbus::ObjectPath>> inventoryPaths;
    std::optional<std::vector<dbus::ObjectPath>> scopePaths;
};

} // namespace pldm::platform_mc::oem_nvidia
