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

#include "common/types.hpp"
#include "nvlinkPortMirror.hpp"
#include "platform-mc/state_set.hpp"

#include <xyz/openbmc_project/Inventory/Decorator/Instance/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Endpoint/server.hpp>
#include <xyz/openbmc_project/State/Decorator/SecureState/server.hpp>

#include <filesystem>
#include <optional>

#ifdef OEM_NVIDIA
#include <tal.hpp>
#endif

namespace pldm
{
namespace platform_mc
{
namespace oem_nvidia
{

using EndpointIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::Endpoint>;
using InstanceIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::Instance>;

class StateSetNvlink : public StateSet
{
  private:
    std::unique_ptr<PortIntf> ValuePortIntf = nullptr;
    std::unique_ptr<PortInfoIntf> ValuePortInfoIntf = nullptr;
    std::unique_ptr<PortStateIntf> ValuePortStateIntf = nullptr;
    std::unique_ptr<EndpointIntf> endpointIntf = nullptr;
    std::unique_ptr<AssociationDefinitionsInft>
        endpointAssociationDefinitionsIntf = nullptr;
    std::unique_ptr<InstanceIntf> endpointInstanceIntf = nullptr;
    std::string objPath;
    const StateSensor& stateSensor;

    // Last state value seen by setValue(). The device re-reports its current
    // state on every sensor poll, so this gates link-state logging to actual
    // transitions instead of once per poll.
    std::optional<uint8_t> prevValue;

    // WORKAROUND: NVLink per-partition Port mirroring
    NvlinkPortMirror portMirror;

    // C2CLink fabric prefix
    const std::string fabricsObjectPath =
        "/xyz/openbmc_project/inventory/system/fabrics/";
#ifdef PLATFORM_PREFIX
    const std::string c2clinkFabricPrefix = PLATFORM_PREFIX "_C2CLinkFabric_";
#else
    const std::string c2clinkFabricPrefix = "C2CLinkFabric_";
#endif

  public:
    StateSetNvlink(uint16_t stateSetId, std::string& objectPath,
                   dbus::PathAssociation& stateAssociation,
                   StateSensor& sensorRef) :
        StateSet(stateSetId), objPath(objectPath), stateSensor(sensorRef)
    {
        auto& bus = pldm::utils::DBusHandler::getBus();

        // WORKAROUND: NVLink per-partition Port mirroring
        const std::vector<std::string> portPaths =
            NvlinkPortMirror::computePaths(id, objectPath);
        objPath = portPaths.front();
        if (portPaths.size() > 1)
        {
            lg2::info("NVLink mirror: {SRC} -> {COUNT} Ports (primary {PRI})",
                      "SRC", objectPath, "COUNT", portPaths.size(), "PRI",
                      objPath);
        }

        associationDefinitionsIntf =
            std::make_unique<AssociationDefinitionsInft>(bus, objPath.c_str());
        associationDefinitionsIntf->associations(
            {{stateAssociation.forward.c_str(),
              stateAssociation.reverse.c_str(),
              stateAssociation.path.c_str()}});
        ValuePortIntf = std::make_unique<PortIntf>(bus, objPath.c_str());
        ValuePortInfoIntf =
            std::make_unique<PortInfoIntf>(bus, objPath.c_str());
        ValuePortStateIntf =
            std::make_unique<PortStateIntf>(bus, objPath.c_str());

        // WORKAROUND: create mirror Ports for paths[1..N-1] (no-op if N == 1).
        portMirror.init(bus, portPaths, stateAssociation);

        setDefaultValue();
    }

    ~StateSetNvlink() = default;

#ifdef OEM_NVIDIA
    void updateShmemReading(const std::string& propName)
    {
        std::string propertyName = propName;
        std::string ifaceName = ValuePortStateIntf->interface;
        uint16_t retCode = 0;
        std::vector<uint8_t> rawPropValue = {};
        uint64_t steadyTimeStamp = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());

        DbusVariantType propValue{};
        if (propName == "LinkStatus")
        {
            propValue = PortStateIntf::convertLinkStatusTypeToString(
                ValuePortStateIntf->linkStatus());
        }
        else if (propName == "LinkState")
        {
            propValue = PortStateIntf::convertLinkStatesToString(
                ValuePortStateIntf->linkState());
        }

        std::string endpoint{};
        auto definitions = associationDefinitionsIntf->associations();
        for (const auto& assoc : definitions)
        {
            std::string forward{std::get<0>(assoc)};
            std::string reverse{std::get<1>(assoc)};
            if (forward == "chassis" && reverse == "all_states")
            {
                endpoint = std::get<2>(assoc);
                if ((endpoint.size() > 0) &&
                    (!stateSensor.isDefaultInventoryAssociated()))
                {
                    tal::TelemetryAggregator::updateTelemetry(
                        objPath, ifaceName, propertyName, rawPropValue,
                        steadyTimeStamp, retCode, propValue, endpoint);
                    // WORKAROUND: publish the same reading on every mirror.
                    portMirror.publishShmem(ifaceName, propertyName, propValue,
                                            steadyTimeStamp, endpoint);
                }
            }
        }
    }
#endif
    void setValue(uint8_t value) override
    {
        // SatMC-emitted OEM state-set values (see libpldm
        // nvidia_oem_pldm_state_set_{nvlink,clink}_values):
        //
        //   NVLink (0x8000):
        //     PLDM_STATE_SET_NVLINK_INACTIVE (1)
        //     2..16 = training / PLL / BIST / calibration failure modes
        //     PLDM_STATE_SET_NVLINK_ACTIVE   (17)
        //
        //   CLink (0x8003):
        //     PLDM_STATE_SET_CLINK_INACTIVE       (1)
        //     PLDM_STATE_SET_CLINK_FAIL_INTR      (2)
        //     PLDM_STATE_SET_CLINK_FAIL_EXCEPTION (3)
        //     PLDM_STATE_SET_CLINK_ACTIVE         (4)
        //
        // Dispatch on the OEM state-set id because the two enums do not
        // share numeric values for ACTIVE; treating a CLink 4 as the
        // NVLink enum (or vice versa) would misrepresent link health.

        // Log link-state changes only on transition. The device re-reports
        // its current state on every sensor poll (~30s); a persistent failure
        // or invalid state would otherwise re-log an error/warning each poll
        // and flood the journal.
        const bool valueChanged = (prevValue != value);
        // Rendered only when we actually log (on a change); shown as "none"
        // before the first sample so the log reads as a transition.
        std::string prevValueStr;
        if (valueChanged)
        {
            prevValueStr = prevValue ? std::to_string(*prevValue) : "none";
        }
        prevValue = value;

        PortLinkStates newLinkState = PortLinkStates::Unknown;
        PortLinkStatus newLinkStatus = PortLinkStatus::NoLink;

        if (id == PLDM_NVIDIA_OEM_STATE_SET_CLINK)
        {
            switch (value)
            {
                case PLDM_STATE_SET_CLINK_INACTIVE:
                    newLinkState = PortLinkStates::Disabled;
                    newLinkStatus = PortLinkStatus::LinkDown;
                    break;
                case PLDM_STATE_SET_CLINK_ACTIVE:
                    newLinkState = PortLinkStates::Enabled;
                    newLinkStatus = PortLinkStatus::LinkUp;
                    break;
                case PLDM_STATE_SET_CLINK_FAIL_INTR:
                case PLDM_STATE_SET_CLINK_FAIL_EXCEPTION:
                    newLinkState = PortLinkStates::Error;
                    newLinkStatus = PortLinkStatus::LinkDown;
                    if (valueChanged)
                    {
                        lg2::error(
                            "Device reported CLink port state change {PREV} -> {VAL} for {PATH} - So setting LinkState as Error with Status as LinkDown",
                            "PREV", prevValueStr, "VAL", value, "PATH",
                            objPath);
                    }
                    break;
                default:
                    newLinkState = PortLinkStates::Unknown;
                    newLinkStatus = PortLinkStatus::NoLink;
                    if (valueChanged)
                    {
                        lg2::warning(
                            "Device reported invalid CLink state change {PREV} -> {VAL} for {PATH}; So setting default LinkState Unknown and LinkStatus as NoLink",
                            "PREV", prevValueStr, "VAL", value, "PATH",
                            objPath);
                    }
                    break;
            }
        }
        else if (id == PLDM_NVIDIA_OEM_STATE_SET_NVLINK)
        {
            // NVLink OEM state-set id (0x8000).
            switch (value)
            {
                case PLDM_STATE_SET_NVLINK_INACTIVE:
                    newLinkState = PortLinkStates::Disabled;
                    newLinkStatus = PortLinkStatus::LinkDown;
                    break;
                case PLDM_STATE_SET_NVLINK_ACTIVE:
                    newLinkState = PortLinkStates::Enabled;
                    newLinkStatus = PortLinkStatus::LinkUp;
                    break;
                // 15 distinct training/PLL/calibration failure codes;
                // collapse to a single Error state on the D-Bus interface.
                case PLDM_STATE_SET_NVLINK_INVALID_SPEEDO_CODE:
                case PLDM_STATE_SET_NVLINK_INVALID_FREQ:
                case PLDM_STATE_SET_NVLINK_INVALID_LINK:
                case PLDM_STATE_SET_NVLINK_C2C0_TR_FAIL:
                case PLDM_STATE_SET_NVLINK_C2C1_TR_FAIL:
                case PLDM_STATE_SET_NVLINK_1D_PR_FAIL:
                case PLDM_STATE_SET_NVLINK_2D_VOS_FAIL:
                case PLDM_STATE_SET_NVLINK_PR_REMOTE_FAIL:
                case PLDM_STATE_SET_NVLINK_IOBIST_FAIL:
                case PLDM_STATE_SET_NVLINK_C2C0_REFPLL_FAIL:
                case PLDM_STATE_SET_NVLINK_C2C1_REFPLL_FAIL:
                case PLDM_STATE_SET_NVLINK_C2C0_PLLCAL_FAIL:
                case PLDM_STATE_SET_NVLINK_C2C1_PLLCAL_FAIL:
                case PLDM_STATE_SET_NVLINK_C2C0_CLKDET_FAIL:
                case PLDM_STATE_SET_NVLINK_C2C1_CLKDET_FAIL:
                    newLinkState = PortLinkStates::Error;
                    newLinkStatus = PortLinkStatus::LinkDown;
                    if (valueChanged)
                    {
                        lg2::error(
                            "Device reported NVLink port state change {PREV} -> {VAL} for {PATH} - So setting LinkState as Error with Status as LinkDown",
                            "PREV", prevValueStr, "VAL", value, "PATH",
                            objPath);
                    }
                    break;
                default:
                    newLinkState = PortLinkStates::Unknown;
                    newLinkStatus = PortLinkStatus::NoLink;
                    if (valueChanged)
                    {
                        lg2::warning(
                            "Device reported invalid NVLink state change {PREV} -> {VAL} for {PATH}; So setting default LinkState Unknown and LinkStatus as NoLink",
                            "PREV", prevValueStr, "VAL", value, "PATH",
                            objPath);
                    }
                    break;
            }
        }
        else
        {
            lg2::warning(
                "Device reported unrecognized state set ID as {ID} for {PATH}",
                "ID", id, "PATH", objPath);
            return;
        }

        ValuePortStateIntf->linkState(newLinkState);
        ValuePortStateIntf->linkStatus(newLinkStatus);
        // WORKAROUND: mirror the partition state onto every sibling Port.
        portMirror.applyState(newLinkState, newLinkStatus);

#ifdef OEM_NVIDIA
        updateShmemReading("LinkState");
        updateShmemReading("LinkStatus");
#endif
    }

    void setDefaultValue() override
    {
        ValuePortInfoIntf->type(PortType::BidirectionalPort);
        ValuePortInfoIntf->protocol(PortProtocol::NVLink);
        ValuePortStateIntf->linkState(PortLinkStates::Unknown);
        ValuePortStateIntf->linkStatus(PortLinkStatus::NoLink);
        // WORKAROUND: apply the same defaults to every mirror Port.
        portMirror.applyDefaults(PortType::BidirectionalPort,
                                 PortProtocol::NVLink);
    }

    std::tuple<std::string, std::string, Level, std::string, std::string>
        getEventData([[maybe_unused]] utils::SensorEventInfo* sensorEventInfo)
            const override
    {
        if (ValuePortStateIntf->linkStatus() == PortLinkStatus::LinkUp)
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                    std::string("LinkUp"), Level::Informational, "", ""};
        }
        else if (ValuePortStateIntf->linkStatus() == PortLinkStatus::LinkDown)
        {
            if (sensorEventInfo)
            {
                std::string eventId = "";
                std::string impactedComponent =
                    sensorEventInfo->impactedComponent;
                auto it = sensorEventInfo->eventIdsMap.find("LinkDown");
                if (it != sensorEventInfo->eventIdsMap.end())
                {
                    eventId = it->second;
                }
                return {
                    std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                    std::string("LinkDown"), Level::Informational, eventId,
                    impactedComponent};
            }

            return {std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                    std::string("LinkDown"), Level::Informational, "", ""};
        }
        else if (ValuePortStateIntf->linkState() == PortLinkStates::Error)
        {
            return {
                std::string("ResourceEvent.1.0.ResourceStatusChangedCritical"),
                std::string("Error"), Level::Critical, "", ""};
        }
        else
        {
            return {
                std::string("ResourceEvent.1.0.ResourceStatusChangedWarning"),
                std::string("Unknown"), Level::Warning, "", ""};
        }
    }

    std::string getStringStateType() const override
    {
        if (id == PLDM_NVIDIA_OEM_STATE_SET_CLINK)
        {
            return std::string("CLink");
        }
        return std::string("NVLink");
    }

    void setAssociation(
        std::vector<dbus::PathAssociation>& stateAssociations) override
    {
        if (!associationDefinitionsIntf)
        {
            return;
        }

        if (stateAssociations.empty())
        {
            return;
        }
        auto stateAssociation = stateAssociations[0];

        try
        {
            std::string path =
                std::filesystem::path(stateAssociation.path).parent_path();
            auto getSubTreeResponse = utils::DBusHandler().getSubtree(
                path, 0, {"xyz.openbmc_project.Inventory.Item.Chassis"});

            std::string chassisCpuPath;
            if (getSubTreeResponse.size() != 0)
            {
                for (const auto& [objectPath, serviceMap] : getSubTreeResponse)
                {
                    chassisCpuPath = objectPath;
                }
            }

            // only look for Dbus path for System processors
            for (auto& assoc : stateAssociations)
            {
                // filter out the Dbus objects with Chassis interface which is
                // for Chassis CPU instead of system processors.
                if (assoc.path != chassisCpuPath)
                {
                    stateAssociation = assoc;
                    break;
                }
            }
        }
        catch (const sdbusplus::exception::SdBusError& e)
        {
            lg2::error("Failed to query Dbus for CPU: {ERROR}", "ERROR", e);
        }

        if (stateAssociation.path.empty())
        {
            return;
        }

        associationDefinitionsIntf->associations(
            {{stateAssociation.forward.c_str(),
              stateAssociation.reverse.c_str(),
              stateAssociation.path.c_str()}});
        // WORKAROUND: mirror the chassis association onto every sibling Port.
        portMirror.applyAssociation(stateAssociation);

#ifdef NVLINK_C2C_FABRIC_OBJECT
        // C2CLinkFabric_* endpoints model the CPU<->GPU NVLink fabric only.
        // CLink (CPU<->CPU) reuses this handler purely for Port creation and
        // does not participate in that fabric, so skip the endpoint setup.
        if (id == PLDM_NVIDIA_OEM_STATE_SET_CLINK)
        {
            return;
        }
        pldm::pdr::EntityInstance instanceNumber = 0;
        constexpr auto instanceInterface =
            "xyz.openbmc_project.Inventory.Decorator.Instance";
        constexpr auto instanceProperty = "InstanceNumber";

        try
        {
            // C2C NVLink instanceNumber should pick processorModule SMBIOS
            // instanceNumber instead of CPU SMBIOS instanceNumber.
            // CPU is counted per processorModule so all CPU
            // SMBIOS instanceNumber is 0 on CG4.
            // ProcessModule is counted per baseboard so its instanceNumber is
            // 0~3 on CG4.
            std::string parentPath =
                std::filesystem::path(stateAssociation.path).parent_path();
            instanceNumber = utils::DBusHandler().getDbusProperty<uint64_t>(
                parentPath.c_str(), instanceProperty, instanceInterface);
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to query instanceId Dbus, {ERROR}", "ERROR", e);
            return;
        }

        std::string endpointName =
            std::filesystem::path(stateAssociation.path).filename();
        std::string endpointObjectPath =
            fabricsObjectPath + c2clinkFabricPrefix +
            std::to_string(instanceNumber) + "/Endpoints/" + endpointName;

        auto& bus = pldm::utils::DBusHandler::getBus();
        try
        {
            if (!endpointIntf)
            {
                endpointIntf = std::make_unique<EndpointIntf>(
                    bus, endpointObjectPath.c_str());
            }

            if (!endpointInstanceIntf)
            {
                endpointInstanceIntf = std::make_unique<InstanceIntf>(
                    bus, endpointObjectPath.c_str());
                endpointInstanceIntf->instanceNumber(instanceNumber);
            }

            if (!endpointAssociationDefinitionsIntf)
            {
                endpointAssociationDefinitionsIntf =
                    std::make_unique<AssociationDefinitionsInft>(
                        bus, endpointObjectPath.c_str());
                endpointAssociationDefinitionsIntf->associations(
                    {{ "entity_link",
                       "",
                       stateAssociation.path.c_str() }});
            }
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to create PDIs at {OBJPATH}, {ERROR}", "OBJPATH",
                       endpointObjectPath, "ERROR", e);
            return;
        }
#endif
    }
};

} // namespace oem_nvidia
} // namespace platform_mc
} // namespace pldm
