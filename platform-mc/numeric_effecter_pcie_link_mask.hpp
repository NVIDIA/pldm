/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
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
#include "platform-mc/numeric_effecter_base_unit.hpp"

#include <com/nvidia/PCIe/LinkEnableMask/server.hpp>
#include <exec/start_detached.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/Common/Device/error.hpp>
#include <xyz/openbmc_project/Common/error.hpp>

namespace pldm
{
namespace platform_mc
{
using namespace sdbusplus;
using LinkEnableMaskIntf = sdbusplus::server::object_t<
    sdbusplus::com::nvidia::PCIe::server::LinkEnableMask>;

/**
 * @brief NumericEffecterPcieLinkMask
 *
 * Unit interface for the SatMC per-CPU-package PCIe root-port link enable
 * mask effecter (base unit Bits on a PCI Express Bus entity, aux name
 * PCIeRPLinkCtrl). Publishes com.nvidia.PCIe.LinkEnableMask. The mask is a
 * bitmask with up to 48 significant bits, which is below 2^53 and so
 * round-trips exactly through the generic double-based value plumbing.
 *
 * Writes are gated on the effecter operational state: SatMC accepts
 * SetNumericEffecterValue only while the state is enabled (the UEFI
 * polling window is open).
 */
class NumericEffecterPcieLinkMask :
    public NumericEffecterBaseUnit,
    public LinkEnableMaskIntf
{
  public:
    /** @brief Constructor to put object onto bus at a dbus path.
     *  @param[in] effecter - Reference to the owning NumericEffecter.
     *  @param[in] bus - Bus to attach to.
     *  @param[in] path - Path to attach at.
     */
    NumericEffecterPcieLinkMask(NumericEffecter& effecter,
                                sdbusplus::bus_t& bus, const char* path) :
        NumericEffecterBaseUnit(effecter), LinkEnableMaskIntf(bus, path)
    {}

    void pdrMaxSettable(double value) override
    {
        NumericEffecterBaseUnit::pdrMaxSettable(value);
        // SupportedMask is populated once from the PDR maxSettable value
        // (0x0000FFFFFFFFFFFF for 48 root ports). The value is below 2^53
        // so the double handed in by the generic PDR plumbing is exact.
        LinkEnableMaskIntf::supportedMask(static_cast<uint64_t>(value));
    }

    void handleGetNumericEffecterValue(
        pldm_effecter_oper_state effecterOperState, double pendingValue,
        double presentValue) override
    {
        // The mask is a bitmask with up to 48 significant bits, below 2^53,
        // so it round-trips through double exactly.
        // GetNumericEffecterValue returns the last value known to the
        // platform and is valid in any operational state.
        switch (effecterOperState)
        {
            case EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING:
                LinkEnableMaskIntf::enableMask(
                    static_cast<uint64_t>(pendingValue), false);
                break;
            default:
                LinkEnableMaskIntf::enableMask(
                    static_cast<uint64_t>(presentValue), false);
                break;
        }
    }

    /** @brief Return cached mask value */
    uint64_t enableMask() const override
    {
        return LinkEnableMaskIntf::enableMask();
    }

    /** @brief Set a new mask on the terminus. The D-Bus value is updated
     *  from the GetNumericEffecterValue refresh that follows the set.
     */
    uint64_t enableMask(uint64_t value) override
    {
        if (value & ~LinkEnableMaskIntf::supportedMask())
        {
            lg2::warning(
                "EnableMask write rejected: invalid bits set, value={VALUE}, path={PATH}",
                "VALUE", lg2::hex, value, "PATH", effecter.path);
            throw sdbusplus::xyz::openbmc_project::Common::Error::
                InvalidArgument();
        }

        auto state = effecter.state();
        if (state == StateType::UnavailableOffline)
        {
            lg2::warning(
                "EnableMask write rejected: cannot update value at this system stage, path={PATH}",
                "PATH", effecter.path);
            throw sdbusplus::xyz::openbmc_project::Common::Error::Unavailable();
        }

        // DSP0248 ENABLED_UPDATEPENDING (mapped to Deferring by
        // updateValue) is still an enabled, writable state: a set is in
        // flight but the effecter keeps accepting writes.
        if (state != StateType::Enabled && state != StateType::Deferring)
        {
            lg2::warning(
                "EnableMask write rejected: effecter not enabled (effective window closed), path={PATH}",
                "PATH", effecter.path);
            throw sdbusplus::xyz::openbmc_project::Common::Error::NotAllowed();
        }

        exec::start_detached(stdexec::on(stdexec::inline_scheduler{},
                                         setEffecterValue(effecter, value)));
        return LinkEnableMaskIntf::enableMask();
    }

  private:
    /** @brief Send the 64-bit SetNumericEffecterValue and surface a
     *  non-SUCCESS completion code.
     *
     *  @param[in] effecter - the owning NumericEffecter
     *  @param[in] value - mask value to set
     */
    static exec::task<void> setEffecterValue(NumericEffecter& effecter,
                                             uint64_t value)
    {
        auto rc = co_await effecter.setNumericEffecterValue(
            static_cast<double>(value));
        if (rc != PLDM_SUCCESS)
        {
            lg2::error(
                "EnableMask write failed on terminus, cc={CC}, path={PATH}",
                "CC", rc, "PATH", effecter.path);
        }
        co_return;
    }
};

} // namespace platform_mc
} // namespace pldm
