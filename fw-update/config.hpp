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
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

PHOSPHOR_LOG2_USING;

namespace fs = std::filesystem;

namespace pldm::fw_update
{

/** @brief Parse the firmware update config file
 *
 *  Parses the config file to generate D-Bus firmware inventory from firmware
 *  update inventory commands. The config file also generates args for update
 *  message registry entries.
 *
 *  Device (RoT chassis) inventory is not parsed here: the RoT chassis objects
 *  are owned by entity-manager (Configuration.PLDMDeviceInventory) and pldmd
 *  only writes the dynamic UUID to the EM-created object.
 *
 *  @param[in] jsonPath - Path of firmware update config file
 *  @param[out] fwInventoryInfo - D-Bus firmware inventory config info
 *  @param[out] componentNameMapInfo - Component name info
 *
 */
void parseConfig(const fs::path& jsonPath,
                 FirmwareInventoryInfo& fwInventoryInfo,
                 ComponentNameMapInfo& componentNameMapInfo);

} // namespace pldm::fw_update
