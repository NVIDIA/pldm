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

#include "libpldm/oem/nvidia/state_set_oem_nvidia.h"

#include "platform-mc/numeric_effecter.hpp"
#include "platform-mc/oem_base.hpp"
#include "platform-mc/state_effecter.hpp"
#include "platform-mc/state_sensor.hpp"

#include <xyz/openbmc_project/Common/error.hpp>
#include <xyz/openbmc_project/Control/Processor/RemoteDebug/server.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

namespace pldm
{
namespace platform_mc
{
namespace oem_nvidia
{

using RemoteDebugIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Control::Processor::server::RemoteDebug>;

using DebugState = sdbusplus::xyz::openbmc_project::Control::Processor::server::
    RemoteDebug::DebugState;

using DebugPolicy = sdbusplus::xyz::openbmc_project::Control::Processor::
    server::RemoteDebug::DebugPolicy;

class StateSetDebugState : public StateSet
{
  public:
    StateSetDebugState(uint16_t stateSetId, uint8_t compId,
                       [[maybe_unused]] std::string& objectPath,
                       [[maybe_unused]] dbus::PathAssociation& stateAssociation,
                       [[maybe_unused]] StateEffecter* effecter) :
        StateSet(stateSetId), compId(compId)
    {
        auto& bus = pldm::utils::DBusHandler::getBus();
        setDefaultValue();
        associationDefinitionsIntf =
            std::make_unique<AssociationDefinitionsInft>(bus,
                                                         objectPath.c_str());
    }

    ~StateSetDebugState() = default;

    void setValue(uint8_t v) override
    {
        value = v;
    }

    void setDefaultValue() override
    {
        value = PLDM_STATE_SET_DEBUG_STATE_OFFLINE;
    }

    uint8_t getValue() override
    {
        return value;
    }

    std::string getStringStateType() const override
    {
        return std::string("DebugState");
    }

    std::tuple<std::string, std::string, Level, std::string, std::string>
        getEventData([[maybe_unused]] utils::SensorEventInfo* sensorEventInfo)
            const override
    {
        if (value == PLDM_STATE_SET_DEBUG_STATE_DISABLED)
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                    std::string("Disable"), Level::Informational, "", ""};
        }
        else if (value == PLDM_STATE_SET_DEBUG_STATE_ENABLED)
        {
            return {
                std::string("ResourceEvent.1.0.ResourceStatusChangedWarning"),
                std::string("Enabled"), Level::Warning, "", ""};
        }
        else if (value == PLDM_STATE_SET_DEBUG_STATE_OFFLINE)
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                    std::string("Offline"), Level::Informational, "", ""};
        }
        else
        {
            return {
                std::string("ResourceEvent.1.0.ResourceStatusChangedWarning"),
                std::string("Unknown"), Level::Warning, "", ""};
        }
    }

  private:
    uint8_t value;
    [[maybe_unused]] uint8_t compId;
};

class OemRemoteDebugIntf : public OemIntf, public RemoteDebugIntf
{
    enum DebugCompId : uint8_t
    {
        JTAGEN_COMP_ID = 0,
        DEVEN_COMP_ID,
        SPNIDEN_COMP_ID,
        SPIDEN_COMP_ID,
        NIDEN_COMP_ID,
        DBGEN_COMP_ID,
        INVALID_COMP_ID = 255
    };

  public:
    /** Number of debug components in the remote debug composite effecter
     *  and sensor, indexed by DebugCompId. */
    static constexpr std::size_t debugCompCount = DBGEN_COMP_ID + 1;

    /** @brief Check that an effecter or sensor has one debug state set per
     *         debug component, so that the interface can index its state
     *         sets by DebugCompId
     *
     *  The PDR declares the state sets, and the effecter or sensor builds
     *  its state sets from them. A state effecter drops the state sets it
     *  does not support, and a state sensor keeps a nullptr for them. So
     *  check the built state sets, and check that none were dropped, so
     *  that their indices match the device's composite indices.
     *
     *  @param[in] declared - state sets declared by the PDR
     *  @param[in] built - state sets built by the effecter or sensor
     *
     *  @return true if the interface can use the state sets
     */
    static bool hasDebugStateSets(const std::vector<StateSetData>& declared,
                                  const StateSets& built)
    {
        if (declared.size() != debugCompCount || built.size() != debugCompCount)
        {
            return false;
        }

        return std::ranges::all_of(
            built, [](const std::shared_ptr<StateSet>& stateSet) {
                return stateSet != nullptr &&
                       stateSet->getStateSetId() ==
                           PLDM_NVIDIA_OEM_STATE_SET_DEBUG_STATE;
            });
    }

    OemRemoteDebugIntf(bus::bus& bus, const char* path,
                       std::shared_ptr<StateEffecter> stateEffecter,
                       std::shared_ptr<NumericEffecter> numericEffecter,
                       std::shared_ptr<StateSensor> stateSensor) :
        RemoteDebugIntf(bus, path), stateEffecter(*stateEffecter),
        numericEffecter(*numericEffecter), stateSensor(*stateSensor)
    {}

    DebugState jtagDebug() const override
    {
        return getDebugState(JTAGEN_COMP_ID);
    }

    DebugState deviceDebug() const override
    {
        return getDebugState(DEVEN_COMP_ID);
    }

    DebugState securePrivilegeNonInvasiveDebug() const override
    {
        return getDebugState(SPNIDEN_COMP_ID);
    }

    DebugState securePrivilegeInvasiveDebug() const override
    {
        return getDebugState(SPIDEN_COMP_ID);
    }

    DebugState nonInvasiveDebug() const override
    {
        return getDebugState(NIDEN_COMP_ID);
    }

    DebugState invasiveDebug() const override
    {
        return getDebugState(DBGEN_COMP_ID);
    }

    uint32_t timeout(uint32_t value, bool skipSignal) override
    {
        stdexec::start_detached(
            numericEffecter.setNumericEffecterValue(
                numericEffecter.baseToRaw(value)),
            exec::default_task_context<void>(exec::inline_scheduler{}));
        return RemoteDebugIntf::timeout(value, skipSignal);
    }

    uint32_t timeout() const override
    {
        stdexec::start_detached(
            numericEffecter.getNumericEffecterValue(),
            exec::default_task_context<void>(exec::inline_scheduler{}));
        return numericEffecter.rawToBase(numericEffecter.getValue());
    }

    void enable(std::vector<DebugPolicy> debugPolicy) override
    {
        uint8_t cmpEffCnt = stateEffecter.stateSets.size();
        std::vector<set_effecter_state_field> stateField(cmpEffCnt,
                                                         {PLDM_NO_CHANGE, 0});
        for (auto& v : debugPolicy)
        {
            auto compId = toCompId(v);
            if (compId == INVALID_COMP_ID)
            {
                throw sdbusplus::xyz::openbmc_project::Common::Error::
                    InvalidArgument();
            }

            if (!canSetDebugState(compId))
            {
                throw sdbusplus::xyz::openbmc_project::Common::Error::
                    NotAllowed();
            }
            stateField[compId] = {PLDM_REQUEST_SET,
                                  PLDM_STATE_SET_DEBUG_STATE_ENABLED};
        }

        stdexec::start_detached(
            stateEffecter.setStateEffecterStates(stateField),
            exec::default_task_context<void>(exec::inline_scheduler{}));
    }

    void disable(std::vector<DebugPolicy> debugPolicy) override
    {
        uint8_t cmpEffCnt = stateEffecter.stateSets.size();
        std::vector<set_effecter_state_field> stateField(cmpEffCnt,
                                                         {PLDM_NO_CHANGE, 0});

        for (auto& v : debugPolicy)
        {
            auto compId = toCompId(v);
            if (compId == INVALID_COMP_ID)
            {
                throw sdbusplus::xyz::openbmc_project::Common::Error::
                    InvalidArgument();
            }

            if (!canSetDebugState(compId))
            {
                throw sdbusplus::xyz::openbmc_project::Common::Error::
                    NotAllowed();
            }
            stateField[compId] = {PLDM_REQUEST_SET,
                                  PLDM_STATE_SET_DEBUG_STATE_DISABLED};
        }

        stdexec::start_detached(
            stateEffecter.setStateEffecterStates(stateField),
            exec::default_task_context<void>(exec::inline_scheduler{}));
    }

    uint8_t toCompId(DebugPolicy value)
    {
        switch (value)
        {
            case DebugPolicy::JtagDebug:
                return JTAGEN_COMP_ID;
            case DebugPolicy::DeviceDebug:
                return DEVEN_COMP_ID;
            case DebugPolicy::SecurePrivilegeNonInvasiveDebug:
                return SPNIDEN_COMP_ID;
            case DebugPolicy::SecurePrivilegeInvasiveDebug:
                return SPIDEN_COMP_ID;
            case DebugPolicy::NonInvasiveDebug:
                return NIDEN_COMP_ID;
            case DebugPolicy::InvasiveDebug:
                return DBGEN_COMP_ID;
            default:
                return INVALID_COMP_ID;
        }
    }

    DebugState toDebugState(uint8_t value) const
    {
        switch (value)
        {
            case PLDM_STATE_SET_DEBUG_STATE_ENABLED:
                return DebugState::Enabled;
            case PLDM_STATE_SET_DEBUG_STATE_DISABLED:
                return DebugState::Disabled;
            case PLDM_STATE_SET_DEBUG_STATE_OFFLINE:
                return DebugState::Offline;
            default:
                return DebugState::Unknown;
        }
    }

    DebugState getDebugState(uint8_t compositeId) const
    {
        StateSet* const effecterState =
            getStateSet(stateEffecter.stateSets, compositeId);
        StateSet* const sensorState =
            getStateSet(stateSensor.stateSets, compositeId);
        if (effecterState == nullptr || sensorState == nullptr)
        {
            return DebugState::Unknown;
        }

        if (effecterState->getOpState() ==
            EFFECTER_OPER_STATE_ENABLED_UPDATEPENDING)
        {
            return DebugState::Pending;
        }
        return toDebugState(sensorState->getValue());
    }

  private:
    /** @brief Get the state set of a debug component
     *
     *  @param[in] stateSets - state sets of the effecter or sensor
     *  @param[in] compId - debug component ID
     *
     *  @return the state set, or nullptr if there is none at compId
     */
    static StateSet* getStateSet(const StateSets& stateSets, std::size_t compId)
    {
        if (compId >= stateSets.size())
        {
            return nullptr;
        }
        return stateSets[compId].get();
    }

    /** @brief Check that Enable and Disable can set a debug component
     *
     *  Enable and Disable size their state fields by the effecter's state
     *  sets, so an effecter state set for compId also keeps compId within
     *  the state fields.
     *
     *  @param[in] compId - debug component ID
     *
     *  @return true if the effecter and the sensor have a state set for
     *          compId and the sensor does not read Offline
     */
    bool canSetDebugState(uint8_t compId) const
    {
        StateSet* const effecterState =
            getStateSet(stateEffecter.stateSets, compId);
        StateSet* const sensorState =
            getStateSet(stateSensor.stateSets, compId);
        return effecterState != nullptr && sensorState != nullptr &&
               sensorState->getValue() != PLDM_STATE_SET_DEBUG_STATE_OFFLINE;
    }

    StateEffecter& stateEffecter;
    NumericEffecter& numericEffecter;
    StateSensor& stateSensor;
};

} // namespace oem_nvidia
} // namespace platform_mc
} // namespace pldm
