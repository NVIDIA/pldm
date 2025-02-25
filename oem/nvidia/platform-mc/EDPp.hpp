/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2025 NVIDIA CORPORATION &
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
#include "platform-mc/errors.hpp"
#include "platform-mc/numeric_effecter_base_unit.hpp"
#include "platform-mc/oem_base.hpp"

#include <com/nvidia/Edpp/server.hpp>
#include <sdbusplus/server/object.hpp>

namespace pldm
{
namespace platform_mc
{

namespace nvidia
{

using namespace sdbusplus;
using EdppPdiIntf =
    sdbusplus::server::object_t<sdbusplus::com::nvidia::server::Edpp>;

class EDPpIntf : public NumericEffecterBaseUnit, public EdppPdiIntf
{
  private:
    bool persistent = false;

  public:
    EDPpIntf(NumericEffecter& effecter, bus::bus& bus, const char* path,
             bool persistence) :
        NumericEffecterBaseUnit(effecter),
        EdppPdiIntf(bus, path), persistent(persistence)
    {}

    void pdrMaxSettable(double maxValue) override
    {
        EdppPdiIntf::allowableMax(static_cast<size_t>(std::round(maxValue)));
    }

    void pdrMinSettable(double minValue) override
    {
        EdppPdiIntf::allowableMin(static_cast<size_t>(std::round(minValue)));
    }

    double pdrMaxSettable() override
    {
        return static_cast<double>(EdppPdiIntf::allowableMax());
    }

    double pdrMinSettable() override
    {
        return static_cast<double>(EdppPdiIntf::allowableMin());
    }

    void handleGetNumericEffecterValue(
        pldm_effecter_oper_state effecterOperState, double pendingValue,
        double presentValue) override
    {
        size_t value;
        bool enabled;
        switch (effecterOperState)
        {
            case EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING:
                value = static_cast<size_t>(std::round(pendingValue));
                enabled = true;
                break;
            case EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING:
                value = static_cast<size_t>(std::round(presentValue));
                enabled = true;
                break;
            default:
                enabled = false;
                break;
        }
        if (enabled)
        {
            EdppPdiIntf::setPoint(std::make_tuple(value, persistent));
        }
    }

    // the size_t setPoint is in unit
    std::tuple<size_t, bool> setPoint(std::tuple<size_t, bool> value) override
    {
        size_t setpoint = std::get<0>(value);

        if (setpoint > EdppPdiIntf::allowableMax() ||
            setpoint < EdppPdiIntf::allowableMin())
        {
            throw errors::InvalidArgument("setPoint", "Out of range");
        }

        // write to the CPU
        effecter.setNumericEffecterValue(effecter.baseToRaw(setpoint)).detach();
        // update the PDI value
        return EdppPdiIntf::setPoint(value, persistent);
    }

    void reset() override
    {
        EdppPdiIntf::setPoint(
            std::make_tuple(EdppPdiIntf::allowableMax(), persistent));
    }
};

} // namespace nvidia
} // namespace platform_mc
} // namespace pldm