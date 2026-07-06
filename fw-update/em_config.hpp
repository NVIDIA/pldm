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

} // namespace pldm::fw_update::em_config
