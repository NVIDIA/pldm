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

#include <cmath>
#include <limits>

namespace pldm
{
namespace platform_mc
{

class NumericEffecter;

/**
 * @brief NumericEffecterBaseUnit
 *
 * This class provides common APIs for all numeric effecter DBus interfaces,
 * all APIs are virtual functions that can be overridden by individual
 * interfaces.
 */
class NumericEffecterBaseUnit
{
  public:
    constexpr static pldm_effecter_oper_state EFFECTER_OPER_NO_REQ =
        EFFECTER_OPER_STATE_STATUSUNKNOWN;

    NumericEffecterBaseUnit(NumericEffecter& effecter) : effecter(effecter)
    {}

    virtual ~NumericEffecterBaseUnit() = default;

    virtual void pdrMaxSettable([[maybe_unused]] double value)
    {
        maxValue = value;
    }

    virtual void pdrMinSettable([[maybe_unused]] double value)
    {
        minValue = value;
    }

    virtual double pdrMaxSettable()
    {
        return maxValue;
    }

    virtual double pdrMinSettable()
    {
        return minValue;
    }

    virtual void handleGetNumericEffecterValue(
        [[maybe_unused]] pldm_effecter_oper_state effecterOperState,
        [[maybe_unused]] double pendingValue,
        [[maybe_unused]] double presentValue)
    {}

    virtual void handleErrGetNumericEffecterValue()
    {
        handleGetNumericEffecterValue(EFFECTER_OPER_STATE_FAILED, 0.0, 0.0);
    }

  protected:
    /** @brief Reference to associated NumericEffecter */
    NumericEffecter& effecter;
    double maxValue;
    double minValue;
};

} // namespace platform_mc
} // namespace pldm
