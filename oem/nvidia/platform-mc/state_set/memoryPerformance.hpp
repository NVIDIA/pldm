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

#include "platform-mc/state_set.hpp"

#include <com/nvidia/MemoryPerformance/server.hpp>

#ifdef OEM_NVIDIA
#include <tal.hpp>
#endif

namespace pldm
{
namespace platform_mc
{

using MemoryPerformanceIntf = sdbusplus::server::object_t<
    sdbusplus::com::nvidia::server::MemoryPerformance>;

using MemoryPerformanceStates =
    sdbusplus::com::nvidia::server::MemoryPerformance::PerformanceStates;

class StateSetMemoryPerformance : public StateSet
{
  private:
    std::string objPath;

  public:
    StateSetMemoryPerformance(uint16_t stateSetId, uint8_t compId,
                              std::string& objectPath,
                              dbus::PathAssociation& stateAssociation) :
        StateSet(stateSetId),
        objPath(objectPath), compId(compId)
    {
        auto& bus = pldm::utils::DBusHandler::getBus();
        associationDefinitionsIntf =
            std::make_unique<AssociationDefinitionsInft>(bus,
                                                         objectPath.c_str());
        associationDefinitionsIntf->associations(
            {{stateAssociation.forward.c_str(),
              stateAssociation.reverse.c_str(),
              stateAssociation.path.c_str()}});
        ValueIntf =
            std::make_unique<MemoryPerformanceIntf>(bus, objectPath.c_str());
        setDefaultValue();
    }

    ~StateSetMemoryPerformance() = default;

#ifdef OEM_NVIDIA
    void updateShmemReading(const std::string& propName)
    {
        std::string propertyName = propName;
        std::string ifaceName = ValueIntf->interface;
        uint16_t retCode = 0;
        std::vector<uint8_t> rawPropValue = {};
        uint64_t steadyTimeStamp = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());

        DbusVariantType propValue{
            MemoryPerformanceIntf::convertPerformanceStatesToString(
                ValueIntf->value())};

        std::string endpoint{};
        auto definitions = associationDefinitionsIntf->associations();
        for (const auto& assoc : definitions)
        {
            std::string forward{std::get<0>(assoc)};
            std::string reverse{std::get<1>(assoc)};
            if (forward == "memory" && reverse == "all_states")
            {
                endpoint = std::get<2>(assoc);
                if (endpoint.size() > 0)
                {
                    tal::TelemetryAggregator::updateTelemetry(
                        objPath, ifaceName, propertyName, rawPropValue,
                        steadyTimeStamp, retCode, propValue, endpoint);
                }
            }
        }
    }
#endif

    void setValue(uint8_t value) override
    {
        switch (value)
        {
            case PLDM_STATESET_PERFORMANCE_NORMAL:
                ValueIntf->value(MemoryPerformanceStates::Normal);
                break;
            case PLDM_STATESET_PERFORMANCE_THROTTLED:
                ValueIntf->value(MemoryPerformanceStates::Throttled);
                break;
            default:
                ValueIntf->value(MemoryPerformanceStates::Unknown);
                break;
        }
#ifdef OEM_NVIDIA
        updateShmemReading("Value");
#endif
    }

    void setDefaultValue() override
    {
        ValueIntf->value(MemoryPerformanceStates::Unknown);
    }

    std::tuple<std::string, std::string, Level> getEventData() const override
    {
        if (ValueIntf->value() == MemoryPerformanceStates::Normal)
        {
            return {std::string("ResourceEvent.1.0.ResourceErrorsCorrected"),
                    std::string("Normal"), Level::Informational};
        }
        else
        {
            std::string arg = "PerformanceDegraded due to high temperature";
            return {std::string("ResourceEvent.1.0.ResourceErrorsDetected"),
                    arg, Level::Error};
        }
    }

    std::string getStringStateType() const override
    {
        return std::string("Performance");
    }

  private:
    std::unique_ptr<MemoryPerformanceIntf> ValueIntf = nullptr;
    uint8_t compId = 0;
};

} // namespace platform_mc
} // namespace pldm
