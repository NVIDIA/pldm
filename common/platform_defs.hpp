#pragma once

#include <cstdint>

namespace pldm
{

namespace platform
{

/**
 * @brief OEM Event Classes
 *
 * OEM event classes 0xFA-0xFC are defined in libpldm/platform.h.
 * but there is no OEM support like decode or encode in libpldm.
 * So it make sense to define OEM classes here instead of libpldm.
 */

// OEM Event Class for Telemetry Management
// Used by terminus devices (e.g., during Live Firmware Activation)
constexpr uint8_t PLDM_OEM_EVENT_CLASS_0xFD = 0xFD;

// Telemetry Management Event States for OEM Event 0xFD
constexpr uint8_t PLDM_TELEMETRY_PAUSE = 0x00;      // Pause Type 2 monitoring
constexpr uint8_t PLDM_TELEMETRY_REDISCOVER = 0x01; // Rediscover Type 2
constexpr uint8_t PLDM_TELEMETRY_RESUME = 0x02;     // Resume Type 2 monitoring

// OEM Event Classes for SatMC Diagnostic Data
// These events carry diagnostic data from SatMC to HMC after effecter trigger
constexpr uint8_t PLDM_OEM_EVENT_CLASS_ERROR_COUNTER = 0xF0;
constexpr uint8_t PLDM_OEM_EVENT_CLASS_PCIE_TELEMETRY = 0xF1;
constexpr uint8_t PLDM_OEM_EVENT_CLASS_MFTDUMP = 0xF2;

// OEM Event Class for SatMC Inventory JSON
constexpr uint8_t PLDM_OEM_EVENT_CLASS_0xF3 = 0xF3;

} // namespace platform

} // namespace pldm
