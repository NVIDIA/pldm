/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2026 NVIDIA CORPORATION &
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
#include "common/utils.hpp"
#include "platform-mc/errors.hpp"
#include "platform-mc/numeric_effecter.hpp"
#include "platform-mc/numeric_effecter_base_unit.hpp"

#include <exec/async_scope.hpp>
#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/Sensor/Value/server.hpp>

#include <limits>
#include <memory>
#include <optional>

namespace pldm
{
namespace platform_mc
{
using namespace sdbusplus;
using SensorUnit = sdbusplus::xyz::openbmc_project::Sensor::server::Value::Unit;
using EffecterValueIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Sensor::server::Value>;

/**
 * @brief NumericEffecterValueInft
 *
 * Generic numeric effecter D-Bus interface using
 * xyz.openbmc_project.Sensor.Value. Suitable for effecters with base units such
 * as DegreesC, Hertz, Minutes, etc. Exposes Value (read/write), MaxValue,
 * MinValue, and Unit on D-Bus.
 */
class NumericEffecterValueInft :
    public NumericEffecterBaseUnit,
    EffecterValueIntf
{
  public:
    static bool createMirrorValueIntf(NumericEffecter& effecter)
    {
        auto unit = getMirrorSensorUnit(effecter);
        if (!unit)
        {
            return false;
        }

        effecter.unitIntf =
            std::make_unique<NumericEffecterValueInft>(effecter, *unit);
        return true;
    }

    /** @brief Constructor to put object onto bus at a dbus path.
     *  @param[in] effecter - Reference to the owning NumericEffecter.
     *  @param[in] unit - The sensor unit type for this effecter.
     */
    NumericEffecterValueInft(NumericEffecter& effecter, SensorUnit unit) :
        NumericEffecterBaseUnit(effecter),
        EffecterValueIntf(pldm::utils::DBusHandler::getBus(),
                          effecter.path.c_str())
    {
        double maxVal = effecter.unitIntf->pdrMaxSettable();
        double minVal = effecter.unitIntf->pdrMinSettable();

        EffecterValueIntf::unit(unit);
        EffecterValueIntf::value(std::numeric_limits<double>::quiet_NaN());
        NumericEffecterBaseUnit::maxValue = maxVal;
        NumericEffecterBaseUnit::minValue = minVal;
        EffecterValueIntf::maxValue(maxVal);
        EffecterValueIntf::minValue(minVal);
    }

    ~NumericEffecterValueInft() override
    {
        stdexec::sync_wait(setValueScope.on_empty());
    }

    void pdrMaxSettable(double max) override
    {
        NumericEffecterBaseUnit::maxValue = max;
        EffecterValueIntf::maxValue(max);
    }

    void pdrMinSettable(double min) override
    {
        NumericEffecterBaseUnit::minValue = min;
        EffecterValueIntf::minValue(min);
    }

    void handleGetNumericEffecterValue(
        pldm_effecter_oper_state effecterOperState, double pendingValue,
        double presentValue) override
    {
        switch (effecterOperState)
        {
            case EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING:
                EffecterValueIntf::value(pendingValue);
                break;
            case EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING:
                EffecterValueIntf::value(presentValue);
                break;
            default:
                EffecterValueIntf::value(
                    std::numeric_limits<double>::quiet_NaN());
                break;
        }
    }

    void handleErrGetNumericEffecterValue() override
    {
        EffecterValueIntf::value(std::numeric_limits<double>::quiet_NaN());
    }

    /** @brief Return cached value */
    double value() const override
    {
        return EffecterValueIntf::value();
    }

    /** @brief Set a new value — triggers SetNumericEffecterValue to the
     *  terminus and returns the current cached value. The D-Bus value will
     *  be updated on the next getNumericEffecterValue() polling cycle.
     */
    double value(double newValue) override
    {
        if (newValue > NumericEffecterBaseUnit::maxValue ||
            newValue < NumericEffecterBaseUnit::minValue)
        {
            throw errors::InvalidArgument("Value", "Out of range");
        }

        setValueScope.spawn(
            effecter.setNumericEffecterValue(effecter.baseToRaw(newValue)) |
            stdexec::then([](int) {}));
        return EffecterValueIntf::value();
    }

  private:
    exec::async_scope setValueScope;

    static std::optional<SensorUnit> getMirrorSensorUnit(
        NumericEffecter& effecter)
    {
        switch (effecter.getBaseUnit())
        {
            case PLDM_SENSOR_UNIT_WATTS:
                return SensorUnit::Watts;
            case PLDM_SENSOR_UNIT_DEGRESS_C:
                return SensorUnit::DegreesC;
            default:
                return std::nullopt;
        }
    }
};

} // namespace platform_mc
} // namespace pldm
