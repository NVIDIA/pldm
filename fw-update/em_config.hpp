/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
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

#include "common/types.hpp"

#include <optional>
#include <string>

/** @brief Read operations on the entity-manager-published PLDM firmware
 *         update configuration (Configuration.PLDMFirmwareDevice).
 *
 *  Kept separate from the firmware update Manager, which orchestrates the
 *  high-level update flow and consumes the data returned from here.
 */
namespace pldm::fw_update::em_config
{

/** @brief Resolve a device's friendly Name (the MCTPTargetName join key)
 *         for a discovered endpoint.
 *
 *  Identity is resolved via configured_by ONLY: the configurations map is
 *  keyed by the entity-manager config path that the endpoint's configured_by
 *  association resolves to; the stored name is the transport object's
 *  `.Name`, which equals the MCTPTargetName cited by every PLDM-* entry for
 *  the same device. This is the canonical, transport-agnostic key.
 *
 *  The EID is never used as an identity key. If configured_by is absent the
 *  name is empty and the endpoint is treated as not (yet) resolvable.
 *
 *  @param[in] configurations - configured_by-resolved EM config entries
 *  @param[in] mctpEid - MCTP endpoint
 *  @return the device's target Name, empty if not resolved
 */
std::string targetNameForEid(const Configurations& configurations,
                             pldm::eid mctpEid);

/** @brief Per-device component metadata read from the entity-manager
 *         Configuration.PLDMFirmwareDevice.Components array.
 */
struct DeviceComponentInfo
{
    /** @brief id → {Name, Associations, Manufacturer, UpdateOnly} */
    CreateComponentIdNameMap emComponents;
    /** @brief id → Name (for target filtering / name fallback) */
    ComponentIdNameMap idNameMap;
};

/** @brief Fetch per-component naming/Associations/Manufacturer from the
 *         entity-manager Configuration.PLDMFirmwareDevice.Components array.
 *
 *  Resolves the device's target Name via targetNameForEid (configured_by
 *  only), runs a global ObjectMapper GetSubTree for
 *  Configuration.PLDMFirmwareDevice, keeps the entry whose MCTPTargetName
 *  equals the target Name, and unpacks the nested Components array
 *  (published by entity-manager as child interfaces) into a
 *  ComponentIdentifier-keyed map.
 *
 *  @param[in] configurations - configured_by-resolved EM config entries
 *  @param[in] mctpEid - MCTP endpoint
 *  @return the unpacked component metadata, std::nullopt when the device has
 *          no resolved name or no matching PLDMFirmwareDevice entry
 */
std::optional<DeviceComponentInfo> fetchComponentInfo(
    const Configurations& configurations, pldm::eid mctpEid);

/** @brief entity-manager interface carrying the firmware-update opt-out.
 *
 *  Exposed so callers needing the raw property name (e.g. to build a canned
 *  D-Bus response in a test) do not have to duplicate it.
 */
constexpr auto pldmExclusionIntf =
    "xyz.openbmc_project.Configuration.PLDMExclusion";

/** @brief The flat inventory-path array published on @ref pldmExclusionIntf. */
constexpr auto excludedInventoryProp = "ExcludedInventory";

/** @brief Read the inventory paths currently excluded from PLDM T5 firmware
 *         update, unioned across every publishing entity-manager object.
 *
 *  entity-manager publishes the opt-out as a Configuration.PLDMExclusion
 *  object carrying a flat `ExcludedInventory` array of inventory object
 *  paths (a flat array survives PlatformExposes flattening, where a nested
 *  object array would not). Every object publishing the interface
 *  contributes, so a platform may split the list across several
 *  entity-manager configuration fragments.
 *
 *  Stateless: each call re-queries ObjectMapper rather than caching
 *  internally, which keeps this unit testable in isolation. The caller (see
 *  Manager::handleMctpEndpoints()) is the one that caches, invoking this at
 *  most once - entity-manager's configuration is assumed to already be on
 *  the bus by the first discovery batch, so there is no late-arriving case
 *  to track incrementally, and nothing a later call could learn that the
 *  first one didn't.
 *
 *  Never throws: an absent configuration, an unreadable object, or an
 *  unusable array element yields (or contributes) nothing.
 *
 *  @return the effective excluded-inventory set; empty when nothing is
 *          excluded
 */
ExcludedInventoryPaths fetchExcludedInventory();

/** @brief D-Bus interface publishing the configured_by/configures
 *         associations that identify a device to firmware update.
 *
 *  Published by mctpreactor directly on the MCTP endpoint object
 *  (`.../networks/<n>/endpoints/<eid>`), on its own D-Bus service - not by
 *  entity-manager, and not the same connection that owns the endpoint object
 *  itself.
 */
constexpr auto associationDefinitionsIntf =
    "xyz.openbmc_project.Association.Definitions";

/** @brief Read the configured_by association target directly off one MCTP
 *         endpoint's own Association.Definitions.
 *
 *  mctpreactor publishes the endpoint's identity as a forward "configured_by"
 *  association naming the entity-manager inventory object that configures
 *  it (e.g.
 *  "/xyz/openbmc_project/inventory/system/platform/.../IO_Board_SMA_2"),
 *  independent of any interface that object happens to carry. This is the
 *  exact string PLDMExclusion's ExcludedInventory entries are meant to name.
 *
 *  Never throws: an endpoint with no configured_by association yet (or
 *  ever, if mctpreactor does not name it), or an unreadable property,
 *  yields nullopt.
 *
 *  @param[in] mctpEid - MCTP endpoint
 *  @param[in] networkId - the endpoint's MCTP network index
 *  @return the configured_by target path, nullopt if unresolved
 */
std::optional<dbus::ObjectPath> fetchConfiguredByPath(pldm::eid mctpEid,
                                                      NetworkId networkId);

/** @brief Whether an MCTP endpoint's own configured_by target is in the
 *         given excluded-inventory set.
 *
 *  An endpoint with no resolvable configured_by association is never
 *  excluded by this: there is nothing to match, so it is treated as not
 *  (yet) opted out rather than conservatively excluded.
 *
 *  @param[in] excludedPaths - the effective excluded-inventory set (see
 *             fetchExcludedInventory())
 *  @param[in] mctpEid - MCTP endpoint
 *  @param[in] networkId - the endpoint's MCTP network index
 *  @return true if the endpoint's configured_by target is excluded
 */
bool isExcludedInventory(const ExcludedInventoryPaths& excludedPaths,
                         pldm::eid mctpEid, NetworkId networkId);

} // namespace pldm::fw_update::em_config
