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
#include "libpldm/requester/pldm.h"

#include "common/types.hpp"
#include "numeric_sensor.hpp"
#include "pldmd/dbus_impl_requester.hpp"
#include "requester/handler.hpp"
#include "terminus.hpp"

namespace pldm::fw_update
{
class Manager;
}

namespace pldm
{
namespace platform_mc
{

const std::string SensorThresholdCriticalHighGoingHigh{
    "OpenBMC.0.2.SensorThresholdCriticalHighGoingHigh"};
const std::string SensorThresholdCriticalHighGoingLow{
    "OpenBMC.0.2.SensorThresholdCriticalHighGoingLow"};
const std::string SensorThresholdCriticalLowGoingHigh{
    "OpenBMC.0.2.SensorThresholdCriticalLowGoingHigh"};
const std::string SensorThresholdCriticalLowGoingLow{
    "OpenBMC.0.2.SensorThresholdCriticalLowGoingLow"};
const std::string SensorThresholdWarningHighGoingHigh{
    "OpenBMC.0.2.SensorThresholdWarningHighGoingHigh"};
const std::string SensorThresholdWarningHighGoingLow{
    "OpenBMC.0.2.SensorThresholdWarningHighGoingLow"};
const std::string SensorThresholdWarningLowGoingHigh{
    "OpenBMC.0.2.SensorThresholdWarningLowGoingHigh"};
const std::string SensorThresholdWarningLowGoingLow{
    "OpenBMC.0.2.SensorThresholdWarningLowGoingLow"};

/**
 * @brief EventManager
 *
 * This class manages PLDM events from terminus. The function includes providing
 * the API for process event data and using phosphor-logging API to log the
 * event.
 *
 */
class EventManager
{
  public:
    EventManager() = delete;
    EventManager(const EventManager&) = delete;
    EventManager(EventManager&&) = delete;
    EventManager& operator=(const EventManager&) = delete;
    EventManager& operator=(EventManager&&) = delete;
    virtual ~EventManager() = default;

    explicit EventManager(
        TerminusManager& terminusManager,
        std::map<mctp_eid_t, std::shared_ptr<Terminus>>& termini,
        fw_update::Manager& fwUpdateManager, bool verbose = false) :
        terminusManager(terminusManager),
        termini(termini), fwUpdateManager(fwUpdateManager), verbose(verbose){};

    /** @brief Handle platform event
     *
     *  @param[in] tid - tid where the event is from
     *  @param[in] eventClass - event class
     *  @param[in] eventData - event data
     *  @param[in] eventDataSize - size of event data
     *  @return PLDM completion code
     *
     */
    int handlePlatformEvent(tid_t tid, uint8_t eventClass,
                            const uint8_t* eventData, size_t eventDataSize,
                            uint8_t& platformEventStatus);

    std::string getSensorThresholdMessageId(uint8_t previousEventState,
                                            uint8_t eventState);

    /** @brief A Coroutine to poll all events from terminus
     *
     *  @param[in] dstTid - the destination TID
     */
    requester::Coroutine pollForPlatformEventTask(tid_t tid,
                                                  uint16_t maxBufferSize);

  protected:
    /** @brief Send pollForPlatformEventMessage and return response
     *
     *  @param[in] tid
     *  @param[in] transferOpFlag
     *  @param[in] dataTransferHandle
     *  @param[in] eventIdToAcknowledge
     *  @param[out] completionCode
     *  @param[out] eventTid
     *  @param[out] eventId
     *  @param[out] nextDataTransferHandle
     *  @param[out] transferFlag
     *  @param[out] eventClass
     *  @param[out] eventDataSize
     *  @param[out] eventData
     *  @param[out] eventDataIntegrityChecksum
     *  @return coroutine return_value - PLDM completion code
     *
     */
    requester::Coroutine pollForPlatformEventMessage(
        tid_t tid, uint8_t transferOperationFlag, uint32_t dataTransferHandle,
        uint16_t eventIdToAcknowledge, uint8_t& completionCode,
        uint8_t& eventTid, uint16_t& eventId, uint32_t& nextDataTransferHandle,
        uint8_t& transferFlag, uint8_t& eventClass, uint32_t& eventDataSize,
        std::vector<uint8_t>& eventData, uint32_t& eventDataIntegrityChecksum);

    void notifyCPERLogger(const std::string& dataPath);

    void processDeferredPldmMessagePollEvent(uint8_t tid);

    void processNumericSensorEvent(tid_t tid, uint16_t sensorId,
                                   const uint8_t* sensorData,
                                   size_t sensorDataLength);

    void processStateSensorEvent(tid_t tid, uint16_t sensorId,
                                 const uint8_t* sensorData,
                                 size_t sensorDataLength);

    virtual void createSensorThresholdLogEntry(const std::string& messageID,
                                               const std::string& sensorName,
                                               const double reading,
                                               const double threshold);

    /** @brief Reference of terminusManager */
    TerminusManager& terminusManager;

    /** @brief List of discovered termini */
    std::map<tid_t, std::shared_ptr<Terminus>>& termini;

    fw_update::Manager& fwUpdateManager;

    /** @brief verbose tracing flag */
    bool verbose;
};
} // namespace platform_mc
} // namespace pldm
