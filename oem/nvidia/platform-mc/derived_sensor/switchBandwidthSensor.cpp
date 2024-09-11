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
#include "switchBandwidthSensor.hpp"

#include "libpldm/platform.h"

#include "common/types.hpp"
#include "common/utils.hpp"

#include <math.h>

#include <phosphor-logging/lg2.hpp>

#include <limits>
#include <regex>

namespace pldm
{
namespace platform_mc
{
namespace oem_nvidia
{

SwitchBandwidthSensor::SwitchBandwidthSensor(
    const tid_t tid, std::string sName, std::string& switchType,
    std::vector<std::string>& switchProtocols,
    const std::vector<dbus::PathAssociation>& associations) :
    tid(tid),
    sensorName(sName)
{
    path = "/xyz/openbmc_project/sensor/PLDM_Id_" + std::to_string(tid) + "/" +
           sName;
    path = std::regex_replace(path, std::regex("[^a-zA-Z0-9_/]+"), "_");

    auto& bus = pldm::utils::DBusHandler::getBus();
    associationDefinitionsIntf =
        std::make_unique<AssociationDefinitionsInft>(bus, path.c_str());
    std::vector<std::tuple<std::string, std::string, std::string>>
        associationsList;
    for (const auto& association : associations)
    {
        associationsList.emplace_back(association.forward, association.reverse,
                                      association.path);
    }
    associationDefinitionsIntf->associations(associationsList);

    switchIntf = std::make_unique<SwitchIntf>(bus, path.c_str());
    switchIntf->enabled(true);
    switchIntf->type(SwitchIntf::convertSwitchTypeFromString(switchType));
    std::vector<SwitchIntf::SwitchType> supportedProtocol;
    for (const auto& protocol : switchProtocols)
    {
        supportedProtocol.emplace_back(
            SwitchIntf::convertSwitchTypeFromString(protocol));
    }
    switchIntf->supportedProtocols(supportedProtocol);
    setDefaultValue();
    updateOnSharedMemory();
}

void SwitchBandwidthSensor::setDefaultValue()
{
    switchIntf->currentBandwidth(0.0);
    switchIntf->maxBandwidth(0.0);
}

void SwitchBandwidthSensor::updateCurrentBandwidth(double oldValue,
                                                   double newValue)
{
    auto curBandwidthOnSwitch = switchIntf->currentBandwidth();
    curBandwidthOnSwitch -= oldValue;
    curBandwidthOnSwitch += newValue;
    switchIntf->currentBandwidth(curBandwidthOnSwitch);
    updateOnSharedMemory();
}

void SwitchBandwidthSensor::updateMaxBandwidth(double value)
{
    auto maxBandwidthOnSwitch = switchIntf->maxBandwidth();
    maxBandwidthOnSwitch += value;
    switchIntf->maxBandwidth(maxBandwidthOnSwitch);
}

std::string SwitchBandwidthSensor::getSensorName()
{
    return sensorName;
}

void SwitchBandwidthSensor::addAssociatedSensorID(uint16_t id)
{
    associatedSensorID.push_back(id);
}

void SwitchBandwidthSensor::updateOnSharedMemory()
{
    uint64_t steadyTimeStamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    uint16_t retCode = 0;
    std::vector<uint8_t> rawSmbpbiData = {};
    auto ifaceName = std::string(switchIntf->interface);

    DbusVariantType variantCB{switchIntf->currentBandwidth()};
    std::string propertyName = "CurrentBandwidth";
    tal::TelemetryAggregator::updateTelemetry(path, ifaceName, propertyName,
                                              rawSmbpbiData, steadyTimeStamp,
                                              retCode, variantCB);

    DbusVariantType variantMB{switchIntf->maxBandwidth()};
    propertyName = "MaxBandwidth";
    tal::TelemetryAggregator::updateTelemetry(path, ifaceName, propertyName,
                                              rawSmbpbiData, steadyTimeStamp,
                                              retCode, variantMB);
}

} // namespace oem_nvidia
} // namespace platform_mc
} // namespace pldm