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
#include "common/types.hpp"

#include <filesystem>

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

} // namespace pldm::fw_update
