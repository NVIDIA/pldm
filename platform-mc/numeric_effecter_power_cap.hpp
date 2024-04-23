/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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
#include "platform-mc/errors.hpp"

#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/Control/Power/Cap/server.hpp>

#include <limits>

namespace pldm
{
namespace platform_mc
{
using namespace sdbusplus;
using PowerCapInft = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Control::Power::server::Cap>;

class NumericEffecterWattInft : public NumericEffecterBaseUnit, PowerCapInft
{
  public:
    /** @brief Constructor to put object onto bus at a dbus path.
     *  @param[in] bus - Bus to attach to.
     *  @param[in] path - Path to attach at.
     */
    NumericEffecterWattInft(NumericEffecter& effecter, bus::bus& bus,
                            const char* path) :
        NumericEffecterBaseUnit(effecter),
        PowerCapInft(bus, path)
    {}

    void pdrMaxSettable(double maxValue) override
    {
        PowerCapInft::maxPowerCapValue(maxValue);
    }

    void pdrMinSettable(double minValue) override
    {
        PowerCapInft::minPowerCapValue(minValue);
        PowerCapInft::minSoftPowerCapValue(minValue);
    }

    void handleGetNumericEffecterValue(
        pldm_effecter_oper_state effecterOperState, double pendingValue,
        double presentValue) override
    {
        double value;
        bool enabled;
        switch (effecterOperState)
        {
            case EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING:
                value = pendingValue;
                enabled = true;
                break;
            case EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING:
                value = presentValue;
                enabled = true;
                break;
            default:
                enabled = false;
                break;
        }
        PowerCapInft::powerCapEnable(enabled, false);
        if (enabled)
        {
            PowerCapInft::powerCap(value, false);
        }
    }

    /** return cached value
     */
    uint32_t powerCap() const override
    {
        return PowerCapInft::powerCap();
    }

    /** set the new value to terminus, and update dbus in
     * handleGetNumericEffecterValue()
     */
    uint32_t powerCap(uint32_t value) override
    {
        if (value > PowerCapInft::maxPowerCapValue() ||
            value < PowerCapInft::minPowerCapValue())
        {
            throw errors::InvalidArgument("PowerCap",
                                          "Out of range");
        }

        double newValue = value;
        effecter.setNumericEffecterValue(effecter.baseToRaw(newValue)).detach();
        return PowerCapInft::powerCap();
    }

    /** set the new value to terminus, and update dbus in
     * handleGetNumericEffecterValue()
     */
    bool powerCapEnable(bool value) override
    {
        pldm_effecter_oper_state newState;
        if (value)
        {
            newState = EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING;
        }
        else
        {
            newState = EFFECTER_OPER_STATE_DISABLED;
        }
        effecter.setNumericEffecterEnable(newState).detach();
        return PowerCapInft::powerCapEnable();
    }
};

} // namespace platform_mc
} // namespace pldm
