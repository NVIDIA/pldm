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

#include "libpldm/platform.h"
#include "libpldm/pldm.h"

#include "common/types.hpp"
#include "common/utils.hpp"
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
                AuxiliaryNames* sensorNames, std::string& associationPath,
                std::shared_ptr<utils::SensorEventInfo> sensorEventInfo);
    ~StateSensor();

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
    inline void setInventoryPaths(const std::vector<std::string>& inventoryPath,
                                  const bool flag)
    {
        for (auto& stateSet : stateSets)
        {
            if (stateSet == nullptr)
            {
                continue;
            }

            std::vector<dbus::PathAssociation> assocs;
            std::string associatedEntityPath;

            for (const auto& path : inventoryPath)
            {
                dbus::PathAssociation assoc = {"chassis", "all_states",
                                               path.c_str()};
                assocs.emplace_back(assoc);
                associatedEntityPath = path;
            }
            stateSet->setAssociation(assocs);
            defaultInventoryAssociated = flag;

            // Track the association entity even while the association is a
            // gated fallback: handleSensorEvent() drops events with an empty
            // entity name, and sensor events arriving in the settling window
            // must keep their (fallback) attribution rather than vanish.
            // Unconditional assignment also clears a stale entity when a
            // sensor later re-degrades from real inventory to the fallback.
            sdbusplus::object_path entityPath(associatedEntityPath);
            associationEntityId = entityPath.filename();
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

    void handleSensorEvent(uint8_t sensorOffset, uint8_t eventState,
                           uint8_t previousEventState);
    void createLogEntry(std::string& messageID, std::string& arg1,
                        std::string& arg2, std::string& resolution,
                        Level level = Level::Informational);
    void createLogEntryAdditionalOEMArgs(
        std::string& messageID, std::string& arg1, std::string& arg2,
        std::string& resolution, std::string& eventId,
        std::string& impactedComponent, Level level = Level::Informational);

#ifdef OEM_NVIDIA
    /** @brief Synthesize a deterministic EventId from the
     *  (entity, sensor, target-state) tuple when no EM SensorEventInfo
     *  override supplied one. Result is upper-cased with non-alphanumeric
     *  runs collapsed to '_', e.g. ("CPU_0","Performance","Throttled")
     *  -> "CPU_0_PERFORMANCE_THROTTLED". (nvbug 6130100, R2)
     */
    static std::string synthesizeEventId(const std::string& entityName,
                                         const std::string& sensorName,
                                         const std::string& targetState);
#endif

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
    uint64_t lastUpdatedTimeStampInUsec = 0;

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

    bool isDefaultInventoryAssociated() const
    {
        return defaultInventoryAssociated;
    }

    /** @brief  getter of sensorEventInfo */
    std::shared_ptr<utils::SensorEventInfo> getSensorEventInfo()
    {
        return sensorEventInfo;
    }

    /** @brief  update sensorEventInfo */
    void updateSensorEventInfo(
        std::shared_ptr<utils::SensorEventInfo> sensorEventInfo)
    {
        this->sensorEventInfo = sensorEventInfo;
    }

  private:
    std::unique_ptr<AvailabilityIntf> availabilityIntf = nullptr;
    std::unique_ptr<OperationalStatusIntf> operationalStatusIntf = nullptr;
    std::string associationEntityId;
    std::string path;
    bool refreshed = false;

    /** @brief flag to indicate if default inventory is associated */
    bool defaultInventoryAssociated;

    /** @brief State sensor event info */
    std::shared_ptr<utils::SensorEventInfo> sensorEventInfo = nullptr;
};
} // namespace platform_mc
} // namespace pldm
