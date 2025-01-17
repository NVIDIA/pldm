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

#include "libpldm/entity.h"

#include "platform-mc/numeric_sensor.hpp"
#include "platform-mc/state_set.hpp"

#ifdef OEM_NVIDIA
#include "oem/nvidia/platform-mc/derived_sensor/switchBandwidthSensor.hpp"
#endif

#include <tal.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/PortInfo/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/PortState/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Port/server.hpp>
#include <xyz/openbmc_project/State/Decorator/SecureState/server.hpp>

#include <regex>

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

using PortType = sdbusplus::server::xyz::openbmc_project::inventory::decorator::
    PortInfo::PortType;
using PortProtocol = sdbusplus::server::xyz::openbmc_project::inventory::
    decorator::PortInfo::PortProtocol;
using PortLinkStates = sdbusplus::server::xyz::openbmc_project::inventory::
    decorator::PortState::LinkStates;
using PortLinkStatus = sdbusplus::server::xyz::openbmc_project::inventory::
    decorator::PortState::LinkStatusType;

class StateSetEthernetPortLinkState : public StateSet
{
  public:
    StateSetEthernetPortLinkState(uint16_t stateSetId, uint8_t compId,
                                  std::string& objectPath,
                                  dbus::PathAssociation& stateAssociation) :
        StateSet(stateSetId),
        compId(compId), objectPath(objectPath)
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
        ValuePortInfoIntf =
            std::make_unique<PortInfoIntf>(bus, objectPath.c_str());
        ValuePortStateIntf =
            std::make_unique<PortStateIntf>(bus, objectPath.c_str());
        setDefaultValue();
    }

    ~StateSetEthernetPortLinkState() = default;

    void setValue(uint8_t value) override
    {
        presentState = value;
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
#ifdef OEM_NVIDIA
            auto oldValue = ValuePortInfoIntf->currentSpeed();
#endif
            // after unitmodifier the numeric sensor is in bps (bits per sec)
            // convert bps to Gbps
            auto sensorSpeedGbps =
                linkSpeedSensor->getReading() * pldm::utils::BPS_TO_GBPS;
            ValuePortInfoIntf->currentSpeed(sensorSpeedGbps);
#ifdef OEM_NVIDIA
            auto newValue = ValuePortInfoIntf->currentSpeed();
            if (switchBandwidthSensor && (oldValue != newValue))
            {
                switchBandwidthSensor->updateCurrentBandwidth(oldValue,
                                                              newValue);
            }
            updateSharedMemory();
#endif
        }
    }

    void setDefaultValue() override
    {
        ValuePortInfoIntf->type(PortType::BidirectionalPort);
        ValuePortInfoIntf->protocol(PortProtocol::Ethernet);
        ValuePortStateIntf->linkState(PortLinkStates::Unknown);
        ValuePortStateIntf->linkStatus(PortLinkStatus::NoLink);

        ValuePortInfoIntf->currentSpeed(0.0);
        ValuePortInfoIntf->maxSpeed(0.0);
    }

    std::tuple<std::string, std::string, Level> getEventData() const override
    {
        if (ValuePortStateIntf->linkStatus() == PortLinkStatus::LinkUp)
        {
            return {std::string("ResourceEvent.1.0.ResourceErrorsCorrected"),
                    std::string("LinkUp"), Level::Informational};
        }
        else if (ValuePortStateIntf->linkStatus() == PortLinkStatus::LinkDown)
        {
            return {std::string("ResourceEvent.1.0.ResourceErrorsDetected"),
                    std::string("LinkDown"), Level::Alert};
        }
        else if (ValuePortStateIntf->linkState() == PortLinkStates::Error)
        {
            return {std::string("ResourceEvent.1.0.ResourceErrorsDetected"),
                    std::string("Error"), Level::Error};
        }
        else
        {
            return {std::string("ResourceEvent.1.0.ResourceErrorsDetected"),
                    std::string("Unknown"), Level::Error};
        }
    }

    std::string getStringStateType() const override
    {
        return objectName;
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

    void setPortProtocolValue(const PortProtocol& protocol)
    {
        ValuePortInfoIntf->protocol(protocol);
    }

    void setMaxSpeedValue(const double value)
    {
        ValuePortInfoIntf->maxSpeed(value);
    }

#ifdef OEM_NVIDIA
    void associateDerivedSensor(
        std::shared_ptr<oem_nvidia::SwitchBandwidthSensor> sensor)
    {
        switchBandwidthSensor = sensor;
    }

    bool isDerivedSensorAssociated()
    {
        if (switchBandwidthSensor)
        {
            return true;
        }
        else
        {
            return false;
        }
    }

    void addSharedMemObjectPath(std::string objPath)
    {
        sharedMemObjectPath = objPath;
    }

    void updateSharedMemory()
    {
        // add values in tal
        uint64_t steadyTimeStamp = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        uint16_t retCode = 0;
        std::vector<uint8_t> rawSmbpbiData = {};
        auto ifaceName = std::string(ValuePortInfoIntf->interface);

        DbusVariantType variantCS{ValuePortInfoIntf->currentSpeed()};
        std::string propertyName = "CurrentSpeed";
        tal::TelemetryAggregator::updateTelemetry(
            sharedMemObjectPath, ifaceName, propertyName, rawSmbpbiData,
            steadyTimeStamp, retCode, variantCS);

        DbusVariantType variantMS{ValuePortInfoIntf->maxSpeed()};
        propertyName = "MaxSpeed";
        tal::TelemetryAggregator::updateTelemetry(
            sharedMemObjectPath, ifaceName, propertyName, rawSmbpbiData,
            steadyTimeStamp, retCode, variantMS);

        DbusVariantType variantLS{
            ValuePortStateIntf->convertLinkStatusTypeToString(
                ValuePortStateIntf->linkStatus())};
        ifaceName = std::string(ValuePortStateIntf->interface);
        propertyName = "LinkStatus";
        tal::TelemetryAggregator::updateTelemetry(
            sharedMemObjectPath, ifaceName, propertyName, rawSmbpbiData,
            steadyTimeStamp, retCode, variantLS);
    }
#endif

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

    virtual void updateSensorName([[maybe_unused]] std::string name) override
    {
        objectName = name;
        if (name == objectPath.filename())
        {
            return;
        }
        objectPath = objectPath.parent_path() / name;

        // update new object path to D-Bus
        auto& bus = pldm::utils::DBusHandler::getBus();
        auto path = std::regex_replace(objectPath.string(),
                                       std::regex("[^a-zA-Z0-9_/]+"), "_");
        if (associationDefinitionsIntf)
        {
            auto associations = associationDefinitionsIntf->associations();
            associationDefinitionsIntf =
                std::make_unique<AssociationDefinitionsInft>(bus, path.c_str());
            associationDefinitionsIntf->associations(associations);
        }

        if (ValuePortIntf)
        {
            ValuePortIntf = std::make_unique<PortIntf>(bus, path.c_str());
        }

        if (ValuePortInfoIntf)
        {
            ValuePortInfoIntf =
                std::make_unique<PortInfoIntf>(bus, path.c_str());
        }

        if (ValuePortStateIntf)
        {
            ValuePortStateIntf =
                std::make_unique<PortStateIntf>(bus, path.c_str());
        }
        setDefaultValue();
        setValue(presentState);
    }

  private:
    std::unique_ptr<PortIntf> ValuePortIntf = nullptr;
    std::unique_ptr<PortInfoIntf> ValuePortInfoIntf = nullptr;
    std::unique_ptr<PortStateIntf> ValuePortStateIntf = nullptr;
    std::unique_ptr<AssociationDefinitionsInft> associationDefinitionsIntf =
        nullptr;
    uint8_t compId = 0;
    std::shared_ptr<NumericSensor> linkSpeedSensor = nullptr;
#ifdef OEM_NVIDIA
    std::shared_ptr<oem_nvidia::SwitchBandwidthSensor> switchBandwidthSensor =
        nullptr;
    std::filesystem::path sharedMemObjectPath;
#endif
    std::filesystem::path objectPath;
    std::string objectName;
    uint8_t presentState;
};

} // namespace platform_mc
} // namespace pldm
