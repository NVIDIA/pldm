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
#include "pcie_port_info.hpp"

#include "common/utils.hpp"

#include <endian.h>

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/PortInfo/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/PortWidth/server.hpp>

#include <cstring>
#include <map>
#include <memory>

namespace pldm
{
namespace oem_nvidia
{
namespace
{

using PortInfoIntf = sdbusplus::server::object_t<
    sdbusplus::server::xyz::openbmc_project::inventory::decorator::PortInfo>;
using PortWidthIntf = sdbusplus::server::object_t<
    sdbusplus::server::xyz::openbmc_project::inventory::decorator::PortWidth>;
using AssociationIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Association::server::Definitions>;
using PortType = sdbusplus::server::xyz::openbmc_project::inventory::decorator::
    PortInfo::PortType;
using PortProtocol = sdbusplus::server::xyz::openbmc_project::inventory::
    decorator::PortInfo::PortProtocol;

// LinkBWTelemetry event = a 4-byte header followed by a fixed
// pcie_rp_link_info[PCIE_MAX_RP_NUM] array.
//
// Header (4 bytes):
//   uint8  formatVersion  (0x01)
//   uint8  formatType     (0x00 = full payload; non-zero = reserved)
//   uint16 size           (payload size in bytes = PCIE_MAX_RP_NUM *
//   recordSize)
//
// pcie_rp_link_info record (7 packed bytes):
//   is_enabled(1) data_valid(1) rp_num(1)
//   link_speed(1) link_width(1) max_link_speed(1) max_link_width(1)
//
// NOTE vs the supplied interface doc, confirmed against the captured device
// payload: link_speed precedes link_width, and the *_speed fields are PCIe
// generation codes (0 = no link, 1..6 = Gen1..Gen6), not raw GT/s
// (max_link_speed reads a constant 6 = Gen6, which is not a valid GT/s value).
// Decoded here to match the wire; confirm the encoding with SatMC/RAS.
struct __attribute__((packed)) PciePortInfoHeader
{
    uint8_t formatVersion; // 0x01
    uint8_t formatType;    // 0x00 = full payload; non-zero = reserved
    uint16_t size;         // payload size in bytes (little-endian on the wire)
};

struct __attribute__((packed)) PciePortInfoRecord
{
    uint8_t isEnabled; // root-port-enabled flag (0/1), wire byte
    uint8_t dataValid; // link-info-valid flag (0/1), wire byte
    uint8_t rpNum;     // root port (link) number, 0-47
    uint8_t linkSpeed; // current link speed, PCIe gen code (1-6, 0 = no link)
    uint8_t linkWidth; // current negotiated link width (lanes)
    uint8_t maxLinkSpeed; // maximum supported link speed, PCIe gen code
    uint8_t maxLinkWidth; // maximum supported link width (lanes)
};

constexpr size_t headerSize = sizeof(PciePortInfoHeader);
constexpr size_t recordSize = sizeof(PciePortInfoRecord);
static_assert(headerSize == 4, "PciePortInfoHeader must be 4 packed bytes");
static_assert(recordSize == 7, "pcie_rp_link_info must be 7 packed bytes");
constexpr uint8_t expectedFormatVersion = 0x01;
constexpr uint8_t formatTypeFullPayload = 0x00;
constexpr size_t pcieMaxRpNum = 48; // PCIE_MAX_RP_NUM
constexpr const char* inventoryBase =
    "/xyz/openbmc_project/inventory/system/cpu";

/** @brief Convert a PCIe link-speed generation code (1-6, 0 = no link) to the
 *  per-lane data rate in Gbps. Gen1/2 use 8b/10b, Gen3-5 128b/130b, Gen6 PAM4.
 *  (Exact Gbps convention to be confirmed with SatMC/RAS.)
 */
static double genToGbps(uint8_t gen)
{
    switch (gen)
    {
        case 1:  // Gen1, 2.5 GT/s
            return 2.0;
        case 2:  // Gen2, 5 GT/s
            return 4.0;
        case 3:  // Gen3, 8 GT/s
            return 7.877;
        case 4:  // Gen4, 16 GT/s
            return 15.754;
        case 5:  // Gen5, 32 GT/s
            return 31.508;
        case 6:  // Gen6, 64 GT/s
            return 63.0;
        default: // 0 or unknown = no link
            return 0.0;
    }
}

/** @brief Extract the trailing integer of a terminus name.
 *  "ProcessorModule_0" -> "0". Defaults to "0" when none is present.
 */
static std::string moduleIndex(const std::string& terminus)
{
    size_t end = terminus.find_last_not_of("0123456789");
    if (end != std::string::npos && end + 1 < terminus.size())
    {
        return terminus.substr(end + 1);
    }
    if (end == std::string::npos && !terminus.empty())
    {
        return terminus; // whole string is digits
    }
    return "0";
}

/** @brief A single per-RP inventory object carrying PortInfo + PortWidth. */
class PciePortInfoObject
{
  public:
    PciePortInfoObject(sdbusplus::bus_t& bus, const std::string& path,
                       const std::string& cpuPath) :
        portInfo(std::make_unique<PortInfoIntf>(bus, path.c_str())),
        portWidth(std::make_unique<PortWidthIntf>(bus, path.c_str())),
        assoc(std::make_unique<AssociationIntf>(bus, path.c_str()))
    {
        portInfo->type(PortType::BidirectionalPort);
        portInfo->protocol(PortProtocol::PCIe);
        assoc->associations({{"chassis", "all_states", cpuPath}});
    }

    void update(const PciePortInfoRecord& r)
    {
        portInfo->currentSpeed(genToGbps(r.linkSpeed));
        portInfo->maxSpeed(genToGbps(r.maxLinkSpeed));
        portWidth->activeWidth(static_cast<size_t>(r.linkWidth));
        portWidth->width(static_cast<size_t>(r.maxLinkWidth));
    }

  private:
    std::unique_ptr<PortInfoIntf> portInfo;
    std::unique_ptr<PortWidthIntf> portWidth;
    std::unique_ptr<AssociationIntf> assoc;
};

/** @brief Owns the per-RP objects across events (created once, updated
 *  in place). Persisted as a function-local static so the event handler
 *  stays a free function and no existing class needs a new member.
 */
class PciePortInfoManager
{
  public:
    void handle(const std::string& terminus, const uint8_t* data, size_t size)
    {
        if (size < headerSize + recordSize)
        {
            lg2::error("PciePortInfo: payload too small ({SZ} bytes) from {T}",
                       "SZ", size, "T", terminus);
            return;
        }
        // Decode the header directly into the packed struct. The uint16 size is
        // little-endian on the wire; convert to host order explicitly. Copy
        // fields into locals before use - references to packed fields are
        // ill-formed.
        PciePortInfoHeader hdr{};
        std::memcpy(&hdr, data, sizeof(hdr));
        const uint8_t formatVersion = hdr.formatVersion;
        const uint8_t formatType = hdr.formatType;
        const size_t declaredSize = le16toh(hdr.size);
        if (formatVersion != expectedFormatVersion)
        {
            lg2::warning("PciePortInfo: unexpected formatVersion {V} from {T}",
                         "V", formatVersion, "T", terminus);
        }
        if (formatType != formatTypeFullPayload)
        {
            lg2::warning(
                "PciePortInfo: reserved formatType {FT} from {T} - ignoring",
                "FT", formatType, "T", terminus);
            return;
        }

        const uint8_t* body = data + headerSize;
        const size_t bodySize = size - headerSize;
        if (declaredSize != bodySize)
        {
            lg2::warning(
                "PciePortInfo: header size {HS} != body {BS} bytes from {T}",
                "HS", declaredSize, "BS", bodySize, "T", terminus);
        }
        size_t count = bodySize / recordSize;
        if (bodySize % recordSize)
        {
            lg2::warning("PciePortInfo: {REM} trailing bytes ignored from {T}",
                         "REM", bodySize % recordSize, "T", terminus);
        }

        const std::string mod = moduleIndex(terminus);
        const std::string cpuPath = std::string(inventoryBase) + "/CPU_" + mod;
        auto& bus = pldm::utils::DBusHandler::getBus();

        size_t updated = 0;
        for (size_t i = 0; i < count; ++i)
        {
            const uint8_t* p = body + (i * recordSize);
            // Decode the record directly into the packed struct.
            PciePortInfoRecord r{};
            std::memcpy(&r, p, sizeof(r));

            // Populate every record (enabled or disabled); only guard against
            // an out-of-range root-port index.
            if (r.rpNum >= pcieMaxRpNum)
            {
                lg2::warning("PciePortInfo: rp_num {RP} out of range from {T}",
                             "RP", r.rpNum, "T", terminus);
                continue;
            }

            std::string leaf = "ProcessorModule_";
            leaf += mod;
            leaf += "_PCIeBus_0_PCIeLink_";
            leaf += std::to_string(r.rpNum);
            std::string path = cpuPath;
            path += "/Ports/";
            path += leaf;

            // Guard the D-Bus mutation path: object construction and the
            // property setters can throw; keep one bad record from unwinding
            // the daemon's event loop.
            try
            {
                auto it = objects.find(path);
                if (it == objects.end())
                {
                    it =
                        objects
                            .emplace(path, std::make_unique<PciePortInfoObject>(
                                               bus, path, cpuPath))
                            .first;
                }
                it->second->update(r);
                ++updated;
            }
            catch (const std::exception& e)
            {
                lg2::error("PciePortInfo: failed to publish {P} from {T}: {E}",
                           "P", path, "T", terminus, "E", e.what());
            }
        }

        lg2::debug(
            "PciePortInfo: updated {N} of {TOTAL} PCIe port record(s) for {T}",
            "N", updated, "TOTAL", count, "T", terminus);
    }

  private:
    std::map<std::string, std::unique_ptr<PciePortInfoObject>> objects;
};

static PciePortInfoManager& manager()
{
    static PciePortInfoManager instance;
    return instance;
}

} // namespace

bool handlePciePortInfoEvent(const std::string& terminus,
                             const uint8_t* eventData, size_t eventDataSize)
{
    if (eventData == nullptr || eventDataSize == 0)
    {
        lg2::error("PciePortInfo event: empty payload from {T}", "T", terminus);
        return false;
    }
    manager().handle(terminus, eventData, eventDataSize);
    return true;
}

} // namespace oem_nvidia
} // namespace pldm
