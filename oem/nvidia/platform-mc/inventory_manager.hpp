/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "common/types.hpp"
#include "entity_tree.hpp"

#include <map>
#include <optional>
#include <span>
#include <string>

namespace pldm::platform_mc::oem_nvidia
{

/** Entity topology of every terminus and what each terminus registered
 *  about itself.
 *
 *  A process-wide singleton reached through get(). Each terminus registers
 *  its configured instance and name and feeds its Entity Association PDRs.
 *  EntityIds are only unique within a terminus, so all of it is kept per
 *  TID. */
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

    /** @brief Forget every terminus (test setup). */
    void reset();

    /** @name Terminus registration
     *  @{ */
    /** @brief Configured instance the terminus's CPU and ProcessorModule
     *         entities match inventory with, in place of the PDR instance.
     *         Disengaged clears it. */
    void setInstance(tid_t tid, std::optional<uint16_t> instance);
    std::optional<uint16_t> instance(tid_t tid) const;
    /** @brief Configured terminus name. */
    void setTerminusName(tid_t tid, const std::string& name);
    /** @brief Forget everything about a terminus: topology, instance and
     *         name. For a terminus re-created at a reused TID. */
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

  private:
    /** @brief What InventoryManager keeps for one terminus. */
    struct TerminusData
    {
        EntityTree entities;
        std::optional<uint16_t> instance;
        std::string name;
    };

    InventoryManager() = default;

    std::map<tid_t, TerminusData> terminusData;
};

} // namespace pldm::platform_mc::oem_nvidia
