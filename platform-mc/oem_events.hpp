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

#include <cstdint>
#include <string>

namespace pldm
{
namespace oem_events
{

/**
 * @brief Base directory to store PLDM OEM event data
 *
 * Event files are stored in terminus-specific subdirectories:
 *   /var/lib/pldm_events/<terminus>/
 *
 * This directory is monitored by the pldm-event-dump tool via inotify.
 * When event files are written here, the dump tool processes them.
 */
constexpr const char* PLDM_EVENT_DIR = "/var/lib/pldm_events";

/**
 * @brief Staging file names for different OEM event types
 * Must match names expected by pldm-event-dump tool in PDC
 */
constexpr const char* CPER_ERROR_COUNT_FILE = "CPERErrorCount_0_0.bin";
constexpr const char* PCIE_LTSSM_FILE = "PCIeLTSSM_0_0.bin";
constexpr const char* PCIE_TELEMETRY_FILE = "PCIeTelemetry_0_0.bin";

/**
 * @brief Fixed output path for the Inventory JSON file.
 *
 * Used when OEM event 0xFC carries inventory data.
 * The inventory is written to a single well-known location consumed
 * by the nvidia-inventory service.
 */
constexpr const char* INVENTORY_FILE = "/var/lib/inventory/inventory.json";

/**
 * @brief Handle CPER Error Counter Event (0xF1)
 *
 * Writes the event payload to the CPER error counter staging file.
 * The pldm-event-dump tool monitors this file via inotify.
 *
 * @param[in] terminus      Terminus name (e.g., "ProcessorModule_0")
 * @param[in] eventData     Pointer to event data payload
 * @param[in] eventDataSize Size of event data payload in bytes
 * @return true on success, false on failure
 */
bool handleCperErrorCountEvent(const std::string& terminus,
                               const uint8_t* eventData, size_t eventDataSize);

/**
 * @brief Handle PCIe LTSSM Event (0xF0)
 *
 * Writes the event payload to the PCIe LTSSM staging file.
 * The pldm-event-dump tool monitors this file via inotify.
 *
 * @param[in] terminus      Terminus name (e.g., "ProcessorModule_0")
 * @param[in] eventData     Pointer to event data payload
 * @param[in] eventDataSize Size of event data payload in bytes
 * @return true on success, false on failure
 */
bool handlePcieLtssmEvent(const std::string& terminus, const uint8_t* eventData,
                          size_t eventDataSize);

/**
 * @brief Handle PCIe Telemetry Event (0xF2)
 *
 * Writes the event payload to the PCIe telemetry staging file.
 * The pldm-event-dump tool monitors this file via inotify.
 *
 * @param[in] terminus      Terminus name (e.g., "ProcessorModule_0")
 * @param[in] eventData     Pointer to event data payload
 * @param[in] eventDataSize Size of event data payload in bytes
 * @return true on success, false on failure
 */
bool handlePcieTelemetryEvent(const std::string& terminus,
                              const uint8_t* eventData, size_t eventDataSize);

/**
 * @brief Handle OEM event 0xFC as SatMC Inventory JSON (new projects)
 *
 * Parses the 4-byte OEM event header, then writes the UTF-8 JSON payload
 * directly to INVENTORY_FILE.
 * (meson option satmc-inventory).
 *
 * @param[in] eventData     Raw event data including the 4-byte OEM header
 * @param[in] eventDataSize Size of eventData in bytes
 * @return true on success, false on failure
 */
bool handleInventoryEvent(const uint8_t* eventData, size_t eventDataSize);

/**
 * @brief Handle OEM event 0xFC as SMBIOS MDR data (legacy)
 *
 * Decodes the SMBIOS event data and triggers an MDR sync.
 *
 * @param[in] eventData     Raw event data including the SMBIOS event header
 * @param[in] eventDataSize Size of eventData in bytes
 * @return true on success, false on failure
 */
bool handleSmbiosEvent(const uint8_t* eventData, size_t eventDataSize);

} // namespace oem_events
} // namespace pldm
