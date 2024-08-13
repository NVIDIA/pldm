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

#include "config.h"

#include "libpldm/state_set_oem_nvidia.h"

#include "common/types.hpp"
#include "platform-mc/state_set.hpp"

#include <xyz/openbmc_project/Inventory/Decorator/Instance/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Endpoint/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Port/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/PortInfo/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/PortState/server.hpp>
#include <xyz/openbmc_project/State/Decorator/SecureState/server.hpp>

#include <filesystem>

#ifdef OEM_NVIDIA
#include <tal.hpp>
#endif

namespace pldm
{
namespace platform_mc
{
namespace oem_nvidia
{

using PortIntf = sdbusplus::server::object_t<
    sdbusplus::server::xyz::openbmc_project::inventory::item::Port>;
using PortInfoIntf = sdbusplus::server::object_t<
    sdbusplus::server::xyz::openbmc_project::inventory::decorator::PortInfo>;
using PortStateIntf = sdbusplus::server::object_t<
    sdbusplus::server::xyz::openbmc_project::inventory::decorator::PortState>;

using PortType =
    sdbusplus::server::xyz::openbmc_project::inventory::decorator::PortInfo::PortType;
using PortProtocol =
    sdbusplus::server::xyz::openbmc_project::inventory::decorator::PortInfo::PortProtocol;
using PortLinkStates =
    sdbusplus::server::xyz::openbmc_project::inventory::decorator::PortState::LinkStates;
using PortLinkStatus =
    sdbusplus::server::xyz::openbmc_project::inventory::decorator::PortState::LinkStatusType;

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
                   dbus::PathAssociation& stateAssociation) :
        StateSet(stateSetId), objPath(objectPath)
    {
        auto& bus = pldm::utils::DBusHandler::getBus();
        associationDefinitionsIntf =
            std::make_unique<AssociationDefinitionsInft>(bus,
                                                         objectPath.c_str());
        associationDefinitionsIntf->associations(
            {{stateAssociation.forward.c_str(),
              stateAssociation.reverse.c_str(),
              stateAssociation.path.c_str()}});
        ValuePortIntf = std::make_unique<PortIntf>(bus, objectPath.c_str());
        ValuePortInfoIntf = std::make_unique<PortInfoIntf>(bus, objectPath.c_str());
        ValuePortStateIntf = std::make_unique<PortStateIntf>(bus, objectPath.c_str());

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

        DbusVariantType propValue{PortStateIntf::convertLinkStatesToString(
            ValuePortStateIntf->linkState())};

        std::string endpoint{};
        auto definitions = associationDefinitionsIntf->associations();
        for (const auto& assoc : definitions)
        {
            std::string forward{std::get<0>(assoc)};
            std::string reverse{std::get<1>(assoc)};
            if (forward == "chassis" && reverse == "all_states")
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
            case PLDM_STATE_SET_NVLINK_INACTIVE:
                ValuePortStateIntf->linkState(PortLinkStates::Disabled);
                ValuePortStateIntf->linkStatus(PortLinkStatus::LinkDown);
                break;
            case PLDM_STATE_SET_NVLINK_ACTIVE:
                ValuePortStateIntf->linkState(PortLinkStates::Enabled);
                ValuePortStateIntf->linkStatus(PortLinkStatus::LinkUp);
                break;
            case PLDM_STATE_SET_NVLINK_ERROR:
                ValuePortStateIntf->linkState(PortLinkStates::Error);
                ValuePortStateIntf->linkStatus(PortLinkStatus::NoLink);
                break;
            default:
                ValuePortStateIntf->linkState(PortLinkStates::Unknown);
                ValuePortStateIntf->linkStatus(PortLinkStatus::NoLink);
                break;
        }
#ifdef OEM_NVIDIA
        updateShmemReading("LinkState");
#endif
    }

    void setDefaultValue() override
    {
        ValuePortInfoIntf->type(PortType::BidirectionalPort);
        ValuePortInfoIntf->protocol(PortProtocol::NVLink);
        ValuePortStateIntf->linkState(PortLinkStates::Unknown);
        ValuePortStateIntf->linkStatus(PortLinkStatus::NoLink);
    }

    std::tuple<std::string, std::string> getEventData() const override
    {
        if (ValuePortStateIntf->linkStatus() == PortLinkStatus::LinkUp)
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChanged"),
                    std::string("LinkUp")};
        }
        else if (ValuePortStateIntf->linkStatus() == PortLinkStatus::LinkDown)
        {
            return {
                std::string("ResourceEvent.1.0.ResourceStatusChangedWarning"),
                std::string("LinkDown")};
        }
        else if (ValuePortStateIntf->linkState() == PortLinkStates::Error)
        {
            return {
                std::string("ResourceEvent.1.0.ResourceStatusChangedCritical"),
                std::string("Error")};
        }
        else
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChanged"),
                    std::string("Unknown")};
        }
    }

    std::string getStringStateType() const override
    {
        return std::string("NVLink");
    }

    virtual void
        setAssociation(std::vector<dbus::PathAssociation>& stateAssociations)
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

#ifdef NVLINK_C2C_FABRIC_OBJECT
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
