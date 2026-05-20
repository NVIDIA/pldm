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

#include "platform-mc/state_effecter.hpp"
#include "platform-mc/state_set.hpp"

#include <xyz/openbmc_project/Control/Trigger/server.hpp>

namespace pldm
{
namespace platform_mc
{
namespace oem_nvidia
{

/** @brief NVIDIA OEM State Set ID for CPU Diagnostics Refresh
 *  Used to trigger collection of PCIe LTSSM History, Error Counters,
 *  and PCIe Telemetry from SatMC
 */
constexpr uint16_t PLDM_NVIDIA_OEM_STATE_SET_CPU_DIAG_REFRESH = 0x8002;

/** @brief State values for CPU Diagnostics Refresh */
enum pldm_state_set_cpu_diag_refresh_values
{
    /** Normal/idle state */
    PLDM_STATE_SET_CPU_DIAG_REFRESH_IDLE = 0,
    /** Trigger refresh/collection */
    PLDM_STATE_SET_CPU_DIAG_REFRESH_REQUESTED = 1
};

using TriggerIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Control::server::Trigger>;

/**
 * @brief D-Bus interface class for CPU Diagnostics Refresh trigger
 *
 * This interface exposes a Refresh property that when set to true,
 * triggers the PLDM effecter to send setStateEffecterStates command
 * to SatMC to collect diagnostic data (PCIe LTSSM History, Error
 * Counters, PCIe Telemetry).
 */
class CpuDiagnosticsRefreshStateIntf : public TriggerIntf
{
  public:
    CpuDiagnosticsRefreshStateIntf(sdbusplus::bus::bus& bus, const char* path,
                                   uint8_t compId) :
        TriggerIntf(bus, path), compId(compId)
    {}

    virtual void update(bool value)
    {
        TriggerIntf::refresh(value);
    }

  protected:
    uint8_t compId = 0;
};

/**
 * @brief Effecter interface for CPU Diagnostics Refresh
 *
 * When Refresh property is set to true, this triggers the PLDM
 * setStateEffecterStates command to SatMC.
 */
class CpuDiagnosticsRefreshEffecterIntf : public CpuDiagnosticsRefreshStateIntf
{
  public:
    CpuDiagnosticsRefreshEffecterIntf(sdbusplus::bus::bus& bus,
                                      const char* path, uint8_t compId,
                                      StateEffecter& effecter) :
        CpuDiagnosticsRefreshStateIntf(bus, path, compId), effecter(effecter)
    {}

    void update(bool value) override
    {
        TriggerIntf::refresh(value);
    }

    /**
     * @brief Override refresh property setter
     *
     * When set to true, sends setStateEffecterStates to trigger
     * diagnostic data collection on SatMC.
     */
    bool refresh(bool value) override
    {
        if (value)
        {
            // Trigger the effecter to request data collection
            stdexec::start_detached(
                effecter.setStateEffecterStates(
                    compId, PLDM_STATE_SET_CPU_DIAG_REFRESH_REQUESTED),
                exec::default_task_context<void>(exec::inline_scheduler{}));
        }
        // Auto-clear the refresh flag after triggering
        return TriggerIntf::refresh(false);
    }

    bool refresh() const override
    {
        return TriggerIntf::refresh();
    }

  private:
    StateEffecter& effecter;
};

/**
 * @brief State Set implementation for CPU Diagnostics Refresh effecter
 *
 * This state set handles the PLDM effecter for triggering CPU diagnostic
 * data collection (PCIe LTSSM History, Error Counters, PCIe Telemetry).
 * It exposes the xyz.openbmc_project.Control.Trigger interface with a
 * Refresh property.
 */
class StateSetCpuDiagnosticsRefresh : public StateSet
{
  public:
    StateSetCpuDiagnosticsRefresh(
        uint16_t stateSetId, uint8_t compId, std::string& objectPath,
        dbus::PathAssociation& stateAssociation, StateEffecter* effecter) :
        StateSet(stateSetId), compId(compId)
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
            ValueIntf = std::make_unique<CpuDiagnosticsRefreshEffecterIntf>(
                bus, objectPath.c_str(), compId, *effecter);
        }
        else
        {
            ValueIntf = std::make_unique<CpuDiagnosticsRefreshStateIntf>(
                bus, objectPath.c_str(), compId);
        }
        setDefaultValue();
    }

    ~StateSetCpuDiagnosticsRefresh() = default;

    void setValue(uint8_t value) override
    {
        switch (value)
        {
            case PLDM_STATE_SET_CPU_DIAG_REFRESH_REQUESTED:
                ValueIntf->update(true);
                break;
            default:
            case PLDM_STATE_SET_CPU_DIAG_REFRESH_IDLE:
                ValueIntf->update(false);
                break;
        }
    }

    void setDefaultValue() override
    {
        ValueIntf->update(false);
    }

    uint8_t getValue() override
    {
        if (ValueIntf->refresh())
        {
            return PLDM_STATE_SET_CPU_DIAG_REFRESH_REQUESTED;
        }
        return PLDM_STATE_SET_CPU_DIAG_REFRESH_IDLE;
    }

    /** @brief Not used - this effecter is trigger-only, events come via
     *  separate PLDM OEM event classes (0xF0, 0xF1, 0xF2). Returns a
     *  registered MessageId placeholder so that, if the override is ever
     *  reached, bmcweb's registry lookup succeeds instead of rendering a
     *  blank LogEntry.
     */
    std::tuple<std::string, std::string, Level, std::string, std::string>
        getEventData([[maybe_unused]] utils::SensorEventInfo* sensorEventInfo)
            const override
    {
        return {std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                std::string("Idle"), Level::Informational, "", ""};
    }

    std::string getStringStateType() const override
    {
        return std::string("CpuDiagnosticsRefresh");
    }

  private:
    std::unique_ptr<CpuDiagnosticsRefreshStateIntf> ValueIntf = nullptr;
    [[maybe_unused]] uint8_t compId = 0;
};

} // namespace oem_nvidia
} // namespace platform_mc
} // namespace pldm
