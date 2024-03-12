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

#include "platform-mc/numeric_sensor.hpp"
#include "platform-mc/state_set.hpp"
#include "libpldm/entity.h"

#include <xyz/openbmc_project/Inventory/Item/Port/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/PortInfo/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/PortState/server.hpp>
#include <xyz/openbmc_project/State/Decorator/SecureState/server.hpp>

namespace pldm
{
namespace platform_mc
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

class StateSetEthernetPortLinkState : public StateSet
{
  public:
    StateSetEthernetPortLinkState(uint16_t stateSetId, uint8_t compId,
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
        ValuePortIntf = std::make_unique<PortIntf>(bus, objectPath.c_str());
        ValuePortInfoIntf = std::make_unique<PortInfoIntf>(bus, objectPath.c_str());
        ValuePortStateIntf = std::make_unique<PortStateIntf>(bus, objectPath.c_str());
        setDefaultValue();
    }

    ~StateSetEthernetPortLinkState() = default;

    void setValue(uint8_t value) override
    {
        switch (value)
        {
            case PLDM_STATESET_LINK_STATE_DISCONNECTED:
                ValuePortStateIntf->linkState(PortLinkStates::Disabled);
                ValuePortStateIntf->linkStatus(PortLinkStatus::LinkDown);
                break;
            case PLDM_STATESET_LINK_STATE_CONNECTED:
                ValuePortStateIntf->linkState(PortLinkStates::Enabled);
                ValuePortStateIntf->linkStatus(PortLinkStatus::LinkUp);
                break;
            default:
                ValuePortStateIntf->linkState(PortLinkStates::Unknown);
                ValuePortStateIntf->linkStatus(PortLinkStatus::NoLink);
                break;
        }

        if (linkSpeedSensor)
        {
            ValuePortInfoIntf->currentSpeed(linkSpeedSensor->getReading());
        }
    }

    void setDefaultValue() override
    {
        ValuePortInfoIntf->type(PortType::BidirectionalPort);
        ValuePortInfoIntf->protocol(PortProtocol::Ethernet);
        ValuePortStateIntf->linkState(PortLinkStates::Unknown);
        ValuePortStateIntf->linkStatus(PortLinkStatus::NoLink);
    }

    std::tuple<std::string, std::string> getEventData() const override
    {
        if (ValuePortStateIntf->linkStatus() == PortLinkStatus::LinkUp)
        {
            return {std::string("ResourceEvent.1.0.ResourceStatusChangedOK"),
                    std::string("Active")};
        }
        else if (ValuePortStateIntf->linkStatus() == PortLinkStatus::LinkDown)
        {
            return {
                std::string("ResourceEvent.1.0.ResourceStatusChangedWarning"),
                std::string("Inactive")};
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
        return std::string("Ethernet");
    }

    virtual void
        associateNumericSensor(const EntityInfo& entityInfo,
                               std::vector<std::shared_ptr<NumericSensor>>&
                                   numericSensors) override final
    {
        for (auto& sensor : numericSensors)
        {
            if (entityInfo == sensor->getEntityInfo())
            {
                auto& [containerID, entityType, entityInstance] = entityInfo;
                if (entityType == PLDM_ENTITY_ETHERNET &&
                    sensor->getBaseUnit() == PLDM_SENSOR_UNIT_BITS)
                {
                    linkSpeedSensor = sensor;
                    break;
                }
            }
        }
    }

    void setPortTypeValue(const PortType& type)
    {
        ValuePortInfoIntf->type(type);
    }

    void setMaxSpeedValue(const double value)
    {
        ValuePortInfoIntf->maxSpeed(value);
    }

    void addAssociation(const std::vector<dbus::PathAssociation>& associations)
    {
        std::vector<std::tuple<std::string, std::string, std::string>>
            associationsList;
        for (const auto& association : associations)
        {
            associationsList.emplace_back(
                association.forward, association.reverse, association.path);
        }
        associationDefinitionsIntf->associations(associationsList);
    }

  private:
    std::unique_ptr<PortIntf> ValuePortIntf = nullptr;
    std::unique_ptr<PortInfoIntf> ValuePortInfoIntf = nullptr;
    std::unique_ptr<PortStateIntf> ValuePortStateIntf = nullptr;
    std::unique_ptr<AssociationDefinitionsInft> associationDefinitionsIntf =
        nullptr;
    uint8_t compId = 0;
    std::shared_ptr<NumericSensor> linkSpeedSensor = nullptr;
};

} // namespace platform_mc
} // namespace pldm
