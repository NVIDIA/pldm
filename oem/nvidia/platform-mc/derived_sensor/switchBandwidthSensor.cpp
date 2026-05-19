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

#include "common/shared_mem_manager.hpp"
#include "common/types.hpp"
#include "common/utils.hpp"

#include <phosphor-logging/lg2.hpp>

#include <cmath>
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
    tid(tid), sensorName(sName)
{
    path = "/xyz/openbmc_project/sensor/PLDM_Id_" + std::to_string(tid) + "/" +
           sName;
    path = std::regex_replace(path, std::regex("[^a-zA-Z0-9_/]+"), "_");

    auto& bus = pldm::utils::DBusHandler::getBus();
    associationDefinitionsIntf =
        std::make_unique<AssociationDefinitionsInft>(bus, path.c_str());
    std::vector<std::tuple<std::string, std::string, std::string>>
        associationsList;
    associationsList.reserve(associations.size());
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
    supportedProtocol.reserve(switchProtocols.size());
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
    // Reject values that are non-finite, negative, or exceeds calculated max
    // bandwidth. This catches both NaN sentinels and finite garbage from
    // pre-poll reads on PLDM_SENSOR_UNIT_BITS sensors. When maxBandwidth()
    // is 0 (not yet accumulated), the upper-bound check is skipped and
    // isfinite + non-negative alone applies.
    const double switchMax = switchIntf->maxBandwidth();
    auto isValid = [switchMax](double v) -> bool {
        return std::isfinite(v) && v >= 0.0 &&
               (switchMax <= 0.0 || v <= switchMax);
    };

    // In the event that newValue is not valid, the currentBandwidth
    // drops reading for the associated sensor. If oldValue is not valid,
    // we had not added the reading to the currentBandwidth in the previous
    // update,
    //  proceed with the new value.
    // If both oldValue and newValue are not valid, the currentBandwidth
    // is not updated.
    auto curBandwidthOnSwitch = switchIntf->currentBandwidth();
    if (isValid(oldValue))
    {
        curBandwidthOnSwitch -= oldValue;
    }
    if (isValid(newValue))
    {
        curBandwidthOnSwitch += newValue;
    }
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
    std::vector<uint8_t> rawSmbpbiData = {};
    auto ifaceName = std::string(switchIntf->interface);

    DbusVariantType variantCB{switchIntf->currentBandwidth()};
    std::string propertyName = "CurrentBandwidth";
    pldm::shmem_utils::SharedMemoryManager::cacheTALData(
        path, ifaceName, propertyName, rawSmbpbiData, variantCB);

    DbusVariantType variantMB{switchIntf->maxBandwidth()};
    propertyName = "MaxBandwidth";
    pldm::shmem_utils::SharedMemoryManager::cacheTALData(
        path, ifaceName, propertyName, rawSmbpbiData, variantMB);
}

} // namespace oem_nvidia
} // namespace platform_mc
} // namespace pldm
