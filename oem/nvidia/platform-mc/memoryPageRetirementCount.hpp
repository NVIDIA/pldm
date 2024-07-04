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
#include "platform-mc/numeric_sensor.hpp"
#include "platform-mc/oem_base.hpp"

#include <com/nvidia/MemoryPageRetirementCount/server.hpp>
#include <sdbusplus/server/object.hpp>

#include <cmath>

namespace pldm
{
namespace platform_mc
{

using namespace sdbusplus;
using MemoryPageRetirementCountInft = sdbusplus::server::object_t<
    sdbusplus::com::nvidia::server::MemoryPageRetirementCount>;

class OemMemoryPageRetirementCountInft :
    public OemIntf,
    MemoryPageRetirementCountInft
{
  public:
    /** @brief Constructor to put object onto bus at a dbus path.
     *  @param[in] bus - Bus to attach to.
     *  @param[in] path - Path to attach at.
     */
    OemMemoryPageRetirementCountInft(std::shared_ptr<NumericSensor> sensor,
                                     bus::bus& bus, const char* path) :
        MemoryPageRetirementCountInft(bus, path),
        sensor(*sensor)
    {}

    virtual ~OemMemoryPageRetirementCountInft() = default;

    uint32_t memoryPageRetirementCount() const override
    {
        auto value = sensor.getReading();
        if (std::isnan(value))
        {
            return 0;
        }
        return value;
    }

  private:
    NumericSensor& sensor;
};

} // namespace platform_mc
} // namespace pldm
