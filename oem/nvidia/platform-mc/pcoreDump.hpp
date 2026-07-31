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

#include "libpldm/platform.h"

#include "common/types.hpp"
#include "platform-mc/numeric_effecter.hpp"
#include "platform-mc/oem_base.hpp"

#include <com/nvidia/PCoreDump/server.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/Common/error.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace pldm
{
namespace platform_mc
{

using PCoreDumpIntf =
    sdbusplus::server::object_t<sdbusplus::server::com::nvidia::PCoreDump>;

/** @brief The association edge naming this effecter's CPU.
 *
 *  The generic edge every numeric effecter carries points wherever the PDR's
 *  entity type resolves to, which for this effecter is not agreed and may well
 *  be the board rather than the CPU. This second edge states the owning CPU
 *  outright, so a consumer can go from a processor to its dump trigger without
 *  matching on the device's auxiliary name.
 */
constexpr auto pcoreDumpCpuForward = "cpu";
constexpr auto pcoreDumpCpuReverse = "pcore_dump_control";

/** @brief Return assocs carrying exactly one CPU edge, pointing at cpuPath.
 *
 *  Replaces rather than appends, and that is load-bearing rather than tidiness:
 *  Terminus::updateAssociations re-runs on rediscovery, and setInventoryPaths
 *  rebuilds the list keyed on the (forward, reverse) pair, rewriting every
 *  entry's path -- including this one -- to the generic entity-resolved path.
 *  The OEM pass that calls this runs immediately afterwards, so it has to be
 *  able to correct the edge on every pass without accumulating a duplicate each
 *  time.
 */
inline Associations withPCoreDumpCpuAssociation(Associations assocs,
                                                const std::string& cpuPath)
{
    std::erase_if(assocs, [](const auto& association) {
        const auto& [forward, reverse, path] = association;
        return forward == pcoreDumpCpuForward && reverse == pcoreDumpCpuReverse;
    });
    assocs.emplace_back(pcoreDumpCpuForward, pcoreDumpCpuReverse, cpuPath);
    return assocs;
}

/**
 * @brief OemPCoreDumpIntf
 *
 * Publishes com.nvidia.PCoreDump on the per-CPU PCore dump numeric effecter
 * object, so a dump collector can ask SatMC for the raw dump of one PCore.
 *
 * The interface triggers and nothing else. Writing the selector makes SatMC
 * return the dump asynchronously as a multipart OEM event class 0xF2, which
 * the event path reassembles and stages on the filesystem. CreateDump
 * returning means the set was *dispatched*, never that a payload was
 * collected.
 *
 * This object holds no collection state at all: no timeout, no retry
 * decision, no partial-failure bookkeeping, and no serialization. The 0xF2
 * payload carries no correlation token, so a request can only be matched to
 * it by issuing one collection at a time -- but that is the caller's
 * discipline to keep, not something enforced here. Blocking a write because
 * pldmd believes an earlier one is still outstanding would put collection
 * policy on both sides of the D-Bus boundary and leave a stuck guard able to
 * refuse traffic the device would have accepted.
 */
class OemPCoreDumpIntf : public OemIntf, public PCoreDumpIntf
{
  public:
    /** @brief Constructor to put object onto bus at a dbus path.
     *  @param[in] bus - Bus to attach to.
     *  @param[in] path - Path to attach at.
     *  @param[in] effecter - the PCore dump numeric effecter to trigger.
     */
    OemPCoreDumpIntf(sdbusplus::bus_t& bus, const char* path,
                     NumericEffecter& effecter) :
        PCoreDumpIntf(bus, path), effecter(effecter)
    {
        // NumericEffecterBaseUnit seeds both limits with quiet_NaN until the
        // PDR populates them, and every comparison against NaN is false -- so
        // a plain range check would wave through any selector at all.
        // Qualify the bounds once, here, and refuse to dispatch while they
        // are unusable.
        if (effecter.unitIntf)
        {
            const double lo = effecter.unitIntf->pdrMinSettable();
            const double hi = effecter.unitIntf->pdrMaxSettable();
            if (std::isfinite(lo) && std::isfinite(hi) && lo >= 0.0)
            {
                // Round inwards, so the range advertised on D-Bus and the
                // range enforced below are the same set of selectors even if
                // the PDR limits are not integral.
                const auto minId = static_cast<uint64_t>(std::ceil(lo));
                const auto maxId = static_cast<uint64_t>(std::floor(hi));
                if (maxId >= minId)
                {
                    boundsUsable = true;
                    PCoreDumpIntf::minPCoreId(minId);
                    PCoreDumpIntf::maxPCoreId(maxId);
                }
            }
        }

        if (!boundsUsable)
        {
            lg2::error(
                "PCoreDump: effecter {PATH} exposes no usable selector range, CreateDump will be refused",
                "PATH", effecter.path);
        }
    }

    virtual ~OemPCoreDumpIntf() = default;

    /** @brief Trigger a raw dump of a single PCore of this processor.
     *
     *  Validates, hands the set to the terminus and returns. Every failure
     *  reported here is a pre-dispatch rejection; once the set is on the wire
     *  its outcome has no D-Bus surface at all (see dispatch below).
     */
    void createDump(uint64_t pcoreId) override
    {
        if (!boundsUsable)
        {
            lg2::error(
                "PCoreDump: refusing CreateDump on {PATH}, no usable selector range",
                "PATH", effecter.path);
            throw sdbusplus::xyz::openbmc_project::Common::Error::Unavailable();
        }

        // Range-check in base units, *before* baseToRaw(). MinPCoreId and
        // MaxPCoreId are derived from unitToBase(min/max_settable), so this
        // compares against the same units the caller asked in. Checking after
        // the conversion would compare against resolution- and offset-scaled
        // numbers, and a selector that slipped through would be written to
        // the wire as one PCore while the collector files the payload under
        // another PCore's name.
        if (pcoreId < PCoreDumpIntf::minPCoreId() ||
            pcoreId > PCoreDumpIntf::maxPCoreId())
        {
            lg2::error(
                "PCoreDump: selector {ID} out of range [{MIN}..{MAX}] on {PATH}",
                "ID", pcoreId, "MIN", PCoreDumpIntf::minPCoreId(), "MAX",
                PCoreDumpIntf::maxPCoreId(), "PATH", effecter.path);
            throw sdbusplus::xyz::openbmc_project::Common::Error::
                InvalidArgument();
        }

        if (!deviceReady())
        {
            lg2::error(
                "PCoreDump: refusing CreateDump on {PATH}, effecter is not usable",
                "PATH", effecter.path);
            throw sdbusplus::xyz::openbmc_project::Common::Error::Unavailable();
        }

        lg2::info(
            "PCoreDump: dispatching selector {ID} to tid {TID} via {PATH}",
            "ID", pcoreId, "TID", effecter.tid, "PATH", effecter.path);

        // setNumericEffecterValue is a coroutine while sdbusplus method
        // handlers are synchronous, so the set is started detached and this
        // method returns as soon as it is handed off. A non-SUCCESS
        // completion code therefore cannot be thrown back to the caller and
        // has no other D-Bus surface either -- it reaches the journal here and
        // the collector sees it as a collection that produced no payload.
        stdexec::start_detached(
            effecter.setNumericEffecterValue(
                effecter.baseToRaw(static_cast<double>(pcoreId))) |
                stdexec::then([this, pcoreId](int rc) {
                    if (rc == PLDM_SUCCESS)
                    {
                        return;
                    }
                    lg2::error(
                        "PCoreDump: set of selector {ID} on tid {TID} failed, rc={RC}. No payload will arrive; the caller sees a collection timeout",
                        "ID", pcoreId, "TID", effecter.tid, "RC", rc);
                }),
            exec::default_task_context<void>(exec::inline_scheduler{}));
    }

  private:
    /** @brief Whether the effecter is in a state worth dispatching to.
     *
     *  Deliberately phrased as "reject the known-bad" rather than "require
     *  Enabled": OperationalStatus only carries a state once the effecter has
     *  been read back from the device, so demanding Enabled would refuse the
     *  very first collection on an effecter that has never been polled.
     */
    bool deviceReady()
    {
        if (!effecter.isAvailable())
        {
            return false;
        }
        switch (effecter.state())
        {
            case StateType::Absent:
            case StateType::Disabled:
            case StateType::Fault:
            case StateType::Starting:
            case StateType::UnavailableOffline:
                return false;
            default:
                return true;
        }
    }

    /** @brief The effecter this trigger writes to. */
    NumericEffecter& effecter;

    /** @brief Whether the PDR gave a selector range that can be enforced. */
    bool boundsUsable = false;
};

} // namespace platform_mc
} // namespace pldm
