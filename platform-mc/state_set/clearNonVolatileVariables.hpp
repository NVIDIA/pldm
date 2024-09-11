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

#include "../state_set.hpp"

#include <xyz/openbmc_project/Control/Boot/ClearNonVolatileVariables/server.hpp>

namespace pldm
{
namespace platform_mc
{

using ClearNonVolatileVariablesIntf =
    sdbusplus::server::object_t<sdbusplus::xyz::openbmc_project::Control::Boot::
                                    server::ClearNonVolatileVariables>;

class ClearNonVolatileVariablesStateIntf : public ClearNonVolatileVariablesIntf
{
  public:
    ClearNonVolatileVariablesStateIntf(bus::bus& bus, const char* path,
                                       uint8_t compId) :
        ClearNonVolatileVariablesIntf(bus, path),
        compId(compId)
    {}

    virtual void update(bool value)
    {
        ClearNonVolatileVariablesIntf::clear(value);
    }

  protected:
    uint8_t compId = 0;
};

class ClearNonVolatileVariablesEffecterIntf :
    public ClearNonVolatileVariablesStateIntf
{
  public:
    ClearNonVolatileVariablesEffecterIntf(bus::bus& bus, const char* path,
                                          uint8_t compId,
                                          StateEffecter& effecter) :
        ClearNonVolatileVariablesStateIntf(bus, path, compId),
        effecter(effecter)
    {}

    void update(bool value)
    {
        ClearNonVolatileVariablesIntf::clear(value);
    }

    bool clear(bool value) override
    {
        uint8_t requestState = 0;
        if (value)
        {
            requestState = PLDM_STATESET_BOOT_REQUEST_REQUESTED;
        }
        else
        {
            requestState = PLDM_STATESET_BOOT_REQUEST_NORMAL;
        }

        effecter.setStateEffecterStates(compId, requestState).detach();
        return value;
    }

    /** return cached value and send getStateEffecterStates command to terminus.
     * the present value will be updated to D-Bus once received response
     */
    bool clear() const override
    {
        return ClearNonVolatileVariablesIntf::clear();
    }

  private:
    StateEffecter& effecter;
};

class StateSetClearNonvolatileVariable : public StateSet
{
  public:
    StateSetClearNonvolatileVariable(uint16_t stateSetId, uint8_t compId,
                                     std::string& objectPath,
                                     dbus::PathAssociation& stateAssociation,
                                     StateEffecter* effecter) :
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

        if (effecter != nullptr)
        {
            ValueIntf = std::make_unique<ClearNonVolatileVariablesEffecterIntf>(
                bus, objectPath.c_str(), compId, *effecter);
        }
        else
        {
            ValueIntf = std::make_unique<ClearNonVolatileVariablesStateIntf>(
                bus, objectPath.c_str(), compId);
        }
        setDefaultValue();
    }

    ~StateSetClearNonvolatileVariable() = default;

    void setValue(uint8_t value) override
    {
        switch (value)
        {
            case PLDM_STATESET_BOOT_REQUEST_REQUESTED:
                ValueIntf->update(true);
                break;
            default:
            case PLDM_STATESET_BOOT_REQUEST_NORMAL:
                ValueIntf->update(false);
                break;
        }
    }

    void setDefaultValue() override
    {
        ValueIntf->update(false);
    }

    uint8_t getValue()
    {
        uint8_t state = 0;
        if (ValueIntf->clear())
        {
            state = PLDM_STATESET_BOOT_REQUEST_REQUESTED;
        }
        else
        {
            state = PLDM_STATESET_BOOT_REQUEST_NORMAL;
        }
        return state;
    }

    std::tuple<std::string, std::string> getEventData() const override
    {
        if (ValueIntf->clear())
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                    std::string("True")};
        }
        else
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                    std::string("False")};
        }
    }

    std::string getStringStateType() const override
    {
        return std::string("ClearNonvolatileVariable");
    }

  private:
    std::unique_ptr<ClearNonVolatileVariablesStateIntf> ValueIntf = nullptr;
    uint8_t compId = 0;
};

} // namespace platform_mc
} // namespace pldm