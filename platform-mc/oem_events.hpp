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
constexpr const char* PCOREDUMP_FILE = "PCoreDump_0_0.bin";
constexpr const char* PCIE_TELEMETRY_FILE = "PCIeTelemetry_0_0.bin";

/**
 * @brief D-Bus identifiers for dbus-sensors nvidia-info CreateInfo.
 *
 * OEM event 0xF3 inventory JSON is passed in-band via
 *   xyz.openbmc_project.NvidiaInfo
 *     /xyz/openbmc_project/NvidiaInfo
 *       CreateInfo(int32 processorModuleIndex, string jsonPayload)
 * (processor module index 0 or 1; matches terminus ProcessorModule_0 / _1).
 */
constexpr const char* nvidiaInfoService = "xyz.openbmc_project.NvidiaInfo";
constexpr const char* nvidiaInfoObjectPath = "/xyz/openbmc_project/NvidiaInfo";
constexpr const char* nvidiaInfoInterface = "xyz.openbmc_project.NvidiaInfo";

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
 * @brief Handle PCoreDump Event (0xF2)
 *
 * Writes the complete event buffer to the PCoreDump staging file.
 * The pldm-event-dump tool monitors this file via inotify.
 *
 * @param[in] terminus      Terminus name (e.g., "ProcessorModule_0")
 * @param[in] eventData     Pointer to event data payload
 * @param[in] eventDataSize Size of event data payload in bytes
 * @return true on success, false on failure
 */
bool handlePCoreDumpEvent(const std::string& terminus, const uint8_t* eventData,
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
 * @brief Handle OEM event 0xF3 as SatMC Inventory JSON
 *
 * Parses the 4-byte OEM event header, then sends the UTF-8 JSON payload to
 * NvidiaInfo.CreateInfo over D-Bus (terminus must be ProcessorModule_0 or
 * ProcessorModule_1). (meson option satmc-inventory).
 *
 * @param[in] terminus      Terminus name (e.g., "ProcessorModule_0")
 * @param[in] eventData     Raw event data including the 4-byte OEM header
 * @param[in] eventDataSize Size of eventData in bytes
 * @return true on success, false on failure
 */
bool handleInventoryEvent(const std::string& terminusName,
                          const uint8_t* eventData, size_t eventDataSize);

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
