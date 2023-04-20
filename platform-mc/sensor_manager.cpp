#include "sensor_manager.hpp"

#include "manager.hpp"
#include "terminus_manager.hpp"

namespace pldm
{
namespace platform_mc
{

using namespace std::chrono;

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
    auto& bus = pldm::utils::DBusHandler::getBus();

    enableIntf = std::make_unique<SensorPollingEnableIntf>(*this);

    aggregationIntf =
        std::make_unique<AggregationIntf>(bus, aggregationDataPath);

    aggregationIntf->sensorMetrics(sensorMetric);
    aggregationIntf->staleSensorUpperLimitms(
        STALE_SENSOR_UPPER_LIMITS_POLLING_TIME);

    // default priority sensor name spaces
    prioritySensorNameSpaces.emplace_back(
        "/xyz/openbmc_project/sensors/temperature/");
    prioritySensorNameSpaces.emplace_back(
        "/xyz/openbmc_project/sensors/power/");
    prioritySensorNameSpaces.emplace_back(
        "/xyz/openbmc_project/sensors/energy/");

    // default aggregation sensor name spaces
    aggregationSensorNameSpaces.emplace_back(
        "/xyz/openbmc_project/sensors/temperature/");
    aggregationSensorNameSpaces.emplace_back(
        "/xyz/openbmc_project/sensors/power/");
    aggregationSensorNameSpaces.emplace_back(
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

    // load aggregation sensor name spaces
    nameSpaces = data.value("AggregationSensorNameSpaces", emptyStringArray);
    if (nameSpaces.size() > 0)
    {
        aggregationSensorNameSpaces.clear();
        for (const auto& nameSpace : nameSpaces)
        {
            aggregationSensorNameSpaces.emplace_back(nameSpace);
        }
    }
}

bool SensorManager::isPriority(std::shared_ptr<NumericSensor> sensor)
{
    return (std::find(prioritySensorNameSpaces.begin(),
                      prioritySensorNameSpaces.end(),
                      sensor->sensorNameSpace) !=
            prioritySensorNameSpaces.end());
}

bool SensorManager::inSensorMetrics(std::shared_ptr<NumericSensor> sensor)
{
    return (std::find(aggregationSensorNameSpaces.begin(),
                      aggregationSensorNameSpaces.end(),
                      sensor->sensorNameSpace) !=
            aggregationSensorNameSpaces.end());
}

void SensorManager::startPolling()
{
    // initialize prioritySensors and roundRobinSensors list
    for (const auto& [tid, terminus] : termini)
    {
        if (!terminus->doesSupport(PLDM_PLATFORM))
        {
            continue;
        }

        // numeric sensor
        for (auto& sensor : terminus->numericSensors)
        {
            if (isPriority(sensor))
            {
                sensor->isPriority = true;
                prioritySensors[tid].emplace_back(sensor);
            }
            else
            {
                sensor->isPriority = false;
                roundRobinSensors[tid].push(sensor);
            }

            if (inSensorMetrics(sensor))
            {
                sensor->inSensorMetrics = true;
            }
            else
            {
                sensor->inSensorMetrics = false;
            }
        }

        // state sensor
        for (auto& sensor : terminus->stateSensors)
        {
            if (!sensor->async)
            {
                roundRobinSensors[tid].push(sensor);
            }
        }

        if (sensorPollTimers.find(tid) == sensorPollTimers.end())
        {
            sensorPollTimers[tid] = std::make_unique<phosphor::Timer>(
                event.get(),
                std::bind_front(&SensorManager::doSensorPolling, this, tid));
        }

        if (!sensorPollTimers[tid]->isRunning())
        {
            sensorPollTimers[tid]->start(
                duration_cast<std::chrono::milliseconds>(
                    milliseconds(pollingTime)),
                true);
        }
    }

    enableIntf->EnableIntf::enabled(true, false);
}

void SensorManager::stopPolling()
{
    prioritySensors.clear();
    roundRobinSensors.clear();

    for (const auto& [tid, terminus] : termini)
    {
        if (sensorPollTimers[tid])
        {
            sensorPollTimers[tid]->stop();
        }
    }

    enableIntf->EnableIntf::enabled(false, false);
}

void SensorManager::doSensorPolling(tid_t tid)
{
    if (doSensorPollingTaskHandles[tid])
    {
        if (doSensorPollingTaskHandles[tid].done())
        {
            doSensorPollingTaskHandles[tid].destroy();
            auto co = doSensorPollingTask(tid);
            doSensorPollingTaskHandles[tid] = co.handle;
            if (doSensorPollingTaskHandles[tid].done())
            {
                doSensorPollingTaskHandles[tid] = nullptr;
            }
        }
    }
    else
    {
        auto co = doSensorPollingTask(tid);
        doSensorPollingTaskHandles[tid] = co.handle;
        if (doSensorPollingTaskHandles[tid].done())
        {
            doSensorPollingTaskHandles[tid] = nullptr;
        }
    }
}

requester::Coroutine SensorManager::doSensorPollingTask(tid_t tid)
{
    uint64_t t0 = 0;
    uint64_t t1 = 0;
    uint64_t elapsed = 0;
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

        if (manager && terminus->pollEvent)
        {
            co_await manager->pollForPlatformEvent(tid);
        }

        for (auto& effecter : terminus->numericEffecters)
        {
            // GetNumericEffecterValue if we haven't sync.
            if (effecter->state() == StateType::Deferring)
            {
                co_await effecter->getNumericEffecterValue();
                if (sensorPollTimers[tid] &&
                    !sensorPollTimers[tid]->isRunning())
                {
                    co_return PLDM_ERROR;
                }
            }
        }

        for (auto effecter : terminus->stateEffecters)
        {
            // Get StateEffecter states if we haven't sync.
            if (effecter->getOperationalStatus() == StateType::Deferring)
            {
                co_await effecter->getStateEffecterStates();
                if (sensorPollTimers[tid] &&
                    !sensorPollTimers[tid]->isRunning())
                {
                    co_return PLDM_ERROR;
                }
            }
        }

        for (auto sensor : terminus->stateSensors)
        {
            // Get State sensor if we haven't sync.
            if (sensor->needUpdate)
            {
                co_await getStateSensorReadings(sensor);
                if (sensorPollTimers[tid] &&
                    !sensorPollTimers[tid]->isRunning())
                {
                    co_return PLDM_ERROR;
                }
                sensor->needUpdate = false;
            }
        }

        // poll priority Sensors
        for (auto& sensor : prioritySensors[tid])
        {
            if (sensor->updateTime == std::numeric_limits<uint64_t>::max())
            {
                continue;
            }

            sd_event_now(event.get(), CLOCK_MONOTONIC, &t1);
            elapsed = t1 - t0;
            sensor->elapsedTime += (pollingTimeInUsec + elapsed);
            if (sensor->elapsedTime >= sensor->updateTime)
            {
                co_await getSensorReading(sensor);
                if (sensorPollTimers[tid] &&
                    !sensorPollTimers[tid]->isRunning())
                {
                    co_return PLDM_ERROR;
                }
                sensor->elapsedTime = 0;
            }
        }

        // poll roundRobin Sensors
        sd_event_now(event.get(), CLOCK_MONOTONIC, &t1);
        auto toBeUpdated = roundRobinSensors[tid].size();
        while (((t1 - t0) < pollingTimeInUsec) && (toBeUpdated > 0))
        {
            auto sensor = roundRobinSensors[tid].front();
            if (std::holds_alternative<std::shared_ptr<NumericSensor>>(sensor))
            {
                co_await getSensorReading(
                    std::get<std::shared_ptr<NumericSensor>>(sensor));
                if (sensorPollTimers[tid] &&
                    !sensorPollTimers[tid]->isRunning())
                {
                    co_return PLDM_ERROR;
                }
            }
            else if (std::holds_alternative<std::shared_ptr<StateSensor>>(
                         sensor))
            {
                co_await getStateSensorReadings(
                    std::get<std::shared_ptr<StateSensor>>(sensor));
                if (sensorPollTimers[tid] &&
                    !sensorPollTimers[tid]->isRunning())
                {
                    co_return PLDM_ERROR;
                }
            }

            toBeUpdated--;
            roundRobinSensors[tid].pop();
            roundRobinSensors[tid].push(std::move(sensor));
            sd_event_now(event.get(), CLOCK_MONOTONIC, &t1);
        }

        aggregationIntf->sensorMetrics(sensorMetric);

        if (verbose)
        {
            sd_event_now(event.get(), CLOCK_MONOTONIC, &t1);
            lg2::info("end sensor polling at {END}. duration(us):{DELTA}",
                      "END", t1, "DELTA", t1 - t0);
        }

        sd_event_now(event.get(), CLOCK_MONOTONIC, &t1);
    } while ((t1 - t0) >= pollingTimeInUsec);

    co_return PLDM_SUCCESS;
}

requester::Coroutine
    SensorManager::getSensorReading(std::shared_ptr<NumericSensor> sensor)
{
    auto tid = sensor->tid;
    auto sensorId = sensor->sensorId;
    Request request(sizeof(pldm_msg_hdr) + PLDM_GET_SENSOR_READING_REQ_BYTES);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    auto rc = encode_get_sensor_reading_req(0, sensorId, false, requestMsg);
    if (rc)
    {
        lg2::error("encode_get_sensor_reading_req failed, tid={TID}, rc={RC}.",
                   "TID", tid, "RC", rc);
        co_return rc;
    }

    const pldm_msg* responseMsg = NULL;
    size_t responseLen = 0;
    rc = co_await terminusManager.SendRecvPldmMsg(tid, request, &responseMsg,
                                                  &responseLen);
    if (rc)
    {
        lg2::error(
            "getSensorReading failed, tid={TID}, sensorId={SID}, rc={RC}.",
            "TID", tid, "SID", sensorId, "RC", rc);
        co_return rc;
    }

    if (sensorPollTimers[tid] && !sensorPollTimers[tid]->isRunning())
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
            sensor->updateReading(true, false, 0, &sensorMetric);
            co_return completionCode;
        case PLDM_SENSOR_UNAVAILABLE:
        default:
            sensor->updateReading(false, false, 0, &sensorMetric);
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
        default:
            value = std::numeric_limits<double>::quiet_NaN();
            break;
    }

    sensor->updateReading(true, true, value, &sensorMetric);
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

    if (sensorPollTimers[tid] && !sensorPollTimers[tid]->isRunning())
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
} // namespace platform_mc
} // namespace pldm
