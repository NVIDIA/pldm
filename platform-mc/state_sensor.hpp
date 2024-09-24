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

#include "config.h"

#include "libpldm/platform.h"
#include "libpldm/requester/pldm.h"

#include "common/types.hpp"
#include "state_set.hpp"

#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/Sensor/Threshold/Critical/server.hpp>
#include <xyz/openbmc_project/Sensor/Threshold/Warning/server.hpp>
#include <xyz/openbmc_project/Sensor/Value/server.hpp>
#include <xyz/openbmc_project/State/Decorator/Availability/server.hpp>
#include <xyz/openbmc_project/State/Decorator/OperationalStatus/server.hpp>

#include <vector>

namespace pldm
{
namespace platform_mc
{

using namespace pldm::pdr;
using namespace std::chrono;
using OperationalStatusIntf =
    sdbusplus::server::object_t<sdbusplus::xyz::openbmc_project::State::
                                    Decorator::server::OperationalStatus>;
using AvailabilityIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::State::Decorator::server::Availability>;

/**
 * @brief StateSensor
 *
 * This class handles sensor reading updated by sensor manager and export
 * status to D-Bus interface.
 */
class StateSensor
{
  public:
    StateSensor(const uint8_t tid, const bool sensorDisabled,
                const uint16_t sensorId, StateSetInfo sensorInfo,
                AuxiliaryNames* sensorNames, std::string& associationPath);
    ~StateSensor(){};

    /** @brief The function called by Sensor Manager to set sensor to
     * error status.
     */
    void handleErrGetSensorReading();

    void updateReading(bool available, bool functional, uint8_t compSensorIndex,
                       uint8_t value);

    /** @brief Get the ContainerID, EntityType, EntityInstance of the PLDM
     * Entity which the sensor belongs to
     *  @return EntityInfo - Entity ID
     */
    inline auto getEntityInfo()
    {
        return std::get<0>(sensorInfo);
    }

    /** @brief Updating the association to D-Bus interface
     *  @param[in] inventoryPath - inventory path of the entity
     */
    inline void setInventoryPaths(const std::vector<std::string>& inventoryPath)
    {
        for (auto& stateSet : stateSets)
        {
            if (stateSet == nullptr)
            {
                continue;
            }

            std::vector<dbus::PathAssociation> assocs;

            for (const auto& path : inventoryPath)
            {
                dbus::PathAssociation assoc = {"chassis", "all_states",
                                               path.c_str()};
                assocs.emplace_back(assoc);
            }
            stateSet->setAssociation(assocs);
        }
    }

    /** @brief associating numeric sensor to state set D-Bus interface
     *  @param[in] inventoryPath - inventory path of the entity
     */
    inline void associateNumericSensor(
        std::vector<std::shared_ptr<NumericSensor>>& numericSensors)
    {
        for (auto& stateSet : stateSets)
        {
            if (stateSet)
            {
                stateSet->associateNumericSensor(getEntityInfo(),
                                                 numericSensors);
            }
        }
    }

    void handleSensorEvent(uint8_t sensorOffset, uint8_t eventState);
    void createLogEntry(std::string& messageID, std::string& arg1,
                        std::string& arg2, std::string& resolution);

    /** @brief Terminus ID of the PLDM Terminus which the sensor belongs to
     */
    uint8_t tid;

    /** @brief Sensor ID */
    uint16_t sensorId;

    /** @brief  State Sensor Info */
    StateSetInfo sensorInfo;

    /** @brief flag to update the value once */
    bool needUpdate;

    /** @brief indicate the sensor updated asynchronously */
    bool async;

    /** @brief  getter of associationEntityId */
    std::string getAssociationEntityId()
    {
        return associationEntityId;
    }

    /** @brief  update sensorName to sensor PDIs*/
    void updateSensorNames(AuxiliaryNames& auxNames);

    StateSets stateSets;

    void setRefreshed(bool r)
    {
        refreshed = r;
    }

    inline bool isRefreshed()
    {
        return refreshed;
    }

    /** @brief  The time since last getStateSensorReadings command in usec */
    uint64_t lastUpdatedTimeStampInUsec;

    /** @brief  The refresh limit in usec */
    uint64_t refreshLimitInUsec = DEFAULT_RR_REFRESH_LIMIT_IN_MS * 1000;

    inline void setLastUpdatedTimeStamp(const uint64_t currentTimestampInUsec)
    {
        lastUpdatedTimeStampInUsec = currentTimestampInUsec;
    }

    inline bool needsUpdate(const uint64_t currentTimestampInUsec)
    {
        const uint64_t deltaInUsec =
            currentTimestampInUsec - lastUpdatedTimeStampInUsec;
        return (deltaInUsec > refreshLimitInUsec);
    }

  private:
    std::unique_ptr<AvailabilityIntf> availabilityIntf = nullptr;
    std::unique_ptr<OperationalStatusIntf> operationalStatusIntf = nullptr;
    std::string associationEntityId;
    std::string path;
    bool refreshed = false;
};
} // namespace platform_mc
} // namespace pldm
