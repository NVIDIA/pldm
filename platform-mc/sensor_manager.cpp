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
#include "config.h"

#include "sensor_manager.hpp"

#include "common/sleep.hpp"
#include "manager.hpp"
#include "terminus_manager.hpp"

namespace pldm
{
namespace platform_mc
{

using namespace std::chrono;
using SensorVariant =
    std::variant<std::shared_ptr<NumericSensor>, std::shared_ptr<StateSensor>>;

SensorPollingEnableIntf::SensorPollingEnableIntf(SensorManager& parent) :
    EnableIntf(pldm::utils::DBusHandler::getBus(), sensorPollingControlPath),
    parent(parent){};

bool SensorPollingEnableIntf::enabled(bool value)
{
    if (value)
    {
        parent.startPolling();
    }
    else
    {
        parent.stopPolling();
    }
    // We have set the Enabled property in start/stop Polling functions
    return EnableIntf::enabled();
}

SensorManager::SensorManager(
    sdeventplus::Event& event, TerminusManager& terminusManager,
    std::map<tid_t, std::shared_ptr<Terminus>>& termini, Manager* manager,
    bool verbose, const std::filesystem::path& configJson) :
    event(event),
    terminusManager(terminusManager), termini(termini),
    pollingTime(SENSOR_POLLING_TIME), verbose(verbose), manager(manager)
{
    enableIntf = std::make_unique<SensorPollingEnableIntf>(*this);

    // default priority sensor name spaces
    prioritySensorNameSpaces.emplace_back(
        "/xyz/openbmc_project/sensors/temperature/");
    prioritySensorNameSpaces.emplace_back(
        "/xyz/openbmc_project/sensors/power/");
    prioritySensorNameSpaces.emplace_back(
        "/xyz/openbmc_project/sensors/energy/");

    if (!std::filesystem::exists(configJson))
    {
        return;
    }

    std::ifstream jsonFile(configJson);
    auto data = nlohmann::json::parse(jsonFile, nullptr, false);
    if (data.is_discarded())
    {
        lg2::error("Parsing json file failed. FilePath={FILE_PATH}",
                   "FILE_PATH", std::string(configJson));
        return;
    }

    // load priority sensor name spaces
    const std::vector<std::string> emptyStringArray{};
    auto nameSpaces = data.value("PrioritySensorNameSpaces", emptyStringArray);
    if (nameSpaces.size() > 0)
    {
        prioritySensorNameSpaces.clear();
        for (const auto& nameSpace : nameSpaces)
        {
            prioritySensorNameSpaces.emplace_back(nameSpace);
        }
    }
}

bool SensorManager::isPriority(std::shared_ptr<NumericSensor> sensor)
{
    return (std::find(prioritySensorNameSpaces.begin(),
                      prioritySensorNameSpaces.end(),
                      sensor->getSensorNameSpace()) !=
            prioritySensorNameSpaces.end());
}

void SensorManager::startPolling(tid_t tid)
{
    if (termini.find(tid) == termini.end())
    {
        return;
    }

    auto terminus = termini[tid];
    terminus->stopPolling = false;
    doSensorPolling(tid);
}

void SensorManager::stopPolling(tid_t tid)
{
    if (termini.find(tid) == termini.end())
    {
        return;
    }

    auto terminus = termini[tid];
    terminus->stopPolling = true;
}

void SensorManager::startPolling()
{
    for (const auto& [tid, terminus] : termini)
    {
        startPolling(tid);
    }

    enableIntf->EnableIntf::enabled(true, false);
}

void SensorManager::stopPolling()
{
    for (const auto& [tid, terminus] : termini)
    {
        stopPolling(tid);
    }

    enableIntf->EnableIntf::enabled(false, false);
}

void SensorManager::doSensorPolling(tid_t tid)
{
    if (termini.find(tid) == termini.end())
    {
        return;
    }

    auto terminus = termini[tid];
    if (terminus->doSensorPollingTaskHandle)
    {
        if (terminus->doSensorPollingTaskHandle.done())
        {
            terminus->doSensorPollingTaskHandle.destroy();
            auto co = doSensorPollingTask(tid);
            terminus->doSensorPollingTaskHandle = co.handle;
            if (terminus->doSensorPollingTaskHandle.done())
            {
                terminus->doSensorPollingTaskHandle = nullptr;
            }
        }
    }
    else
    {
        auto co = doSensorPollingTask(tid);
        terminus->doSensorPollingTaskHandle = co.handle;
        if (terminus->doSensorPollingTaskHandle.done())
        {
            terminus->doSensorPollingTaskHandle = nullptr;
        }
    }
}

requester::Coroutine SensorManager::doSensorPollingTask(tid_t tid)
{
    uint64_t t0 = 0;
    uint64_t t1 = 0;
    uint64_t allowedBufferInUsec = 50 * 1000;
    uint64_t pollingTimeInUsec = pollingTime * 1000;

    do
    {
        sd_event_now(event.get(), CLOCK_MONOTONIC, &t0);

        if (verbose)
        {
            lg2::info("TID:{TID} start sensor polling at {NOW}.", "TID", tid,
                      "NOW", t0);
        }

        if (termini.find(tid) == termini.end())
        {
            co_return PLDM_SUCCESS;
        }

        auto& terminus = termini[tid];

        if (manager && !terminus->resumed)
        {
            co_await manager->resumeTerminus(tid);
        }

        if (manager && terminus->pollEvent)
        {
            co_await manager->pollForPlatformEvent(tid);
        }

        for (auto effector : terminus->numericEffecters)
        {
            if (manager && terminus->pollEvent)
            {
                break;
            }

            // Get numeric effector if we haven't sync.
            if (effector->needUpdate)
            {
                co_await effector->getNumericEffecterValue();
                if (terminus->stopPolling)
                {
                    co_return PLDM_ERROR;
                }
                effector->needUpdate = false;
            }
        }

        for (auto effector : terminus->stateEffecters)
        {
            if (manager && terminus->pollEvent)
            {
                break;
            }

            // Get state effector if it haven't been synced or it is
            // updatePending
            if (effector->needUpdate || effector->isUpdatePending())
            {
                co_await effector->getStateEffecterStates();
                if (terminus->stopPolling)
                {
                    co_return PLDM_ERROR;
                }
                effector->needUpdate = false;
            }
        }

        for (auto sensor : terminus->stateSensors)
        {
            if (manager && terminus->pollEvent)
            {
                break;
            }

            // Get State sensor if we haven't sync.
            if (sensor->needUpdate)
            {
                co_await getStateSensorReadings(sensor);
                if (terminus->stopPolling)
                {
                    co_return PLDM_ERROR;
                }
                sensor->needUpdate = false;
            }
        }

        if (terminus->initSensorList)
        {
            initSensorList(tid);
        }

        // poll priority Sensors
        for (auto& sensor : terminus->prioritySensors)
        {
            if (manager && terminus->pollEvent)
            {
                break;
            }

            if (sensor->updateTime == std::numeric_limits<uint64_t>::max())
            {
                continue;
            }

            sd_event_now(event.get(), CLOCK_MONOTONIC, &t1);
            if (sensor->needsUpdate(t1))
            {
                co_await getSensorReading(sensor);
                if (terminus->stopPolling)
                {
                    co_return PLDM_ERROR;
                }
                sensor->setLastUpdatedTimeStamp(t1);
            }
        }

        if (verbose)
        {
            sd_event_now(event.get(), CLOCK_MONOTONIC, &t1);
            lg2::info(
                "TID:{TID} end prioritySensors polling at {END}. duration(us):{DELTA}",
                "TID", tid, "END", t1, "DELTA", t1 - t0);
        }

        // poll roundRobin Sensors
        sd_event_now(event.get(), CLOCK_MONOTONIC, &t1);
        auto toBeUpdated = terminus->roundRobinSensors.size();

        do
        {
            if (!toBeUpdated)
            {
                if (!terminus->ready)
                {
                    // Either we were able to succesfully update all sensors in
                    // one iteration or there are no sensors in the queue. Mark
                    // ready in both cases.
                    terminus->ready = true;
                    checkAllTerminiReady();
                }
                break;
            }

            if (manager && terminus->pollEvent)
            {
                break;
            }

            auto sensor = terminus->roundRobinSensors.front();
            terminus->roundRobinSensors.pop();
            terminus->roundRobinSensors.push(sensor);
            toBeUpdated--;

            // ServiceReady Logic:
            // The round-robin queue is circular hence encountering the first
            // refreshed sensor marks a "complete iteration" of the queue.
            std::visit(
                [&terminus, this](auto&& sensor) {
                    using T = std::decay_t<decltype(sensor)>;
                    if constexpr (std::is_same_v<
                                      T, std::shared_ptr<NumericSensor>> ||
                                  std::is_same_v<T,
                                                 std::shared_ptr<StateSensor>>)
                    {
                        if (!terminus->ready && sensor->isRefreshed())
                        {
                            // The terminus isn't ready but we have found our
                            // first refreshed sensor. Mark the device ready.
                            terminus->ready = true;
                            checkAllTerminiReady();
                        }
                    }
                    else
                    {
                        assert(false && "Unhandled sensor type encountered");
                    }
                },
                sensor);

            sd_event_now(event.get(), CLOCK_MONOTONIC, &t1);
            if (std::holds_alternative<std::shared_ptr<NumericSensor>>(sensor))
            {
                auto numericSesnor =
                    std::get<std::shared_ptr<NumericSensor>>(sensor);
                if (numericSesnor->needsUpdate(t1))
                {
                    co_await getSensorReading(numericSesnor);
                    if (terminus->stopPolling)
                    {
                        co_return PLDM_ERROR;
                    }
                    numericSesnor->setLastUpdatedTimeStamp(t1);
                }
            }
            else if (std::holds_alternative<std::shared_ptr<StateSensor>>(
                         sensor))
            {
                auto stateSesnor =
                    std::get<std::shared_ptr<StateSensor>>(sensor);
                if (stateSesnor->needsUpdate(t1))
                {
                    co_await getStateSensorReadings(stateSesnor);
                    if (terminus->stopPolling)
                    {
                        co_return PLDM_ERROR;
                    }
                    stateSesnor->setLastUpdatedTimeStamp(t1);
                }
            }

            std::visit(
                [](auto&& sensor) {
                    using T = std::decay_t<decltype(sensor)>;
                    if constexpr (std::is_same_v<
                                      T, std::shared_ptr<NumericSensor>> ||
                                  std::is_same_v<T,
                                                 std::shared_ptr<StateSensor>>)
                    {
                        sensor->setRefreshed(true);
                    }
                    else
                    {
                        assert(false && "Unhandled sensor type encountered");
                    }
                },
                sensor);

            sd_event_now(event.get(), CLOCK_MONOTONIC, &t1);
        } while ((t1 - t0) < pollingTimeInUsec);

        if (verbose)
        {
            sd_event_now(event.get(), CLOCK_MONOTONIC, &t1);
            lg2::info(
                "TID:{TID} end roundRobinSensors polling at {END}. duration(us):{DELTA}",
                "TID", tid, "END", t1, "DELTA", t1 - t0);
        }

        sd_event_now(event.get(), CLOCK_MONOTONIC, &t1);

        uint64_t diff = t1 - t0;
        if (diff > pollingTimeInUsec)
        {
            // We have already crossed the polling interval. Don't sleep
            continue;
        }

        uint64_t sleepDeltaInUsec = pollingTimeInUsec - diff;
        if (sleepDeltaInUsec < allowedBufferInUsec)
        {
            // If the delta is within the allowed buffer, we can skip sleeping
            // and continue polling.
            continue;
        }

        co_await timer::Sleep(event, sleepDeltaInUsec, timer::Priority);

    } while (true);

    co_return PLDM_SUCCESS;
}

requester::Coroutine
    SensorManager::getSensorReading(std::shared_ptr<NumericSensor> sensor)
{
    // Do not get sensor reading if sensor does not have a Value Interface
    if (!sensor->valueIntf)
    {
        co_return PLDM_SUCCESS;
    }

    auto tid = sensor->tid;
    auto sensorId = sensor->sensorId;
    auto pollingIndicator = sensor->getPollingIndicator();
    const pldm_msg* responseMsg = NULL;
    size_t responseLen = 0;
    int rc;

    if (pollingIndicator == POLLING_METHOD_INDICATOR_PLDM_TYPE_TWO)
    {
        Request request(sizeof(pldm_msg_hdr) +
                        PLDM_GET_SENSOR_READING_REQ_BYTES);
        auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

        rc = encode_get_sensor_reading_req(0, sensorId, false, requestMsg);
        if (rc)
        {
            lg2::error(
                "encode_get_sensor_reading_req failed, tid={TID}, rc={RC}.",
                "TID", tid, "RC", rc);
            co_return rc;
        }
        rc = co_await terminusManager.SendRecvPldmMsg(
            tid, request, &responseMsg, &responseLen);
    }
#ifdef OEM_NVIDIA
    else if (pollingIndicator == POLLING_METHOD_INDICATOR_PLDM_TYPE_OEM)
    {
        Request request(sizeof(pldm_msg_hdr) +
                        PLDM_GET_OEM_ENERGYCOUNT_SENSOR_READING_REQ_BYTES);
        auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

        rc = encode_get_oem_enegy_count_sensor_reading_req(0, sensorId,
                                                           requestMsg);
        if (rc)
        {
            lg2::error(
                "encode_get_oem_enegycount_sensor_reading_req failed, tid={TID}, rc={RC}.",
                "TID", tid, "RC", rc);
            co_return rc;
        }
        rc = co_await terminusManager.SendRecvPldmMsg(
            tid, request, &responseMsg, &responseLen);
    }
#endif
    else
    {
        lg2::error(
            "Incorrect PLDM polling type [Type2 or OEM type are valid], PldmType={INDICATOR}.",
            "INDICATOR", pollingIndicator);
        co_return PLDM_ERROR;
    }

    if (rc)
    {
        lg2::error(
            "getSensorReading failed, tid={TID}, sensorId={SID}, rc={RC}.",
            "TID", tid, "SID", sensorId, "RC", rc);
        co_return rc;
    }

    if (termini.find(tid) == termini.end())
    {
        co_return PLDM_ERROR;
    }

    auto terminus = termini[tid];
    if (terminus->stopPolling)
    {
        co_return PLDM_ERROR;
    }

    uint8_t completionCode = PLDM_SUCCESS;
    uint8_t sensorDataSize = PLDM_SENSOR_DATA_SIZE_SINT32;
    uint8_t sensorOperationalState = 0;
    uint8_t sensorEventMessageEnable = 0;
    uint8_t presentState = 0;
    uint8_t previousState = 0;
    uint8_t eventState = 0;
    union_sensor_data_size presentReading;

    if (pollingIndicator == POLLING_METHOD_INDICATOR_PLDM_TYPE_TWO)
    {
        rc = decode_get_sensor_reading_resp(
            responseMsg, responseLen, &completionCode, &sensorDataSize,
            &sensorOperationalState, &sensorEventMessageEnable, &presentState,
            &previousState, &eventState,
            reinterpret_cast<uint8_t*>(&presentReading));

        if (rc)
        {
            lg2::error(
                "Failed to decode response of GetSensorReading, tid={TID}, rc={RC}.",
                "TID", tid, "RC", rc);
            sensor->handleErrGetSensorReading();
            co_return rc;
        }
    }
#ifdef OEM_NVIDIA
    else if (pollingIndicator == POLLING_METHOD_INDICATOR_PLDM_TYPE_OEM)
    {
        sensorDataSize = PLDM_SENSOR_DATA_SIZE_SINT64;
        rc = decode_get_oem_energy_count_sensor_reading_resp(
            responseMsg, responseLen, &completionCode, &sensorDataSize,
            &sensorOperationalState,
            reinterpret_cast<uint8_t*>(&presentReading));

        if (rc)
        {
            lg2::error(
                "Failed to decode response of GetOemEnergyCountSensorReading, tid={TID}, rc={RC}.",
                "TID", tid, "RC", rc);
            sensor->handleErrGetSensorReading();
            co_return rc;
        }
    }
#endif
    else
    {
        lg2::error(
            "Incorrect PLDM polling type [Type2 or OEM type are valid], PldmType={INDICATOR}.",
            "INDICATOR", pollingIndicator);
        co_return PLDM_ERROR;
    }

    if (completionCode != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to decode response of GetSensorReading, tid={TID}, rc={RC}, cc={CC}.",
            "TID", tid, "RC", rc, "CC", completionCode);
        co_return completionCode;
    }

    switch (sensorOperationalState)
    {
        case PLDM_SENSOR_ENABLED:
            break;
        case PLDM_SENSOR_DISABLED:
            sensor->updateReading(true, false, 0);
            co_return completionCode;
        case PLDM_SENSOR_UNAVAILABLE:
        default:
            sensor->updateReading(false, false, 0);
            co_return completionCode;
    }

    double value;
    switch (sensorDataSize)
    {
        case PLDM_SENSOR_DATA_SIZE_UINT8:
            value = static_cast<double>(presentReading.value_u8);
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT8:
            value = static_cast<double>(presentReading.value_s8);
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT16:
            value = static_cast<double>(presentReading.value_u16);
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT16:
            value = static_cast<double>(presentReading.value_s16);
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT32:
            value = static_cast<double>(presentReading.value_u32);
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT32:
            value = static_cast<double>(presentReading.value_s32);
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT64:
            value = static_cast<double>(presentReading.value_u64);
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT64:
            value = static_cast<double>(presentReading.value_s64);
            break;
        default:
            value = std::numeric_limits<double>::quiet_NaN();
            break;
    }

    sensor->updateReading(true, true, value);
    co_return completionCode;
}

requester::Coroutine
    SensorManager::getStateSensorReadings(std::shared_ptr<StateSensor> sensor)
{
    auto tid = sensor->tid;
    auto sensorId = sensor->sensorId;

    Request request(sizeof(pldm_msg_hdr) +
                    PLDM_GET_STATE_SENSOR_READINGS_REQ_BYTES);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    auto rc = encode_get_state_sensor_readings_req(0, sensorId, (bitfield8_t)0,
                                                   0x0, requestMsg);
    if (rc)
    {
        lg2::error(
            "encode_get_state_sensor_readings_req failed, sid={SID}, tid={TID}, rc={RC}.",
            "SID", sensorId, "TID", tid, "RC", rc);
        co_return rc;
    }

    const pldm_msg* responseMsg = NULL;
    size_t responseLen = 0;
    rc = co_await terminusManager.SendRecvPldmMsg(tid, request, &responseMsg,
                                                  &responseLen);

    if (rc)
    {
        lg2::error(
            "getStateSensorReadings failed, tid={TID}, sensorId={SID}, rc={RC}.",
            "TID", tid, "SID", sensorId, "RC", rc);
        co_return rc;
    }

    if (termini.find(tid) == termini.end())
    {
        co_return PLDM_ERROR;
    }

    auto terminus = termini[tid];
    if (terminus->stopPolling)
    {
        co_return PLDM_ERROR;
    }

    uint8_t completionCode = PLDM_SUCCESS;
    uint8_t comp_sensor_count = 0;
    std::array<get_sensor_state_field, 8> stateField{};
    rc = decode_get_state_sensor_readings_resp(
        responseMsg, responseLen, &completionCode, &comp_sensor_count,
        stateField.data());
    if (rc)
    {
        lg2::error(
            "Failed to decode response of GetStateSensorReadings, sid={SID}, tid={TID}, rc={RC}.",
            "SID", sensorId, "TID", tid, "RC", rc);
        sensor->handleErrGetSensorReading();
        co_return rc;
    }
    if (completionCode != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to decode response of GetStateSensorReadings, sid={SID}, tid={TID}, cc={CC}.",
            "SID", sensorId, "TID", tid, "CC", completionCode);
        sensor->handleErrGetSensorReading();
        co_return rc;
    }

    for (size_t i = 0; i < comp_sensor_count; i++)
    {
        sensor->updateReading(true, true, i, stateField[i].present_state);
    }

    co_return completionCode;
}

void SensorManager::initSensorList(tid_t tid)
{
    if (termini.find(tid) == termini.end())
    {
        return;
    }

    auto terminus = termini[tid];
    // clear and initialize prioritySensors and roundRobinSensors list
    terminus->prioritySensors.clear();
    std::queue<std::variant<std::shared_ptr<NumericSensor>,
                            std::shared_ptr<StateSensor>>>
        empty;
    std::swap(terminus->roundRobinSensors, empty);

    // numeric sensor
    for (auto& sensor : terminus->numericSensors)
    {
        if (isPriority(sensor))
        {
            sensor->isPriority = true;
            terminus->prioritySensors.emplace_back(sensor);
        }
        else
        {
            sensor->isPriority = false;
            terminus->roundRobinSensors.push(sensor);
        }
    }

    // state sensor
    for (auto& sensor : terminus->stateSensors)
    {
        if (!sensor->async)
        {
            terminus->roundRobinSensors.push(sensor);
        }
    }
    terminus->initSensorList = false;
}

} // namespace platform_mc
} // namespace pldm
