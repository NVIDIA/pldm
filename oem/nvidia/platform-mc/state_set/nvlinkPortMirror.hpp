/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */
#pragma once

// === WORKAROUND: NVLink per-partition Port mirroring (vr-nvl-hmc) ===
// SatMC publishes one state sensor per NVLink partition. Expand each partition
// sensor into N Inventory.Item.Port D-Bus objects so all per-CPU NVLink Ports
// are present. Partition P -> Port indices [P*N .. P*N + N - 1].
// N = NUMBER_OF_LINKS_PER_PARTITION (meson `number-of-links-per-partition`,
// default 1 = no mirroring).

#include "libpldm/oem/nvidia/state_set_oem_nvidia.h"

#include "common/types.hpp"
#include "platform-mc/state_set.hpp"

#include <phosphor-logging/lg2.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/PortInfo/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/PortState/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Port/server.hpp>

#include <memory>
#include <string>
#include <vector>

#ifdef OEM_NVIDIA
#include <tal.hpp>
#endif

#ifndef NUMBER_OF_LINKS_PER_PARTITION
#define NUMBER_OF_LINKS_PER_PARTITION 1
#endif

namespace pldm
{
namespace platform_mc
{
namespace oem_nvidia
{

using PortIntf = sdbusplus::server::object_t<
    sdbusplus::server::xyz::openbmc_project::inventory::item::Port>;
using PortInfoIntf = sdbusplus::server::object_t<
    sdbusplus::server::xyz::openbmc_project::inventory::decorator::PortInfo>;
using PortStateIntf = sdbusplus::server::object_t<
    sdbusplus::server::xyz::openbmc_project::inventory::decorator::PortState>;

using PortType = sdbusplus::server::xyz::openbmc_project::inventory::decorator::
    PortInfo::PortType;
using PortProtocol = sdbusplus::server::xyz::openbmc_project::inventory::
    decorator::PortInfo::PortProtocol;
using PortLinkStates = sdbusplus::server::xyz::openbmc_project::inventory::
    decorator::PortState::LinkStates;
using PortLinkStatus = sdbusplus::server::xyz::openbmc_project::inventory::
    decorator::PortState::LinkStatusType;

/** @brief Owns the N-1 mirror Ports for a partition state sensor and
 *         propagates state / info / association / shmem to every mirror.
 *         All operations are no-ops when N == 1.
 */
class NvlinkPortMirror
{
  public:
    NvlinkPortMirror() = default;
    ~NvlinkPortMirror() = default;
    NvlinkPortMirror(const NvlinkPortMirror&) = delete;
    NvlinkPortMirror& operator=(const NvlinkPortMirror&) = delete;
    NvlinkPortMirror(NvlinkPortMirror&&) = default;
    NvlinkPortMirror& operator=(NvlinkPortMirror&&) = default;

    /** @brief Resolve N Port object paths from @p sourcePath. paths[0] is the
     *         primary; paths[1..N-1] are mirrors. Returns just @p sourcePath
     *         when the workaround is off or doesn't apply.
     */
    static std::vector<std::string> computePaths(
        [[maybe_unused]] uint16_t stateSetId, const std::string& sourcePath)
    {
        std::vector<std::string> paths{sourcePath};

#if NUMBER_OF_LINKS_PER_PARTITION > 1
        if (stateSetId != PLDM_NVIDIA_OEM_STATE_SET_NVLINK)
        {
            return paths;
        }

        const auto pos = sourcePath.rfind('_');
        if (pos == std::string::npos || pos + 1 >= sourcePath.size())
        {
            return paths;
        }

        size_t firmwareIdx = 0;
        try
        {
            firmwareIdx =
                static_cast<size_t>(std::stoul(sourcePath.substr(pos + 1)));
        }
        catch (const std::exception& e)
        {
            lg2::error("NVLink mirror: bad partition index in {PATH}: {ERROR}",
                       "PATH", sourcePath, "ERROR", e.what());
            return paths;
        }

        const std::string prefix = sourcePath.substr(0, pos + 1);
        const size_t base = firmwareIdx * NUMBER_OF_LINKS_PER_PARTITION;

        paths.clear();
        paths.reserve(NUMBER_OF_LINKS_PER_PARTITION);
        for (size_t i = 0; i < NUMBER_OF_LINKS_PER_PARTITION; ++i)
        {
            paths.emplace_back(prefix + std::to_string(base + i));
        }
#endif // NUMBER_OF_LINKS_PER_PARTITION > 1

        return paths;
    }

    /** @brief Materialise mirror Port D-Bus objects for paths[1..N-1]. */
    void init(sdbusplus::bus_t& bus, const std::vector<std::string>& paths,
              const dbus::PathAssociation& assoc)
    {
        for (size_t i = 1; i < paths.size(); ++i)
        {
            MirrorPort mp;
            mp.path = paths[i];
            mp.assocDefsIntf = std::make_unique<AssociationDefinitionsInft>(
                bus, mp.path.c_str());
            mp.assocDefsIntf->associations(
                {{assoc.forward.c_str(), assoc.reverse.c_str(),
                  assoc.path.c_str()}});
            mp.portIntf = std::make_unique<PortIntf>(bus, mp.path.c_str());
            mp.portInfoIntf =
                std::make_unique<PortInfoIntf>(bus, mp.path.c_str());
            mp.portStateIntf =
                std::make_unique<PortStateIntf>(bus, mp.path.c_str());
            mirrors.push_back(std::move(mp));
        }
    }

    /** @brief Re-publish the chassis association on every mirror Port. */
    void applyAssociation(const dbus::PathAssociation& assoc)
    {
        for (auto& mp : mirrors)
        {
            mp.assocDefsIntf->associations(
                {{assoc.forward.c_str(), assoc.reverse.c_str(),
                  assoc.path.c_str()}});
        }
    }

    /** @brief Apply default PortInfo / PortState values to every mirror. */
    void applyDefaults(PortType type, PortProtocol protocol)
    {
        for (auto& mp : mirrors)
        {
            mp.portInfoIntf->type(type);
            mp.portInfoIntf->protocol(protocol);
            mp.portStateIntf->linkState(PortLinkStates::Unknown);
            mp.portStateIntf->linkStatus(PortLinkStatus::NoLink);
        }
    }

    /** @brief Apply a state update to every mirror. */
    void applyState(PortLinkStates linkState, PortLinkStatus linkStatus)
    {
        for (auto& mp : mirrors)
        {
            mp.portStateIntf->linkState(linkState);
            mp.portStateIntf->linkStatus(linkStatus);
        }
    }

#ifdef OEM_NVIDIA
    /** @brief Publish a shmem telemetry reading for every mirror Port. */
    void publishShmem(const std::string& iface, const std::string& property,
                      DbusVariantType& value, uint64_t timestamp,
                      const std::string& endpoint) const
    {
        std::vector<uint8_t> emptyRaw;
        constexpr uint16_t okRetCode = 0;
        for (const auto& mp : mirrors)
        {
            tal::TelemetryAggregator::updateTelemetry(
                mp.path, iface, property, emptyRaw, timestamp, okRetCode, value,
                endpoint);
        }
    }
#endif

    [[nodiscard]] bool empty() const noexcept
    {
        return mirrors.empty();
    }

    [[nodiscard]] size_t size() const noexcept
    {
        return mirrors.size();
    }

  private:
    struct MirrorPort
    {
        std::string path;
        std::unique_ptr<AssociationDefinitionsInft> assocDefsIntf;
        std::unique_ptr<PortIntf> portIntf;
        std::unique_ptr<PortInfoIntf> portInfoIntf;
        std::unique_ptr<PortStateIntf> portStateIntf;
    };

    std::vector<MirrorPort> mirrors;
};

} // namespace oem_nvidia
} // namespace platform_mc
} // namespace pldm
