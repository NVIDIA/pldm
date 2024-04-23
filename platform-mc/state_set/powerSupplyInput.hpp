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

#include "../state_set.hpp"

#include <xyz/openbmc_project/State/Decorator/PowerSystemInputs/server.hpp>

namespace pldm
{
namespace platform_mc
{

using PowerSupplyValueIntf =
    sdbusplus::server::object_t<sdbusplus::xyz::openbmc_project::State::
                                    Decorator::server::PowerSystemInputs>;
using PowerSupplyInputStatus = sdbusplus::xyz::openbmc_project::State::
    Decorator::server::PowerSystemInputs::Status;

class StateSetPowerSupplyInput : public StateSet
{
  public:
    StateSetPowerSupplyInput(uint16_t stateSetId, uint8_t compId,
                             std::string& objectPath,
                             dbus::PathAssociation& stateAssociation) :
        StateSet(stateSetId),
        compId(compId)
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
            std::make_unique<PowerSupplyValueIntf>(bus, objectPath.c_str());
        setDefaultValue();
    }

    ~StateSetPowerSupplyInput() = default;

    void setValue(uint8_t value) override
    {
        switch (value)
        {
            case PLDM_STATESET_POWERSUPPLY_NORMAL:
                ValueIntf->status(PowerSupplyInputStatus::Good);
                break;
            case PLDM_STATESET_POWERSUPPLY_OUTOFRANGE:
                ValueIntf->status(PowerSupplyInputStatus::InputOutOfRange);
                break;
            default:
                ValueIntf->status(PowerSupplyInputStatus::Unknown);
                break;
        }
    }

    void setDefaultValue() override
    {
        ValueIntf->status(PowerSupplyInputStatus::Unknown);
    }

    std::tuple<std::string, std::string> getEventData() const override
    {
        if (ValueIntf->status() == PowerSupplyInputStatus::Good)
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                    std::string("Normal")};
        }
        else
        {
            return {
                std::string("ResourceEvent.1.0.ResourceStatusChangedWarning"),
                std::string("Current Input out of Range")};
        }
    }

    std::string getStringStateType() const override
    {
        return std::string("EDP Violation State");
    }

  private:
    std::unique_ptr<PowerSupplyValueIntf> ValueIntf = nullptr;
    uint8_t compId = 0;
};

} // namespace platform_mc
} // namespace pldm