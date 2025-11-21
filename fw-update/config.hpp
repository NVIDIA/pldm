/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION &
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

#include <libpldm/pldm.h>

#include <phosphor-logging/lg2.hpp>

#include <filesystem>
#include <string>
#include <unordered_map>

PHOSPHOR_LOG2_USING;

namespace fs = std::filesystem;

namespace pldm::fw_update
{

/** @brief Parse the firmware update config file
 *
 *  Parses the config file to generate D-Bus device inventory and firmware
 *  inventory from firmware update inventory commands. The config file also
 *  generate args for update message registry entries.
 *
 *  @param[in] jsonPath - Path of firmware update config file
 *  @param[out] deviceInventoryInfo - D-Bus device inventory config info
 *  @param[out] fwInventoryInfo - D-Bus firmware inventory config info
 *  @param[out] componentNameMapInfo - Component name info
 *
 */
void parseConfig(const fs::path& jsonPath,
                 DeviceInventoryInfo& deviceInventoryInfo,
                 FirmwareInventoryInfo& fwInventoryInfo,
                 ComponentNameMapInfo& componentNameMapInfo);

/** @brief Get device/component name from EID using fw_update_config.json
 *
 *  This function parses the fw_update_config.json file once (on first call) and
 *  builds a cached map of EID→device_name for fast lookups. The device name is
 *  extracted from the "object_path" field in each config entry.
 *
 *  Extraction logic:
 *  1. Primary: Extract from device_inventory.create.object_path (if present)
 *  2. Fallback: If no device_inventory, extract from first firmware component
 * name
 *
 *  Example config with device_inventory (EID 9):
 *    "match": {"Interface": "xyz.openbmc_project.MCTP.Endpoint",
 *              "Properties": [{"Name": "EID", "Type": "y", "Value": 9}]},
 *    "device_inventory": {
 *      "create": {
 *        "object_path":
 * "/xyz/openbmc_project/inventory/system/chassis/HGX_ERoT_BMC_0"
 *      }
 *    }
 *  → Returns: "HGX_ERoT_BMC_0"
 *
 *  Example config without device_inventory (EID 14):
 *    "match": {"Interface": "xyz.openbmc_project.MCTP.Endpoint",
 *              "Properties": [{"Name": "EID", "Type": "y", "Value": 14}]},
 *    "firmware_inventory": {
 *      "create": {
 *        "HGX_SBIOS_FMC_0": { ... }
 *      }
 *    }
 *  → Returns: "HGX_SBIOS_FMC_0"
 *
 *  This is thread-safe using C++11 static initialization guarantees.
 *
 *  @param[in] eid - endpoint ID of the device
 *  @return optional device name string (filename of object path or first
 * component name), std::nullopt if not found
 */
std::optional<std::string> getDeviceNameFromEid(mctp_eid_t eid);

} // namespace pldm::fw_update
