#pragma once

#include <phosphor-logging/lg2.hpp>
#include <xyz/openbmc_project/Logging/Entry/server.hpp>

#include <cerrno>
#include <cstdio>

namespace mdr
{

constexpr const char* defaultFile = "/var/lib/smbios/smbios2";
constexpr const char* service = "xyz.openbmc_project.Smbios.MDR_V2";
constexpr const char* objectPath = "/xyz/openbmc_project/Smbios/MDR_V2";
constexpr const char* interface = "xyz.openbmc_project.Smbios.MDR_V2";

constexpr uint8_t dirVersion = 1;
constexpr uint8_t typeII = 2;

struct MDRSMBIOSHeader
{
    uint8_t dirVer;
    uint8_t mdrType;
    uint32_t timestamp;
    uint32_t dataSize;
} __attribute__((packed));

bool saveSmbiosData(uint16_t smbiosEventDataLength, uint8_t* smbiosEventData);
bool syncSmbiosData();

} // namespace mdr
