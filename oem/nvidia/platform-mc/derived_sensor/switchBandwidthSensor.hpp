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

#include "libpldm/platform.h"
#include "libpldm/requester/pldm.h"

#include "common/types.hpp"
#include "platform-mc/oem_base.hpp"

#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/Switch/server.hpp>
#include <tal.hpp>

namespace pldm
{
namespace platform_mc
{
namespace oem_nvidia
{

using namespace std::chrono;
using namespace sdbusplus;
using namespace pdr;

using AssociationDefinitionsInft = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Association::server::Definitions>;
using SwitchIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Inventory::Item::server::Switch>;

/**
 * @brief SwitchBandwidthSensor
 *
 * This handles sensors which are not PDR driven rather are based on combination
 * existing sensors. This class handles sensor reading updated by sensor manager
 * and export status to D-Bus interface.
 */
class SwitchBandwidthSensor
{
  public:
    SwitchBandwidthSensor(
        const tid_t tid, std::string sName, std::string& switchType,
        std::vector<std::string>& switchProtocols,
        const std::vector<dbus::PathAssociation>& associations);
    ~SwitchBandwidthSensor(){};

    void setDefaultValue();
    void updateOnSharedMemory();
    void updateCurrentBandwidth(double oldValue, double newValue);
    void updateMaxBandwidth(double value);
    void addAssociatedSensorID(uint16_t id);
    std::string getSensorName();

    tid_t tid;
    std::string path;

    std::unique_ptr<SwitchIntf> switchIntf = nullptr;
    std::unique_ptr<AssociationDefinitionsInft> associationDefinitionsIntf =
        nullptr;

  private:
    std::string sensorName;
    std::vector<uint16_t> associatedSensorID;
};

} // namespace oem_nvidia
} // namespace platform_mc
} // namespace pldm