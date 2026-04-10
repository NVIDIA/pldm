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
#include "oem_events.hpp"

#include "libpldm/platform.h"

#include "common/utils.hpp"
#include "smbios_mdr.hpp"

#include <endian.h>

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>

#include <cerrno>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string_view>

namespace pldm
{
namespace oem_events
{

namespace fs = std::filesystem;

// OEM Event Data Header Format
// Format: [formatVersion(1B)][formatType(1B)][payloadSize(2B)][payload...]
constexpr size_t OEM_EVENT_HEADER_SIZE = 4;
constexpr uint8_t OEM_EVENT_FORMAT_VERSION = 0x01;
constexpr uint8_t OEM_EVENT_FORMAT_TYPE_FULL = 0x00;

#pragma pack(push, 1)
struct OemEventHeader
{
    uint8_t formatVersion; // Expected: 0x01
    uint8_t formatType;    // 0x00 = Full payload
    uint16_t payloadSize;  // Size of the payload
};
#pragma pack(pop)

/**
 * @brief Sanitize terminus name to prevent path traversal
 *
 * Replaces path separators and removes dangerous sequences to ensure
 * the terminus name cannot escape the intended directory tree.
 *
 * @param[in] terminus Raw terminus name from PLDM
 * @return Sanitized terminus name safe for filesystem use
 */
static std::string sanitizeTerminusName(const std::string& terminus)
{
    std::string sanitized = terminus;

    // Replace path separators with underscores
    for (char& c : sanitized)
    {
        if (c == '/' || c == '\\')
        {
            c = '_';
        }
    }

    // Remove any ".." sequences that could escape the directory
    size_t pos;
    while ((pos = sanitized.find("..")) != std::string::npos)
    {
        sanitized.erase(pos, 2);
    }

    // If sanitization results in empty string, use a default
    if (sanitized.empty())
    {
        sanitized = "unknown_terminus";
    }

    return sanitized;
}

/**
 * @brief Get the terminus-specific event directory path
 *
 * @param[in] terminus Terminus name (e.g., "ProcessorModule_0")
 * @return Full path to the terminus event directory
 */
static std::string getEventDir(const std::string& terminus)
{
    return std::format("{}/{}", PLDM_EVENT_DIR, sanitizeTerminusName(terminus));
}

/**
 * @brief Internal helper to save event data to a staging file
 *
 * Event data format:
 *   uint8  formatVersion: 0x01
 *   uint8  formatType: 0x00 = Full payload, non-zero = Reserved
 *   uint16 payloadSize: Size of the payload
 *   uint8  payload[]: Event payload starts here
 *
 * Only the payload is written to file (header is stripped).
 *
 * @param[in] terminus      Terminus name (e.g., "ProcessorModule_0")
 * @param[in] fileName      Event file name
 * @param[in] eventData     Pointer to event data (including header)
 * @param[in] eventDataSize Size of event data in bytes (including header)
 * @return true on success, false on failure
 */
static bool saveEventData(const std::string& terminus,
                          const std::string& fileName, const uint8_t* eventData,
                          size_t eventDataSize)
{
    // Validate minimum size for header
    if (eventDataSize < OEM_EVENT_HEADER_SIZE)
    {
        lg2::error("OEM event data too small: size={SIZE}, minimum={MIN}",
                   "SIZE", eventDataSize, "MIN", OEM_EVENT_HEADER_SIZE);
        return false;
    }

    // Parse the header
    const auto* header = reinterpret_cast<const OemEventHeader*>(eventData);
    uint8_t formatVersion = header->formatVersion;
    uint8_t formatType = header->formatType;
    uint16_t payloadSize = le16toh(header->payloadSize);

    // Log header information
    lg2::info("OEM event header: formatVersion={VER:#x}, formatType={TYPE:#x}, "
              "payloadSize={PSIZE}, totalSize={TSIZE}",
              "VER", formatVersion, "TYPE", formatType, "PSIZE", payloadSize,
              "TSIZE", eventDataSize);

    // Validate header values
    if (formatVersion != OEM_EVENT_FORMAT_VERSION)
    {
        lg2::warning(
            "OEM event unexpected format version: {VER:#x}, expected {EXP:#x}",
            "VER", formatVersion, "EXP", OEM_EVENT_FORMAT_VERSION);
    }

    if (formatType != OEM_EVENT_FORMAT_TYPE_FULL)
    {
        lg2::warning("OEM event non-zero format type: {TYPE:#x} (reserved)",
                     "TYPE", formatType);
    }

    // Validate payload size matches
    size_t expectedTotal = OEM_EVENT_HEADER_SIZE + payloadSize;
    if (eventDataSize < expectedTotal)
    {
        lg2::error("OEM event data size mismatch: got={GOT}, expected={EXP} "
                   "(header={HDR} + payload={PAY})",
                   "GOT", eventDataSize, "EXP", expectedTotal, "HDR",
                   OEM_EVENT_HEADER_SIZE, "PAY", payloadSize);
        return false;
    }

    // Get pointer to payload (after header)
    const uint8_t* payload = eventData + OEM_EVENT_HEADER_SIZE;

    // Ensure the terminus-specific directory exists
    std::string eventDir = getEventDir(terminus);
    try
    {
        fs::path dir(eventDir);
        if (!fs::exists(dir))
        {
            fs::create_directories(dir);
            lg2::info("Created PLDM events directory: {DIR}", "DIR", eventDir);
        }
    }
    catch (const fs::filesystem_error& e)
    {
        lg2::error("Failed to create PLDM events directory: {ERROR}", "ERROR",
                   e.what());
        return false;
    }

    std::string filePath = std::format("{}/{}", eventDir, fileName);

    // Write payload only to staging file (header is stripped)
    // Use a temp file and rename for atomic write operation
    std::string tempPath = std::format("{}.tmp", filePath);

    try
    {
        std::ofstream outFile(tempPath, std::ios::binary | std::ios::trunc);
        if (!outFile.is_open())
        {
            lg2::error("Failed to open temp file for writing: {PATH}", "PATH",
                       tempPath);
            return false;
        }

        // Write only the payload, not the header
        outFile.write(reinterpret_cast<const char*>(payload),
                      static_cast<std::streamsize>(payloadSize));

        if (!outFile)
        {
            lg2::error("Failed to write event data to file: {PATH}", "PATH",
                       tempPath);
            fs::remove(tempPath);
            return false;
        }

        outFile.close();
        if (!outFile)
        {
            lg2::error("Failed to flush/close event data file: {PATH}", "PATH",
                       tempPath);
            fs::remove(tempPath);
            return false;
        }

        // Atomic rename to final file path
        // This ensures the inotify watcher sees a complete file
        fs::rename(tempPath, filePath);

        lg2::info("Saved OEM event payload: size={SIZE}, path={PATH}", "SIZE",
                  payloadSize, "PATH", filePath);

        return true;
    }
    catch (const std::exception& e)
    {
        lg2::error("Exception while saving OEM event data: {ERROR}", "ERROR",
                   e.what());
        try
        {
            fs::remove(tempPath);
        }
        catch (...)
        {}
        return false;
    }
}

bool handleCperErrorCountEvent(const std::string& terminus,
                               const uint8_t* eventData, size_t eventDataSize)
{
    lg2::info(
        "Processing CPER Error Count Event (0xF0), terminus={TERM}, size={SIZE}",
        "TERM", terminus, "SIZE", eventDataSize);

    return saveEventData(terminus, CPER_ERROR_COUNT_FILE, eventData,
                         eventDataSize);
}

bool handlePcieLtssmEvent(const std::string& terminus, const uint8_t* eventData,
                          size_t eventDataSize)
{
    lg2::info(
        "Processing PCIe LTSSM Event (0xF2), terminus={TERM}, size={SIZE}",
        "TERM", terminus, "SIZE", eventDataSize);

    return saveEventData(terminus, PCIE_LTSSM_FILE, eventData, eventDataSize);
}

bool handlePcieTelemetryEvent(const std::string& terminus,
                              const uint8_t* eventData, size_t eventDataSize)
{
    lg2::info(
        "Processing PCIe Telemetry Event (0xF1), terminus={TERM}, size={SIZE}",
        "TERM", terminus, "SIZE", eventDataSize);

    return saveEventData(terminus, PCIE_TELEMETRY_FILE, eventData,
                         eventDataSize);
}

/**
 * @brief Map PLDM terminus name to processor module index for CreateInfo.
 *
 * CreateInfo accepts indices 0 and 1 only (terminus ProcessorModule_0 / _1).
 */
static std::optional<int32_t> processorModuleIndexFromTerminus(
    const std::string& terminusName)
{
    constexpr std::string_view prefix = "ProcessorModule_";

    auto tryParse = [&](std::string_view s) -> std::optional<int32_t> {
        if (!s.starts_with(prefix))
        {
            return std::nullopt;
        }
        s.remove_prefix(prefix.size());
        int32_t val{};
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
        if (ec != std::errc{} || ptr != s.data() + s.size())
        {
            return std::nullopt;
        }
        return val;
    };

    if (auto idx = tryParse(terminusName))
    {
        return idx;
    }
    return tryParse(sanitizeTerminusName(terminusName));
}

/** @brief Pass inventory JSON to dbus-sensors NvidiaInfo.CreateInfo. */
static bool callNvidiaInfoCreateInfo(const std::string& jsonPayload,
                                     int32_t processorModuleIndex)
{
    try
    {
        auto& bus = pldm::utils::DBusHandler::getBus();
        auto method =
            bus.new_method_call(nvidiaInfoService, nvidiaInfoObjectPath,
                                nvidiaInfoInterface, "CreateInfo");
        method.append(processorModuleIndex, jsonPayload);
        bus.call_noreply(method);
        lg2::info(
            "Called NvidiaInfo.CreateInfo: processorModule={M}, len={LEN}", "M",
            processorModuleIndex, "LEN", jsonPayload.size());
        return true;
    }
    catch (const sdbusplus::exception_t& e)
    {
        lg2::error("Failed to call CreateInfo on NvidiaInfo: {E}", "E",
                   e.what());
        return false;
    }
}

bool handleInventoryEvent(const std::string& terminusName,
                          const uint8_t* eventData, size_t eventDataSize)
{
    // Parse the 4-byte OEM header: [formatVersion][formatType][payloadSize(2B
    // LE)]
    constexpr size_t OEM_HDR_SIZE = 4;
    if (eventDataSize < OEM_HDR_SIZE)
    {
        lg2::error("Inventory event too small: size={SIZE}", "SIZE",
                   eventDataSize);
        return false;
    }

    uint8_t formatVersion = eventData[0];
    uint8_t formatType = eventData[1];
    uint16_t payloadSize = static_cast<uint16_t>(eventData[2]) |
                           (static_cast<uint16_t>(eventData[3]) << 8);
    const uint8_t* payload = eventData + OEM_HDR_SIZE;

    if (eventDataSize < static_cast<size_t>(OEM_HDR_SIZE + payloadSize))
    {
        lg2::error("Inventory event truncated: got={GOT}, expected={EXP}",
                   "GOT", eventDataSize, "EXP", OEM_HDR_SIZE + payloadSize);
        return false;
    }

    std::string verStr = std::format("{:#04x}", formatVersion);
    std::string typeStr = std::format("{:#04x}", formatType);
    lg2::info(
        "Received Inventory Event (0xF3) terminus={TERM} ver={VER} type={TYPE} payloadSize={LEN}",
        "TERM", terminusName, "VER", verStr, "TYPE", typeStr, "LEN",
        payloadSize);

    if (payload == nullptr || payloadSize == 0)
    {
        lg2::error("Inventory event: empty payload");
        return false;
    }

    // SatMC sends plain UTF-8 JSON text (output of cJSON_Print()) directly.
    std::string jsonText(reinterpret_cast<const char*>(payload), payloadSize);

    std::optional<int32_t> moduleIdx =
        processorModuleIndexFromTerminus(terminusName);
    if (!moduleIdx)
    {
        lg2::error("Inventory event: terminus {T} is not ProcessorModule_0 or "
                   "ProcessorModule_1",
                   "T", terminusName);
        return false;
    }

    return callNvidiaInfoCreateInfo(jsonText, *moduleIdx);
}

bool handleSmbiosEvent(const uint8_t* eventData, size_t eventDataSize)
{
    lg2::info("Processing SMBIOS Event (0xfc)");

    uint8_t formatVersion;
    uint16_t smbiosEventDataLength;
    uint8_t* smbiosEventData;
    auto rc =
        decode_pldm_smbios_event_data(eventData, eventDataSize, &formatVersion,
                                      &smbiosEventDataLength, &smbiosEventData);

    if (rc)
    {
        lg2::error("Failed to decode SMBIOS event data, rc={RC}", "RC", rc);
        return false;
    }

    if (!mdr::saveSmbiosData(smbiosEventDataLength, smbiosEventData))
    {
        lg2::error("Failed to save SMBIOS data to file");
        return false;
    }

    if (!mdr::syncSmbiosData())
    {
        lg2::error("Failed to trigger SMBIOS MDR sync");
        return false;
    }

    return true;
}

} // namespace oem_events
} // namespace pldm
