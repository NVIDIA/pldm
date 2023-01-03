#pragma once

#include "libpldm/platform.h"
#include "libpldm/requester/pldm.h"

#include "common/types.hpp"
#include "numeric_sensor.hpp"
#include "pldmd/dbus_impl_requester.hpp"
#include "requester/handler.hpp"
#include "terminus.hpp"
#include "terminus_manager.hpp"

#include <xyz/openbmc_project/Sensor/Aggregation/server.hpp>

namespace pldm
{
namespace platform_mc
{

using AggregationIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Sensor::server::Aggregation>;
constexpr auto aggregationDataPath =
    "/xyz/openbmc_project/inventory/platformmetrics";

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
        std::map<tid_t, std::shared_ptr<Terminus>>& termini) :
        event(event),
        terminusManager(terminusManager), termini(termini),
        pollingTime(SENSOR_POLLING_TIME)
    {
        auto& bus = pldm::utils::DBusHandler::getBus();
        aggregationIntf =
            std::make_unique<AggregationIntf>(bus, aggregationDataPath);
        aggregationIntf->sensorMetrics(sensorMetric);
        aggregationIntf->staleSensorUpperLimitms(
            STALE_SENSOR_UPPER_LIMITS_POLLING_TIME);
    };

    /** @brief starting sensor polling task
     */
    void startPolling();

    /** @brief stopping sensor polling task
     */
    void stopPolling();

  protected:
    /** @brief start a coroutine for polling all sensors.
     */
    virtual void doSensorPolling()
    {
        if (doSensorPollingTaskHandle)
        {
            if (doSensorPollingTaskHandle.done())
            {
                doSensorPollingTaskHandle.destroy();
                auto co = doSensorPollingTask();
                doSensorPollingTaskHandle = co.handle;
                if (doSensorPollingTaskHandle.done())
                {
                    doSensorPollingTaskHandle = nullptr;
                }
            }
        }
        else
        {
            auto co = doSensorPollingTask();
            doSensorPollingTaskHandle = co.handle;
            if (doSensorPollingTaskHandle.done())
            {
                doSensorPollingTaskHandle = nullptr;
            }
        }
    }

    /** @brief polling all sensors in each terminus
     */
    requester::Coroutine doSensorPollingTask();

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

    sdeventplus::Event& event;

    /** @brief reference of terminusManager */
    TerminusManager& terminusManager;

    /** @brief List of discovered termini */
    std::map<tid_t, std::shared_ptr<Terminus>>& termini;

    /** @brief sensor polling interval in sec. */
    uint32_t pollingTime;

    /** @brief sensor polling timer */
    std::unique_ptr<phosphor::Timer> sensorPollTimer;

    /** @brief coroutine handle of doSensorPollingTask */
    std::coroutine_handle<> doSensorPollingTaskHandle;

    std::unique_ptr<AggregationIntf> aggregationIntf = nullptr;

    /** @brief All sensor aggregated metrics, mapping from sensor name to sensor
     * value, timestamp and chassis path
     */
    sensorMap sensorMetric{};
};
} // namespace platform_mc
} // namespace pldm
