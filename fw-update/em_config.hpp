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

#include <map>
#include <optional>
#include <set>
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

/** @brief Derive the expected (updatable) component identifiers for the
 *         pre-update validation gate from a device's EM-declared component
 *         id → Name map (DeviceComponentInfo::idNameMap).
 *
 *  Every EM-declared component with a non-empty Name is expected, except
 *  inventory-only components (see isFirmwareInventoryOnlyComponent()): a
 * firmware package never carries an image for them (e.g. GPU InfoROM), so
 * requiring one would reject every whole-system request. UpdateOnly concerns
 *  Software.Version object ownership, not updatability, so it does not
 *  exempt a component.
 *
 *  @param[in] emComponents - id → component metadata from fetchComponentInfo()
 *  @return the ComponentName-grouped expected component identifiers
 */
ExpectedComponentIdsByName expectedComponentIds(
    const CreateComponentIdNameMap& emComponents);

/** @brief The MCTPTargetName of every entity-manager-configured PLDM
 *         firmware device that has at least one updatable component.
 *
 *  Read straight from Configuration.PLDMFirmwareDevice rather than from the
 *  discovered endpoints, so a configured device that has left MCTP (recovery
 *  mode, powered off, hung management path) is still listed. Devices whose
 *  components are all inventory-only are omitted: there is nothing to update
 *  on them. D-Bus read failures are logged and skipped; never thrown.
 *
 *  @return the configured firmware device target names
 */
std::set<std::string> fetchConfiguredFirmwareTargets();

/** @brief Device names of the entity-manager MCTP transport configurations,
 *         keyed by their StaticEndpointID.
 *
 *  A fallback for resolving a statically addressed endpoint to its device
 *  name when its configured_by association is missing (e.g. the reactor
 *  did not republish it after the endpoint was re-added). Bridge-pool
 *  devices have no StaticEndpointID and are not listed. D-Bus read failures
 *  are logged and skipped; never thrown.
 *
 *  @return StaticEndpointID → the transport configuration Names using it
 */
std::map<pldm::eid, std::set<std::string>> fetchStaticEidTargetNames();

/** @brief Whether an EM-declared component is inventory-only (reported but
 *         never updated).
 *
 *  entity-manager marks updatable components with the "activation" backward
 *  association to their inventory item; inventory-only components carry
 *  only "active". A component with no associations is not inventory-only.
 *
 *  @param[in] associations - the component's (forward, backward, endpoint)
 *                            associations
 *  @return true when associations are declared and none is "activation"
 */
bool isFirmwareInventoryOnlyComponent(const Associations& associations);

/** @brief entity-manager interface carrying the firmware-update opt-out. */
constexpr auto pldmExclusionIntf =
    "xyz.openbmc_project.Configuration.PLDMExclusion";

/** @brief The flat inventory-path array published on @ref pldmExclusionIntf. */
constexpr auto excludedInventoryProp = "ExcludedInventory";

/** @brief Read the inventory paths currently excluded from PLDM T5 firmware
 *         update, unioned across every publishing entity-manager object.
 *
 *  Stateless: each call re-queries ObjectMapper rather than caching. The
 *  caller (see Manager::handleMctpEndpoints()) caches the result instead.
 *
 *  @return the effective excluded-inventory set; empty when nothing is
 *          excluded
 */
ExcludedInventoryPaths fetchExcludedInventory();

/** @brief D-Bus interface publishing the configured_by/configures
 *         associations that identify a device to firmware update.
 *
 *  Published by mctpreactor on the MCTP endpoint object itself, not by
 *  entity-manager.
 */
constexpr auto associationDefinitionsIntf =
    "xyz.openbmc_project.Association.Definitions";

/** @brief Read the configured_by association target directly off one MCTP
 *         endpoint's own Association.Definitions.
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
 *  @param[in] excludedPaths - the effective excluded-inventory set (see
 *             fetchExcludedInventory())
 *  @param[in] mctpEid - MCTP endpoint
 *  @param[in] networkId - the endpoint's MCTP network index
 *  @return true if the endpoint's configured_by target is excluded
 */
bool isExcludedInventory(const ExcludedInventoryPaths& excludedPaths,
                         pldm::eid mctpEid, NetworkId networkId);

} // namespace pldm::fw_update::em_config
