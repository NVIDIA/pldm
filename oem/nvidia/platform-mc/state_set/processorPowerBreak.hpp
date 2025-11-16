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

#include "platform-mc/state_set.hpp"

#include <com/nvidia/ProcessorPowerBreak/server.hpp>

#ifdef OEM_NVIDIA
#include <tal.hpp>
#endif

#define STATE_SENSOR_ID_MASK 0x0FFF
#define STATE_SENSOR_CPU_POWER_BREAK 0x0240
#define STATE_SENSOR_CPU_PERFORMANCE 0x0210

namespace pldm
{
namespace platform_mc
{

using ProcessorPowerBreakIntf = sdbusplus::server::object_t<
    sdbusplus::com::nvidia::server::ProcessorPowerBreak>;

using PowerBreakStates =
    sdbusplus::com::nvidia::server::ProcessorPowerBreak::PowerBreakStates;

class StateSetProcessorPowerBreak : public StateSet
{
  private:
    std::string objPath;
    uint8_t compId = 0;
    const StateSensor& stateSensor;

  public:
    StateSetProcessorPowerBreak(
        uint16_t stateSetId, uint8_t compId, std::string& objectPath,
        dbus::PathAssociation& stateAssociation, StateSensor& sensorRef) :
        StateSet(stateSetId), objPath(objectPath), compId(compId),
        stateSensor(sensorRef)
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
            std::make_unique<ProcessorPowerBreakIntf>(bus, objectPath.c_str());
        setDefaultValue();
    }

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
            ProcessorPowerBreakIntf::convertPowerBreakStatesToString(
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
                if ((endpoint.size() > 0) &&
                    (!stateSensor.isDefaultInventoryAssociated()))
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
                ValueIntf->value(PowerBreakStates::Normal);
                break;
            case PLDM_STATESET_PERFORMANCE_THROTTLED:
                ValueIntf->value(PowerBreakStates::Throttled);
                break;
            default:
                ValueIntf->value(PowerBreakStates::Unknown);
                break;
        }
#ifdef OEM_NVIDIA
        updateShmemReading("Value");
#endif
    }

    void setDefaultValue() override
    {
        ValueIntf->value(PowerBreakStates::Unknown);
    }

    std::tuple<std::string, std::string, Level, std::string, std::string>
        getEventData([[maybe_unused]] utils::SensorEventInfo* sensorEventInfo)
            const override
    {
        if (ValueIntf->value() == PowerBreakStates::Normal)
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                    std::string("Normal"), Level::Informational, "", ""};
        }
        else
        {
            return {
                std::string("ResourceEvent.1.0.ResourceStatusChangedWarning"),
                std::string("Throttled"), Level::Informational, "", ""};
        }
    }

    std::string getStringStateType() const override
    {
        return std::string("PowerBreak");
    }

  private:
    std::unique_ptr<ProcessorPowerBreakIntf> ValueIntf = nullptr;
};

} // namespace platform_mc
} // namespace pldm
