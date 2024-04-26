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

#ifdef OEM_NVIDIA
#include "oem/nvidia/libpldm/energy_count_numeric_sensor_oem.h"
#endif

#include "common/types.hpp"
#include "numeric_sensor.hpp"
#include "pldmd/dbus_impl_requester.hpp"
#include "requester/handler.hpp"
#include "terminus.hpp"
#include "terminus_manager.hpp"

#include <xyz/openbmc_project/Object/Enable/server.hpp>
#include <xyz/openbmc_project/Sensor/Aggregation/server.hpp>

#include <queue>
#include <variant>

namespace pldm
{
namespace platform_mc
{

using AggregationIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Sensor::server::Aggregation>;
using EnableIntf = sdbusplus::xyz::openbmc_project::Object::server::Enable;
constexpr auto aggregationDataPath =
    "/xyz/openbmc_project/inventory/platformmetrics";
constexpr auto sensorPollingControlPath =
    "/xyz/openbmc_project/pldm/sensor_polling";

class SensorManager;
class SensorPollingEnableIntf : public EnableIntf
{
  public:
    SensorPollingEnableIntf(SensorManager& parent);
    bool enabled(bool value) override;

  private:
    SensorManager& parent;
};

/**
 * @brief SensorManager
 *
 * This class manages the sensors found in terminus and provides
 * function calls for other classes to start/stop sensor monitoring.
 *
 */
class SensorManager
{
  public:
    SensorManager() = delete;
    SensorManager(const SensorManager&) = delete;
    SensorManager(SensorManager&&) = delete;
    SensorManager& operator=(const SensorManager&) = delete;
    SensorManager& operator=(SensorManager&&) = delete;
    virtual ~SensorManager() = default;

    explicit SensorManager(
        sdeventplus::Event& event, TerminusManager& terminusManager,
        std::map<tid_t, std::shared_ptr<Terminus>>& termini, Manager* manager,
        bool verbose = false,
        const std::filesystem::path& configJson = PLDM_T2_CONFIG_JSON);

    /** @brief starting all termini sensor polling task
     */
    void startPolling();

    /** @brief stopping all termini sensor polling task
     */
    void stopPolling();

    /** @brief starting terminus sensor polling task
     */
    void startPolling(tid_t tid);

    /** @brief stopping terminus sensor polling task
     */
    void stopPolling(tid_t tid);

    /** @brief set terminus sensor online and resume polling timer
     */
    void setOnline(tid_t tid)
    {
        if(termini.find(tid)!= termini.end()) {
            termini[tid]->setOnline();
            startPolling(tid);
        }
    }

    /** @brief set terminus sensor offline and stop polling timer
     */
    void setOffline(tid_t tid)
    {
        if(termini.find(tid)!= termini.end()) {
            termini[tid]->setOffline();
            stopPolling(tid);
        }
    }

  protected:
    /** @brief start a coroutine for polling all sensors.
     */
    virtual void doSensorPolling(tid_t tid);

    /** @brief polling all sensors in each terminus
     */
    requester::Coroutine doSensorPollingTask(tid_t tid);

    /** @brief Sending getSensorReading command for the sensor
     *
     *  @param[in] sensor - the sensor to be updated
     */
    requester::Coroutine
        getSensorReading(std::shared_ptr<NumericSensor> sensor);

    /** @brief Sending getStateSensorReadings command for the sensor
     *
     *  @param[in] sensor - the sensor to be updated
     */
    requester::Coroutine
        getStateSensorReadings(std::shared_ptr<StateSensor> sensor);

    /** @brief check if numeric sensor is in priority name spaces
     *
     *  @param[in] sensor - the sensor to be checked
     *
     *  @return bool - true:is in priority
     */
    bool isPriority(std::shared_ptr<NumericSensor> sensor);

    /** @brief check if numeric sensor is in aggregation name spaces
     *
     *  @param[in] sensor - the sensor to be checked
     *
     *  @return bool - true:is in priority
     */
    bool inSensorMetrics(std::shared_ptr<NumericSensor> sensor);

    sdeventplus::Event& event;

    /** @brief reference of terminusManager */
    TerminusManager& terminusManager;

    /** @brief List of discovered termini */
    std::map<tid_t, std::shared_ptr<Terminus>>& termini;

    /** @brief sensor polling interval in ms. */
    uint32_t pollingTime;

    /** @brief sensor polling timers */
    std::map<tid_t, std::unique_ptr<sdbusplus::Timer>> sensorPollTimers;

    /** @brief coroutine handle of doSensorPollingTasks */
    std::map<tid_t, std::coroutine_handle<>> doSensorPollingTaskHandles;

    /** @brief aggregation Interface */
    std::unique_ptr<AggregationIntf> aggregationIntf = nullptr;

    std::unique_ptr<SensorPollingEnableIntf> enableIntf = nullptr;

    /** @brief verbose tracing flag */
    bool verbose;

    /** @brief aggregation SensorNameSpace list */
    std::vector<std::string> aggregationSensorNameSpaces;

    /** @brief priority SensorNameSpace list */
    std::vector<std::string> prioritySensorNameSpaces;

    /** @brief priority sensor list */
    std::map<tid_t, std::vector<std::shared_ptr<NumericSensor>>>
        prioritySensors;

    /** @brief round robin sensor list */
    std::map<tid_t, std::queue<std::variant<std::shared_ptr<NumericSensor>,
                                            std::shared_ptr<StateSensor>>>>
        roundRobinSensors;

    /** @brief pointer to Manager */
    Manager* manager;
};
} // namespace platform_mc
} // namespace pldm
