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
#include "event_manager.hpp"

#include "libpldm/platform.h"
#include "libpldm/utils.h"

#include "common/sleep.hpp"
#include "fw-update/manager.hpp"
#include "oem_events.hpp"
#include "platform_manager.hpp"
#ifdef OEM_NVIDIA
#include "oem/nvidia/platform-mc/pcie_port_info.hpp"
#endif
#include "sensor_manager.hpp"
#include "terminus_manager.hpp"

#include <phosphor-logging/lg2.hpp>
#include <xyz/openbmc_project/Logging/Entry/server.hpp>

#include <algorithm>
#include <cerrno>
#include <queue>
#include <variant>

namespace pldm
{
namespace platform_mc
{
namespace fs = std::filesystem;

// Use OEM event constants from common platform definitions
using pldm::platform::PLDM_OEM_EVENT_CLASS_0xF3;
using pldm::platform::PLDM_OEM_EVENT_CLASS_0xFD;
using pldm::platform::PLDM_OEM_EVENT_CLASS_ERROR_COUNTER;
using pldm::platform::PLDM_OEM_EVENT_CLASS_MFTDUMP;
using pldm::platform::PLDM_OEM_EVENT_CLASS_PCIE_PORT_INFO;
using pldm::platform::PLDM_OEM_EVENT_CLASS_PCIE_TELEMETRY;
using pldm::platform::PLDM_TELEMETRY_PAUSE;
using pldm::platform::PLDM_TELEMETRY_REDISCOVER;
using pldm::platform::PLDM_TELEMETRY_RESUME;

// Default terminus name used when actual name cannot be determined
constexpr auto DEFAULT_TERMINUS_NAME = "ProcessorModule_0";

int EventManager::handlePlatformEvent(
    tid_t tid, uint8_t eventClass, const uint8_t* eventData,
    size_t eventDataSize, uint8_t& platformEventStatus)
{
    platformEventStatus = PLDM_EVENT_NO_LOGGING;

    lg2::debug("handlePlatformEvent: tid={TID}, eventClass={EC}, size={SIZE}",
               "TID", tid, "EC", lg2::hex, eventClass, "SIZE", eventDataSize);

    if (eventClass == PLDM_SENSOR_EVENT)
    {
        uint16_t sensorId = 0;
        uint8_t sensorEventClassType = 0;
        size_t eventClassDataOffset = 0;
        auto rc = decode_sensor_event_data(eventData, eventDataSize, &sensorId,
                                           &sensorEventClassType,
                                           &eventClassDataOffset);
        if (rc)
        {
            lg2::error(
                "Failed to decode sensor event data, rc={RC} eventDataSize={SIZE} sensorId={SID}, ClassType={CTYPE}.",
                "RC", rc, "SIZE", eventDataSize, "SID", sensorId, "CTYPE",
                sensorEventClassType);
            return rc;
        }
        switch (sensorEventClassType)
        {
            case PLDM_NUMERIC_SENSOR_STATE:
            {
                const uint8_t* sensorData = eventData + eventClassDataOffset;
                size_t sensorDataLength = eventDataSize - eventClassDataOffset;
                processNumericSensorEvent(tid, sensorId, sensorData,
                                          sensorDataLength);
                break;
            }
            case PLDM_STATE_SENSOR_STATE:
            {
                const uint8_t* sensorData = eventData + eventClassDataOffset;
                size_t sensorDataLength = eventDataSize - eventClassDataOffset;
                processStateSensorEvent(tid, sensorId, sensorData,
                                        sensorDataLength);
                break;
            }
            case PLDM_SENSOR_OP_STATE:
            default:
                lg2::info("unhandled sensor event, class type={CLASSTYPE}",
                          "CLASSTYPE", sensorEventClassType);
                platformEventStatus = PLDM_EVENT_LOGGING_REJECTED;
                break;
        }
    }
    else if (eventClass == PLDM_MESSAGE_POLL_EVENT)
    {
        if (verbose)
        {
            lg2::info("received poll event tid={TID}", "TID", tid);
        }
        auto it = termini.find(tid);
        if (it != termini.end())
        {
            // Capture the dataTransferHandle advertised by the poll event so
            // the first PollForPlatformEventMessage starts from it rather than
            // from handle 0.
            pldm_message_poll_event pollEventData{};
            auto drc = decode_pldm_message_poll_event_data(
                eventData, eventDataSize, &pollEventData);
            if (drc == PLDM_SUCCESS)
            {
                it->second->pollDataTransferHandle =
                    pollEventData.data_transfer_handle;
            }
            else
            {
                lg2::error(
                    "Failed to decode message poll event data, tid={TID} rc={RC}",
                    "TID", tid, "RC", drc);
            }
            it->second->pollEvent = true;
        }
    }
    else if (eventClass == PLDM_OEM_EVENT_CLASS_0xFB)
    {
        auto mctpInfo = terminusManager.toMctpInfo(tid);
        if (!mctpInfo)
        {
            lg2::error("handlePlatformEvent: cannot find eid for tid:{TID}.",
                       "TID", tid);
            return PLDM_ERROR;
        }

        auto eid = std::get<0>(mctpInfo.value());
        fwUpdateManager.updateFWInventory(eid);
    }
    else if (eventClass == PLDM_OEM_EVENT_CLASS_0xFA ||
             eventClass == PLDM_CPER_EVENT)
    {
        uint8_t formatVersion;
        uint8_t formatType;
        uint16_t cperEventDataLength;
        uint8_t* cperEventData;
        auto rc = decode_pldm_cper_event_data(
            eventData, eventDataSize, &formatVersion, &formatType,
            &cperEventDataLength, &cperEventData);

        if (rc)
        {
            lg2::error("Failed to decode CPER event data, rc={RC}", "RC", rc);
            return rc;
        }

        const unsigned char* eventDataChar =
            std::bit_cast<const unsigned char*>(eventData);
        notifyCPERLogger(
            std::span<const unsigned char>(eventDataChar, eventDataSize));
    }
    else if (eventClass == PLDM_OEM_EVENT_CLASS_0xFC)
    {
        lg2::info("Handling 0xFC SMBIOS Event from tid={TID}, dataSize={SIZE}",
                  "TID", tid, "SIZE", eventDataSize);

        if (!oem_events::handleSmbiosEvent(eventData, eventDataSize))
        {
            lg2::error("Failed to handle 0xFC event from tid={TID}", "TID",
                       tid);
            platformEventStatus = PLDM_EVENT_LOGGING_REJECTED;
            return PLDM_ERROR;
        }
    }
    else if (eventClass == PLDM_OEM_EVENT_CLASS_0xF3)
    {
        lg2::info(
            "Handling 0xF3 Inventory Event from tid={TID}, dataSize={SIZE}",
            "TID", tid, "SIZE", eventDataSize);

        std::string terminusName = DEFAULT_TERMINUS_NAME;
        auto terminusIt = termini.find(tid);
        if (terminusIt != termini.end() && terminusIt->second)
        {
            auto name = terminusIt->second->getTerminusName();
            if (name.has_value())
            {
                terminusName = std::string(name.value());
            }
        }

        if (!oem_events::handleInventoryEvent(terminusName, eventData,
                                              eventDataSize))
        {
            lg2::error("Failed to handle 0xF3 event from tid={TID}", "TID",
                       tid);
            platformEventStatus = PLDM_EVENT_LOGGING_REJECTED;
            return PLDM_ERROR;
        }
    }
    else if (eventClass == PLDM_OEM_EVENT_CLASS_ERROR_COUNTER ||
             eventClass == PLDM_OEM_EVENT_CLASS_PCIE_TELEMETRY ||
             eventClass == PLDM_OEM_EVENT_CLASS_PCIE_PORT_INFO ||
             eventClass == PLDM_OEM_EVENT_CLASS_MFTDUMP)
    {
        // Helper to get terminus name from tid
        auto getTerminusName = [this](tid_t tid) -> std::string {
            auto it = termini.find(tid);
            if (it != termini.end() && it->second)
            {
                auto name = it->second->getTerminusName();
                if (name.has_value())
                {
                    return std::string(name.value());
                }
            }
            return DEFAULT_TERMINUS_NAME;
        };

        std::string terminusName = getTerminusName(tid);
        bool success = false;

        switch (eventClass)
        {
            case PLDM_OEM_EVENT_CLASS_ERROR_COUNTER:
                // CPER Error Counter Event (0xF0)
                lg2::info(
                    "Received CPER Error Counter Event ({EC}) from tid={TID}",
                    "EC", lg2::hex, eventClass, "TID", tid);
                success = oem_events::handleCperErrorCountEvent(
                    terminusName, eventData, eventDataSize);
                break;

            case PLDM_OEM_EVENT_CLASS_PCIE_TELEMETRY:
                // PCIe Telemetry Event (0xF1)
                lg2::info("Received PCIe Telemetry Event ({EC}) from tid={TID}",
                          "EC", lg2::hex, eventClass, "TID", tid);
                success = oem_events::handlePcieTelemetryEvent(
                    terminusName, eventData, eventDataSize);
                break;

            case PLDM_OEM_EVENT_CLASS_PCIE_PORT_INFO:
                // PCIe Port Info Event (0xF4)
                lg2::debug(
                    "Received PCIe Port Info Event ({EC}) from tid={TID}", "EC",
                    lg2::hex, eventClass, "TID", tid);
#ifdef OEM_NVIDIA
                success = pldm::oem_nvidia::handlePciePortInfoEvent(
                    terminusName, eventData, eventDataSize);
#else
                // OEM feature disabled: accept and drop.
                success = true;
#endif
                break;

            case PLDM_OEM_EVENT_CLASS_MFTDUMP:
                // MFTDump Event (0xF2)
                lg2::info("Received MFTDump Event ({EC}) from tid={TID}", "EC",
                          lg2::hex, eventClass, "TID", tid);
                success = oem_events::handleMftDumpEvent(
                    terminusName, eventData, eventDataSize);
                break;

            default:
                // Should not reach here due to outer else-if condition
                break;
        }

        if (!success)
        {
            lg2::error("Failed to handle OEM event {EC}", "EC", lg2::hex,
                       eventClass);
            platformEventStatus = PLDM_EVENT_LOGGING_REJECTED;
            return PLDM_ERROR;
        }
    }
    else if (eventClass == PLDM_OEM_EVENT_CLASS_0xFD)
    {
        // Handle Telemetry Management Event from terminus
        // Used for scenarios like Live Firmware Activation (LFA)
        // Supports: Pause (0x00), Rediscover (0x01), Resume (0x02)
        //
        // Event Data Format:
        // Byte 0:    Version
        // Byte 1:    Telemetry State (0x00=Pause, 0x01=Rediscover, 0x02=Resume)

        constexpr size_t MIN_EVENT_DATA_SIZE = 2;

        if (eventDataSize < MIN_EVENT_DATA_SIZE)
        {
            lg2::error(
                "Invalid event data size for OEM event 0xFD, size={SIZE}, expected at least {MIN}",
                "SIZE", eventDataSize, "MIN", MIN_EVENT_DATA_SIZE);
            return PLDM_ERROR;
        }

        // Parse event data header
        uint8_t version = eventData[0];
        uint8_t telemetryState = eventData[1];

        lg2::info("OEM Event 0xFD: tid={TID}, version={VER}, state={STATE}",
                  "TID", tid, "VER", static_cast<unsigned int>(version),
                  "STATE", static_cast<unsigned int>(telemetryState));

        if (telemetryState == PLDM_TELEMETRY_PAUSE)
        {
            // Pause PLDM Type 2 Telemetry monitoring
            lg2::info(
                "Received telemetry PAUSE event from tid={TID}, stopping Type 2 monitoring",
                "TID", tid);
            processTelemetryPauseEvent(tid);
        }
        else if (telemetryState == PLDM_TELEMETRY_REDISCOVER)
        {
            // Rediscover PLDM Type 2 - teardown and reinitialize
            lg2::info(
                "Received telemetry REDISCOVER event from tid={TID}, initiating Type 2 rediscovery",
                "TID", tid);

            // Trigger async rediscovery task
            stdexec::start_detached(
                processTelemetryRediscoveryEvent(tid),
                exec::default_task_context<int>(exec::inline_scheduler{}));
        }
        else if (telemetryState == PLDM_TELEMETRY_RESUME)
        {
            // Resume PLDM Type 2 monitoring without rediscovery
            lg2::info(
                "Received telemetry RESUME event from tid={TID}, resuming Type 2 monitoring",
                "TID", tid);
            processTelemetryResumeEvent(tid);
        }
        else
        {
            lg2::error(
                "Unknown telemetry state in OEM event 0xFD, tid={TID}, state={STATE}",
                "TID", tid, "STATE", telemetryState);
            platformEventStatus = PLDM_EVENT_LOGGING_REJECTED;
            return PLDM_ERROR;
        }
    }
    else if (eventClass == PLDM_PDR_REPOSITORY_CHG_EVENT)
    {
        // DSP0248 Table 24 pldmPDRRepositoryChgEvent. Decode synchronously into
        // owned data here (eventData is a borrowed buffer), then hand off to a
        // detached coroutine for the async PDR fetch + D-Bus reconciliation.
        uint8_t eventDataFormat = 0;
        uint8_t numberOfChangeRecords = 0;
        size_t dataOffset = 0;
        auto drc = decode_pldm_pdr_repository_chg_event_data(
            eventData, eventDataSize, &eventDataFormat, &numberOfChangeRecords,
            &dataOffset);
        if (drc != PLDM_SUCCESS)
        {
            lg2::error(
                "PDRRepositoryChgEvent: decode failed for tid={TID}, rc={RC}",
                "TID", tid, "RC", drc);
            platformEventStatus = PLDM_EVENT_LOGGING_REJECTED;
            return PLDM_ERROR;
        }

        bool refreshAll = (eventDataFormat == REFRESH_ENTIRE_REPOSITORY);
        std::vector<std::pair<uint8_t, std::vector<uint32_t>>> changeRecords;

        if (!refreshAll && eventDataFormat == FORMAT_IS_PDR_HANDLES)
        {
            const uint8_t* recData = eventData + dataOffset;
            size_t recSize = eventDataSize - dataOffset;
            while (recSize)
            {
                uint8_t op = 0;
                uint8_t numEntries = 0;
                size_t recOffset = 0;
                drc = decode_pldm_pdr_repository_change_record_data(
                    recData, recSize, &op, &numEntries, &recOffset);
                if (drc != PLDM_SUCCESS)
                {
                    lg2::error(
                        "PDRRepositoryChgEvent: change-record decode failed, rc={RC}",
                        "RC", drc);
                    break;
                }
                std::vector<uint32_t> handles;
                auto handlePtr =
                    reinterpret_cast<const uint32_t*>(recData + recOffset);
                handles.reserve(numEntries);
                for (uint8_t i = 0; i < numEntries; ++i)
                {
                    handles.push_back(le32toh(handlePtr[i]));
                }
                lg2::info(
                    "PDRRepositoryChgEvent: tid={TID} changeRecord op={OP} numHandles={N}",
                    "TID", tid, "OP", op, "N", numEntries);
                changeRecords.emplace_back(op, std::move(handles));
                size_t advance = recOffset + (numEntries * sizeof(uint32_t));
                if (advance == 0 || advance > recSize)
                {
                    break;
                }
                recData += advance;
                recSize -= advance;
            }
        }
        else if (!refreshAll)
        {
            lg2::error(
                "PDRRepositoryChgEvent: unsupported eventDataFormat={FMT} for tid={TID}, ignoring",
                "FMT", eventDataFormat, "TID", tid);
            platformEventStatus = PLDM_EVENT_LOGGING_REJECTED;
            return PLDM_ERROR;
        }

        lg2::info(
            "PDRRepositoryChgEvent: tid={TID}, refreshAll={REFRESH}, changeRecords={N}",
            "TID", tid, "REFRESH", refreshAll, "N", changeRecords.size());

        stdexec::start_detached(
            processPdrRepositoryChgEvent(tid, refreshAll,
                                         std::move(changeRecords)),
            exec::default_task_context<int>(exec::inline_scheduler{}));
    }
    else
    {
        lg2::info("unhandled event, event class={EVENTCLASS}", "EVENTCLASS",
                  eventClass);
        platformEventStatus = PLDM_EVENT_LOGGING_REJECTED;
    }
    return PLDM_SUCCESS;
}

exec::task<int> EventManager::pollForPlatformEventTask(
    tid_t tid, uint16_t maxBufferSize, uint32_t dataTransferHandle)
{
    uint8_t rc = 0;
    uint8_t transferOperationFlag = PLDM_GET_FIRSTPART;
    // dataTransferHandle is seeded by the caller from the terminus' poll event
    // (0 when none was advertised) and then driven by the response handles.
    uint32_t eventIdToAcknowledge = 0;

    uint8_t completionCode = 0;
    uint8_t eventTid = 0;
    uint8_t formatVersion = 0x1; // Constant, no need to reset
    uint16_t eventId = 0xffff;
    uint32_t nextDataTransferHandle = 0;
    uint8_t transferFlag = 0;
    uint8_t eventClass = 0;
    std::vector<uint8_t> eventMessage{};
    uint32_t eventDataSize = maxBufferSize;
    std::vector<uint8_t> eventData(eventDataSize);
    uint32_t eventDataIntegrityChecksum = 0;
    while (eventId != 0)
    {
        rc = co_await pollForPlatformEventMessage(
            tid, formatVersion, transferOperationFlag, dataTransferHandle,
            eventIdToAcknowledge, completionCode, eventTid, eventId,
            nextDataTransferHandle, transferFlag, eventClass, eventDataSize,
            eventData, eventDataIntegrityChecksum);
        if (rc)
        {
            lg2::error(
                "pollForPlatformEventMessage failed. tid={TID} transferOpFlag={OPFLAG} rc={RC}",
                "TID", tid, "OPFLAG", transferOperationFlag, "RC", rc);
            co_return rc;
        }

        if (completionCode != PLDM_SUCCESS)
        {
            lg2::error(
                "pollForPlatformEventMessage failed. tid={TID} transferOpFlag={OPFLAG} cc={CC}",
                "TID", tid, "OPFLAG", transferOperationFlag, "CC",
                completionCode);
            co_return completionCode;
        }

        if (eventDataSize > 0)
        {
            eventMessage.insert(eventMessage.end(), eventData.begin(),
                                eventData.begin() + eventDataSize);
        }

        if (transferOperationFlag == PLDM_ACKNOWLEDGEMENT_ONLY)
        {
            if (eventId == 0xffff)
            {
                transferOperationFlag = PLDM_GET_FIRSTPART;
                dataTransferHandle = 0;
                eventIdToAcknowledge = 0;
                eventMessage.clear();
            }
        }
        else
        {
            /*
             * Check if transferFlag represents either a START or MIDDLE state.
             * For backward compatibility, handle values from both transfer flag
             * enums: PLATFORM_EVENT and PLDM. Note: PLATFORM_EVENT_MIDDLE and
             * PLDM_START both have the value 1.
             */
            if (transferFlag == PLDM_PLATFORM_TRANSFER_START ||
                transferFlag == PLDM_PLATFORM_TRANSFER_MIDDLE ||
                transferFlag == PLDM_MIDDLE)
            {
                transferOperationFlag = PLDM_GET_NEXTPART;
                dataTransferHandle = nextDataTransferHandle;
                eventIdToAcknowledge = 0xffff;
            }
            else
            {
                uint8_t platformEventStatus = PLDM_EVENT_NO_LOGGING;
                if (transferFlag == PLDM_PLATFORM_TRANSFER_START_AND_END)
                {
                    handlePlatformEvent(
                        eventTid, eventClass, eventMessage.data(),
                        eventMessage.size(), platformEventStatus);
                }
                else if (transferFlag == PLDM_PLATFORM_TRANSFER_END)
                {
                    if (eventDataIntegrityChecksum ==
                        pldm_edac_crc32(eventMessage.data(),
                                        eventMessage.size()))
                    {
                        handlePlatformEvent(
                            eventTid, eventClass, eventMessage.data(),
                            eventMessage.size(), platformEventStatus);
                    }
                    else
                    {
                        lg2::error(
                            "pollForPlatformEventMessage checksum error, tid={TID} eventId={EVENTID} eventClass={EVENTCLASS} ",
                            "TID", tid, "EVENTID", eventId, "EVENTCLASS",
                            eventClass);
                    }
                }

                transferOperationFlag = PLDM_ACKNOWLEDGEMENT_ONLY;
                dataTransferHandle = 0;
                eventIdToAcknowledge = eventId;
            }
        }
    }

    co_return PLDM_SUCCESS;
}

exec::task<int> EventManager::pollForPlatformEventMessage(
    tid_t tid, uint8_t formatVersion, uint8_t transferOperationFlag,
    uint32_t dataTransferHandle, uint16_t eventIdToAcknowledge,
    uint8_t& completionCode, uint8_t& eventTid, uint16_t& eventId,
    uint32_t& nextDataTransferHandle, uint8_t& transferFlag,
    uint8_t& eventClass, uint32_t& eventDataSize,
    std::vector<uint8_t>& eventData, uint32_t& eventDataIntegrityChecksum)
{
    Request request(
        sizeof(pldm_msg_hdr) + PLDM_POLL_FOR_PLATFORM_EVENT_MESSAGE_REQ_BYTES);
    auto requestMsg = new (request.data()) pldm_msg;

    auto rc = encode_poll_for_platform_event_message_req(
        0, formatVersion, transferOperationFlag, dataTransferHandle,
        eventIdToAcknowledge, requestMsg, request.size());

    if (rc)
    {
        lg2::error(
            "Failed to encode request PollForPlatformEventMessage for terminus ID {TID}, error {RC} ",
            "TID", tid, "RC", rc);
        co_return rc;
    }

    const pldm_msg* responseMsg = nullptr;
    size_t responseLen = 0;

    rc = co_await terminusManager.SendRecvPldmMsg(tid, request, &responseMsg,
                                                  &responseLen);
    if (rc)
    {
        lg2::error(
            "Failed to send PollForPlatformEventMessage message for terminus {TID}, error {RC}",
            "TID", tid, "RC", rc);
        co_return rc;
    }

    // Temp pointer to decoded event data
    uint8_t* rawEventData = nullptr;

    rc = decode_poll_for_platform_event_message_resp(
        responseMsg, responseLen, &completionCode, &eventTid, &eventId,
        &nextDataTransferHandle, &transferFlag, &eventClass, &eventDataSize,
        reinterpret_cast<void**>(&rawEventData), &eventDataIntegrityChecksum);

    if (rc)
    {
        lg2::error(
            "Failed to decode response PollForPlatformEventMessage for terminus ID {TID}, error {RC} ",
            "TID", tid, "RC", rc);
        co_return rc;
    }

    if (completionCode != PLDM_SUCCESS)
    {
        lg2::error(
            "Error : PollForPlatformEventMessage for terminus ID {TID}, complete code {CC}.",
            "TID", tid, "CC", completionCode);
        co_return rc;
    }

    if (rawEventData)
    {
        // Copy event data
        eventData.assign(rawEventData, rawEventData + eventDataSize);
    }

    co_return completionCode;
}

auto asioCallback =
    [](const boost::system::error_code& ec, sdbusplus::message::message& msg) {
        if (ec)
        {
            lg2::error("Error notifying CPER Logger, {ERROR}.", "ERROR",
                       msg.get_errno());
        }
    };

void EventManager::notifyCPERLogger(std::span<const unsigned char> data)
{
    static constexpr auto loggerObj = "/xyz/openbmc_project/cperlogger";
    static constexpr auto loggerIntf = "xyz.openbmc_project.CPER";
    auto& conn = pldm::utils::DBusHandler::getAsioConnection();

    try
    {
        auto service =
            pldm::utils::DBusHandler().getService(loggerObj, loggerIntf);
        conn->async_method_call(asioCallback, service.c_str(), loggerObj,
                                loggerIntf, "CreateLog", data);
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to notify CPER Logger, {ERROR}.", "ERROR", e);
    }
    return;
}

void EventManager::createSensorThresholdLogEntry(
    const std::string& messageId, const std::string& sensorName,
    const double reading, const double threshold, const std::string& eventId,
    const std::string& impactedComponent)
{
    using namespace sdbusplus::xyz::openbmc_project::Logging::server;
    using Level =
        sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level;

    auto createLog = [&messageId](std::map<std::string, std::string>& addData,
                                  Level& level) {
        static constexpr auto logObjPath = "/xyz/openbmc_project/logging";
        static constexpr auto logInterface =
            "xyz.openbmc_project.Logging.Create";
        auto& bus = pldm::utils::DBusHandler::getBus();

        try
        {
            auto service =
                pldm::utils::DBusHandler().getService(logObjPath, logInterface);
            auto severity = sdbusplus::xyz::openbmc_project::Logging::server::
                convertForMessage(level);
            auto method = bus.new_method_call(service.c_str(), logObjPath,
                                              logInterface, "Create");
            method.append(messageId, severity, addData);
            bus.call_noreply(method);
        }
        catch (const std::exception& e)
        {
            lg2::error(
                "Failed to create D-Bus log entry for message registry, {ERROR}.",
                "ERROR", e);
        }
    };

    std::map<std::string, std::string> addData;
    addData["REDFISH_MESSAGE_ID"] = messageId;
    Level level = Level::Informational;

    addData["REDFISH_MESSAGE_ARGS"] =
        sensorName + "," + std::to_string(reading) + "," +
        std::to_string(threshold);
#ifdef OEM_NVIDIA
    if (!eventId.empty())
    {
        addData["xyz.openbmc_project.Logging.Entry.EventId"] = eventId;
    }
    if (!impactedComponent.empty())
    {
        addData["DEVICE_NAME"] = impactedComponent;
    }
#endif

    if (messageId == SensorThresholdWarningLowGoingHigh ||
        messageId == SensorThresholdWarningHighGoingLow ||
        messageId == SensorThresholdCriticalLowGoingHigh ||
        messageId == SensorThresholdCriticalHighGoingLow)
    {
        // Recovery / deassert transitions (sensor moving back toward Normal)
        level = Level::Informational;
    }
    else if (messageId == SensorThresholdWarningLowGoingLow ||
             messageId == SensorThresholdWarningHighGoingHigh)
    {
        level = Level::Warning;
    }
    else if (messageId == SensorThresholdCriticalLowGoingLow ||
             messageId == SensorThresholdCriticalHighGoingHigh)
    {
        level = Level::Critical;
    }
    else
    {
        lg2::error("Message Registry messageID is not recognised, {MESSAGEID}",
                   "MESSAGEID", messageId);
        return;
    }

    createLog(addData, level);
    return;
}

void EventManager::processNumericSensorEvent(tid_t tid, uint16_t sensorId,
                                             const uint8_t* sensorData,
                                             size_t sensorDataLength)
{
    pldm_numeric_sensor_event_data eventData{};
    auto rc = decode_numeric_sensor_event_data(sensorData, sensorDataLength,
                                               &eventData);
    if (rc != PLDM_SUCCESS)
    {
        lg2::error(
            "Failed to decode numeric sensor event data for TID {TID}, Sensor ID {SENSOR}, rc={RC}",
            "TID", tid, "SENSOR", sensorId, "RC", rc);
        return;
    }

    uint8_t eventState = eventData.event_state;
    uint8_t previousEventState = eventData.previous_event_state;
    uint8_t sensorDataSize = eventData.sensor_data_size;
    union_sensor_data_size presentReading = eventData.present_reading;

    for (auto& [terminusId, terminus] : termini)
    {
        if (tid != terminusId)
        {
            continue;
        }
        for (auto& sensor : terminus->numericSensors)
        {
            if (sensorId != sensor->sensorId)
            {
                continue;
            }

            auto sensorEventInfo = sensor->getSensorEventInfo();
            auto [messageId, eventId, impactedComponent] =
                getSensorThresholdEventData(previousEventState, eventState,
                                            sensorEventInfo);
            double threshold = std::numeric_limits<double>::quiet_NaN();
            double reading = std::numeric_limits<double>::quiet_NaN();

            if (messageId == SensorThresholdWarningHighGoingLow ||
                messageId == SensorThresholdWarningHighGoingHigh)
            {
                threshold = sensor->getThresholdUpperWarning();
            }
            else if (messageId == SensorThresholdCriticalHighGoingHigh ||
                     messageId == SensorThresholdCriticalHighGoingLow)
            {
                threshold = sensor->getThresholdUpperCritical();
            }
            else if (messageId == SensorThresholdWarningLowGoingHigh ||
                     messageId == SensorThresholdWarningLowGoingLow)
            {
                threshold = sensor->getThresholdLowerWarning();
            }
            else if (messageId == SensorThresholdCriticalLowGoingLow ||
                     messageId == SensorThresholdCriticalLowGoingHigh)
            {
                threshold = sensor->getThresholdLowerCritical();
            }

            switch (sensorDataSize)
            {
                case PLDM_SENSOR_DATA_SIZE_UINT8:
                    reading = static_cast<double>(presentReading.value_u8);
                    break;
                case PLDM_SENSOR_DATA_SIZE_SINT8:
                    reading = static_cast<double>(presentReading.value_s8);
                    break;
                case PLDM_SENSOR_DATA_SIZE_UINT16:
                    reading = static_cast<double>(presentReading.value_u16);
                    break;
                case PLDM_SENSOR_DATA_SIZE_SINT16:
                    reading = static_cast<double>(presentReading.value_s16);
                    break;
                case PLDM_SENSOR_DATA_SIZE_UINT32:
                    reading = static_cast<double>(presentReading.value_u32);
                    break;
                case PLDM_SENSOR_DATA_SIZE_SINT32:
                    reading = static_cast<double>(presentReading.value_s32);
                    break;
                case PLDM_SENSOR_DATA_SIZE_UINT64:
                    reading = static_cast<double>(presentReading.value_u64);
                    break;
                case PLDM_SENSOR_DATA_SIZE_SINT64:
                    reading = static_cast<double>(presentReading.value_s64);
                    break;
                default:
                    break;
            }
            createSensorThresholdLogEntry(
                messageId, sensor->getSensorName(),
                sensor->unitModifier(sensor->conversionFormula(reading)),
                threshold, eventId, impactedComponent);
        }
    }
}

std::tuple<std::string, std::string, std::string>
    EventManager::getSensorThresholdEventData(
        uint8_t previousEventState, uint8_t eventState,
        std::shared_ptr<utils::SensorEventInfo> sensorEventInfo)
{
    std::string messageId;
    std::string eventId;
    std::string impactedComponent;
    switch (previousEventState)
    {
        case PLDM_SENSOR_UPPERFATAL:
        case PLDM_SENSOR_UPPERCRITICAL:
            switch (eventState)
            {
                case PLDM_SENSOR_UPPERFATAL:
                case PLDM_SENSOR_UPPERCRITICAL:
                    messageId = SensorThresholdCriticalHighGoingHigh;
                    break;
                case PLDM_SENSOR_UPPERWARNING:
                    messageId = SensorThresholdCriticalHighGoingLow;
                    break;
                case PLDM_SENSOR_NORMAL:
                    messageId = SensorThresholdWarningHighGoingLow;
                    break;
                case PLDM_SENSOR_LOWERWARNING:
                    messageId = SensorThresholdWarningLowGoingLow;
                    break;
                case PLDM_SENSOR_LOWERCRITICAL:
                case PLDM_SENSOR_LOWERFATAL:
                    messageId = SensorThresholdCriticalLowGoingLow;
                    break;
                default:
                    break;
            }
            break;
        case PLDM_SENSOR_UPPERWARNING:
            switch (eventState)
            {
                case PLDM_SENSOR_UPPERFATAL:
                case PLDM_SENSOR_UPPERCRITICAL:
                    messageId = SensorThresholdCriticalHighGoingHigh;
                    break;
                case PLDM_SENSOR_UPPERWARNING:
                    messageId = SensorThresholdWarningHighGoingHigh;
                    break;
                case PLDM_SENSOR_NORMAL:
                    messageId = SensorThresholdWarningHighGoingLow;
                    break;
                case PLDM_SENSOR_LOWERWARNING:
                    messageId = SensorThresholdWarningLowGoingLow;
                    break;
                case PLDM_SENSOR_LOWERCRITICAL:
                case PLDM_SENSOR_LOWERFATAL:
                    messageId = SensorThresholdCriticalLowGoingLow;
                    break;
                default:
                    break;
            }
            break;
        case PLDM_SENSOR_UNKNOWN:
        case PLDM_SENSOR_NORMAL:
            switch (eventState)
            {
                case PLDM_SENSOR_UPPERFATAL:
                case PLDM_SENSOR_UPPERCRITICAL:
                    messageId = SensorThresholdCriticalHighGoingHigh;
                    break;
                case PLDM_SENSOR_UPPERWARNING:
                    messageId = SensorThresholdWarningHighGoingHigh;
                    break;
                case PLDM_SENSOR_NORMAL:
                    break;
                case PLDM_SENSOR_LOWERWARNING:
                    messageId = SensorThresholdWarningLowGoingLow;
                    break;
                case PLDM_SENSOR_LOWERCRITICAL:
                case PLDM_SENSOR_LOWERFATAL:
                    messageId = SensorThresholdCriticalLowGoingLow;
                    break;
                default:
                    break;
            }
            break;
        case PLDM_SENSOR_LOWERWARNING:
            switch (eventState)
            {
                case PLDM_SENSOR_UPPERFATAL:
                case PLDM_SENSOR_UPPERCRITICAL:
                    messageId = SensorThresholdCriticalHighGoingHigh;
                    break;
                case PLDM_SENSOR_UPPERWARNING:
                    messageId = SensorThresholdWarningHighGoingHigh;
                    break;
                case PLDM_SENSOR_NORMAL:
                    messageId = SensorThresholdWarningLowGoingHigh;
                    break;
                case PLDM_SENSOR_LOWERWARNING:
                    messageId = SensorThresholdWarningLowGoingLow;
                    break;
                case PLDM_SENSOR_LOWERCRITICAL:
                case PLDM_SENSOR_LOWERFATAL:
                    messageId = SensorThresholdCriticalLowGoingLow;
                    break;
                default:
                    break;
            }
            break;
        case PLDM_SENSOR_LOWERCRITICAL:
        case PLDM_SENSOR_LOWERFATAL:
            switch (eventState)
            {
                case PLDM_SENSOR_UPPERFATAL:
                case PLDM_SENSOR_UPPERCRITICAL:
                    messageId = SensorThresholdCriticalHighGoingHigh;
                    break;
                case PLDM_SENSOR_UPPERWARNING:
                    messageId = SensorThresholdWarningHighGoingHigh;
                    break;
                case PLDM_SENSOR_NORMAL:
                    messageId = SensorThresholdWarningLowGoingHigh;
                    break;
                case PLDM_SENSOR_LOWERWARNING:
                    messageId = SensorThresholdCriticalLowGoingHigh;
                    break;
                case PLDM_SENSOR_LOWERCRITICAL:
                case PLDM_SENSOR_LOWERFATAL:
                    messageId = SensorThresholdCriticalLowGoingLow;
                    break;
                default:
                    break;
            }
            break;
    }

    if (sensorEventInfo)
    {
        switch (eventState)
        {
            case PLDM_SENSOR_UPPERFATAL:
            {
                auto it =
                    sensorEventInfo->eventIdsMap.find("PLDM_SENSOR_UPPERFATAL");
                if (it != sensorEventInfo->eventIdsMap.end())
                {
                    eventId = it->second;
                }
                impactedComponent = sensorEventInfo->impactedComponent;
                break;
            }
            case PLDM_SENSOR_UPPERCRITICAL:
            {
                auto it = sensorEventInfo->eventIdsMap.find(
                    "PLDM_SENSOR_UPPERCRITICAL");
                if (it != sensorEventInfo->eventIdsMap.end())
                {
                    eventId = it->second;
                }
                impactedComponent = sensorEventInfo->impactedComponent;
                break;
            }
            case PLDM_SENSOR_UPPERWARNING:
            {
                auto it = sensorEventInfo->eventIdsMap.find(
                    "PLDM_SENSOR_UPPERWARNING");
                if (it != sensorEventInfo->eventIdsMap.end())
                {
                    eventId = it->second;
                }
                impactedComponent = sensorEventInfo->impactedComponent;
                break;
            }
            case PLDM_SENSOR_LOWERWARNING:
            {
                auto it = sensorEventInfo->eventIdsMap.find(
                    "PLDM_SENSOR_LOWERWARNING");
                if (it != sensorEventInfo->eventIdsMap.end())
                {
                    eventId = it->second;
                }
                impactedComponent = sensorEventInfo->impactedComponent;
                break;
            }
            case PLDM_SENSOR_LOWERCRITICAL:
            {
                auto it = sensorEventInfo->eventIdsMap.find(
                    "PLDM_SENSOR_LOWERCRITICAL");
                if (it != sensorEventInfo->eventIdsMap.end())
                {
                    eventId = it->second;
                }
                impactedComponent = sensorEventInfo->impactedComponent;
                break;
            }
            case PLDM_SENSOR_LOWERFATAL:
            {
                auto it =
                    sensorEventInfo->eventIdsMap.find("PLDM_SENSOR_LOWERFATAL");
                if (it != sensorEventInfo->eventIdsMap.end())
                {
                    eventId = it->second;
                }
                impactedComponent = sensorEventInfo->impactedComponent;
                break;
            }
            default:
                break;
        }
    }
    return std::make_tuple(messageId, eventId, impactedComponent);
}

void EventManager::processStateSensorEvent(tid_t tid, uint16_t sensorId,
                                           const uint8_t* sensorData,
                                           size_t sensorDataLength)

{
    uint8_t sensorOffset;
    uint8_t eventState;
    uint8_t previousEventState;
    auto rc =
        decode_state_sensor_data(sensorData, sensorDataLength, &sensorOffset,
                                 &eventState, &previousEventState);
    if (rc != PLDM_SUCCESS)
    {
        lg2::error("failed to decode received state sensor event,sid={SID}.",
                   "SID", sensorId);
        return;
    }

    auto it = termini.find(tid);
    if (it == termini.end())
    {
        lg2::info(
            "received a state sensor event,sid={SID}, with invalid tid={TID}",
            "SID", sensorId, "TID", tid);
        return;
    }

    auto terminus = get<1>(*it);
    auto sensorIterator = std::find_if(
        terminus->stateSensors.begin(), terminus->stateSensors.end(),
        [&sensorId](auto& sensor) { return sensor->sensorId == sensorId; });
    if (sensorIterator == terminus->stateSensors.end())
    {
        lg2::error("processStateSensorEvent: sensor id, {SENSORID}, not found.",
                   "SENSORID", sensorId);
        return;
    }
    (*sensorIterator)
        ->handleSensorEvent(sensorOffset, eventState, previousEventState);
}

void EventManager::processTelemetryPauseEvent(tid_t tid)
{
    lg2::info("Processing telemetry pause event for tid={TID}", "TID", tid);

    auto it = termini.find(tid);
    if (it == termini.end())
    {
        lg2::error("processTelemetryPauseEvent: terminus tid={TID} not found",
                   "TID", tid);
        return;
    }

    // Stop sensor polling for this terminus completely
    // This terminates the polling task and frees resources
    sensorManager.stopPolling(tid);

    lg2::info(
        "Telemetry monitoring stopped for terminus tid={TID}, waiting for rediscovery event",
        "TID", tid);
}

exec::task<int> EventManager::processTelemetryRediscoveryEvent(tid_t tid)
{
    lg2::info("Processing telemetry rediscovery event for tid={TID}", "TID",
              tid);

    auto it = termini.find(tid);
    if (it == termini.end())
    {
        lg2::error(
            "processTelemetryRediscoveryEvent: terminus tid={TID} not found",
            "TID", tid);
        co_return PLDM_ERROR;
    }

    auto terminus = it->second;

    // Run the whole teardown + re-discovery under a catch-all: this coroutine
    // is spawned detached (start_detached), so an escaping exception would hit
    // std::terminate and abort pldmd instead of just failing the rebuild.
    try
    {
        // Stop polling - the coroutine will exit asynchronously
        sensorManager.stopPolling(tid);

        // Clear prioritySensors and roundRobinSensors to release sensor refs
        terminus->prioritySensors.clear();

        // Clear roundRobinSensors queue by manually popping all elements
        // In coroutines, local variables persist in the coroutine frame, so we
        // can't use std::swap with a local variable. Manually pop to
        // immediately release references.
        size_t queueSize = terminus->roundRobinSensors.size();
        for (size_t i = 0; i < queueSize; ++i)
        {
            terminus->roundRobinSensors.pop();
        }

        // Wait for the polling coroutine to exit and release all sensor
        // references. stopPolling() only requests the stop; the coroutine may
        // still be processing sensors. Both numeric AND state sensors are held
        // by the priority / round-robin lists, so both must reach use_count==1
        // (held only by their owning vector) before clear() so destructors —
        // and the D-Bus unregistration they trigger — run immediately.
        constexpr int maxRetries = 30;              // Up to 15 seconds
        constexpr uint64_t retryDelayUsec = 500000; // 500ms per check
        bool allReferencesReleased = false;

        for (int retry = 0; retry < maxRetries; ++retry)
        {
            int sensorsWithExtraRefs = 0;

            for (const auto& sensor : terminus->numericSensors)
            {
                if (sensor && sensor.use_count() > 1)
                {
                    sensorsWithExtraRefs++;
                }
            }
            for (const auto& sensor : terminus->stateSensors)
            {
                if (sensor && sensor.use_count() > 1)
                {
                    sensorsWithExtraRefs++;
                }
            }

            if (sensorsWithExtraRefs == 0)
            {
                allReferencesReleased = true;
                break;
            }

            if (retry % 4 == 0)
            {
                lg2::info(
                    "Waiting for polling coroutine to release sensor references: {COUNT} sensor(s) still in use",
                    "COUNT", sensorsWithExtraRefs);
            }

            co_await timer::Sleep(terminusManager.getEvent(), retryDelayUsec,
                                  timer::NonPriority);
        }

        if (!allReferencesReleased)
        {
            lg2::error(
                "Timed out waiting for sensor references to be released after {TIME}s for tid={TID}",
                "TIME", maxRetries * 500 / 1000, "TID", tid);
        }

        // Clear all Type 2 objects - sensors, effecters, raw PDRs.
        // D-Bus interface cleanup happens automatically in sensor/effecter
        // destructors.
        terminus->numericSensors.clear();
        terminus->stateSensors.clear();
        terminus->numericEffecters.clear();
        terminus->stateEffecters.clear();
        terminus->pdrs.clear();

        // Also reset the PARSED-PDR caches. initTerminus() -> parsePDRs()
        // re-parses the freshly fetched raw PDRs by APPENDING to these caches
        // and then creates a D-Bus object for every cache entry. If the stale
        // entries are left here, every sensor/effecter is created twice on
        // re-init and the duplicate collides (sd_bus FileExists), leaving the
        // rebuilt sensors stale. Reset them so re-init starts from empty.
        terminus->clearParsedPdrCaches();

        // Mark terminus as not initialized so it will be re-initialized
        terminus->initalized = false;
        terminus->resumed = false;
        terminus->initSensorList = true;

        lg2::info("Cleared Type 2 telemetry objects for tid={TID}", "TID", tid);

        // Wait for D-Bus to complete asynchronous unregistration
        // After destructors are called (during clear() above), D-Bus still
        // needs time to process the unregistration messages asynchronously. The
        // D-Bus daemon processes unregister requests asynchronously with no way
        // to poll for completion, so we use a conservative fixed delay to
        // ensure all objects are fully unregistered before creating new ones.
        constexpr uint64_t dbusCleanupDelayUsec = 10000000; // 10 seconds

        co_await timer::Sleep(terminusManager.getEvent(), dbusCleanupDelayUsec,
                              timer::NonPriority);

        // Re-initialize ONLY this terminus. Using the global initTerminus()
        // here races concurrent rebuilds of other termini: each rebuild marks
        // its own terminus initalized=false, so a global re-init re-creates
        // those objects too — twice when both rebuilds run — causing D-Bus
        // FileExists. Scoping the re-init to tid removes that cross-terminus
        // collision.
        auto rc = co_await platformManager.initTerminus(tid);

        if (rc != PLDM_SUCCESS)
        {
            lg2::error(
                "Failed to reinitialize terminus tid={TID} after rediscovery, rc={RC}",
                "TID", tid, "RC", rc);
            sensorManager.startPolling(tid);
            co_return rc;
        }

        // Restart sensor polling for this terminus with new PDRs
        sensorManager.startPolling(tid);
        terminus->resumed = true;

        lg2::info("Type 2 telemetry rediscovery completed for tid={TID}", "TID",
                  tid);

        co_return PLDM_SUCCESS;
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "processTelemetryRediscoveryEvent: unhandled exception for tid={TID}, {ERROR}",
            "TID", tid, "ERROR", e);
        // Best-effort: resume polling so the terminus is not left dark.
        sensorManager.startPolling(tid);
        co_return PLDM_ERROR;
    }
}

exec::task<int> EventManager::processPdrRepositoryChgEvent(
    tid_t tid, bool refreshAll,
    std::vector<std::pair<uint8_t, std::vector<uint32_t>>> changeRecords)
{
    auto it = termini.find(tid);
    if (it == termini.end())
    {
        lg2::error("processPdrRepositoryChgEvent: terminus tid={TID} not found",
                   "TID", tid);
        co_return PLDM_ERROR;
    }
    auto terminus = it->second;

    // Serialize rebuilds per terminus. This coroutine is spawned detached, so a
    // second PDRRepositoryChgEvent for the same tid (devices burst these during
    // power-on) could race the first on the shared termini map and the D-Bus
    // object lifecycle. If a rebuild is already running for this tid, skip.
    if (rebuildInProgress.contains(tid))
    {
        lg2::info(
            "processPdrRepositoryChgEvent: rebuild already in progress for tid={TID}, skipping this event",
            "TID", tid);
        co_return PLDM_SUCCESS;
    }
    rebuildInProgress.insert(tid);
    // Erase the in-progress marker on every exit path (normal co_return or an
    // exception unwinding the coroutine frame destroys this guard).
    struct RebuildGuard
    {
        std::unordered_set<tid_t>& set;
        tid_t tid;
        ~RebuildGuard()
        {
            set.erase(tid);
        }
    } rebuildGuard{rebuildInProgress, tid};

    // Run under a catch-all: an exception escaping this detached coroutine
    // would hit std::terminate and abort pldmd instead of just failing the
    // rebuild.
    try
    {
        // Classify the change records. recordsAdded and recordsModified on an
        // already-initialized terminus are handled incrementally (fetch the
        // affected PDRs and create/replace only those objects). refreshAll, any
        // recordsDeleted, or an event before initial discovery completes need
        // the full clear + re-discovery teardown so stale objects are removed
        // and changed PDRs re-parsed consistently.
        bool needFullRebuild = refreshAll || !terminus->initalized;
        std::vector<uint32_t> addedHandles;
        std::vector<uint32_t> modifiedHandles;
        for (const auto& [op, handles] : changeRecords)
        {
            if (op == PLDM_RECORDS_ADDED)
            {
                addedHandles.insert(addedHandles.end(), handles.begin(),
                                    handles.end());
            }
            else if (op == PLDM_RECORDS_MODIFIED)
            {
                modifiedHandles.insert(modifiedHandles.end(), handles.begin(),
                                       handles.end());
            }
            else if (op == PLDM_RECORDS_DELETED)
            {
                // Removing specific objects for deleted handles incrementally
                // is not supported; a full rebuild drops the stale objects.
                needFullRebuild = true;
            }
        }

        if (needFullRebuild)
        {
            lg2::info(
                "processPdrRepositoryChgEvent: tid={TID} requires full rebuild (refreshAll={REFRESH}, initialized={INIT})",
                "TID", tid, "REFRESH", refreshAll, "INIT",
                terminus->initalized);
            co_return co_await processTelemetryRediscoveryEvent(tid);
        }

        if (addedHandles.empty() && modifiedHandles.empty())
        {
            lg2::info(
                "processPdrRepositoryChgEvent: tid={TID} no added/modified records to fetch",
                "TID", tid);
            co_return PLDM_SUCCESS;
        }

        lg2::info(
            "processPdrRepositoryChgEvent: tid={TID} fetching {NA} added + {NM} modified PDR(s)",
            "TID", tid, "NA", addedHandles.size(), "NM",
            modifiedHandles.size());

        std::vector<std::vector<uint8_t>> addedPdrs =
            co_await fetchPdrsByHandles(tid, addedHandles);
        std::vector<std::vector<uint8_t>> modifiedPdrs =
            co_await fetchPdrsByHandles(tid, modifiedHandles);

        if (addedPdrs.empty() && modifiedPdrs.empty())
        {
            lg2::error(
                "processPdrRepositoryChgEvent: tid={TID} no PDRs successfully fetched",
                "TID", tid);
            co_return PLDM_ERROR;
        }

        // Drop modified PDRs of a type platform-mc does not consume (no
        // derived D-Bus object, e.g. PLDM_OEM_DEVICE_PDR). Modifying such a
        // PDR changes nothing we expose, so it needs neither an incremental
        // replace nor a full rebuild.
        std::erase_if(modifiedPdrs, [tid](const std::vector<uint8_t>& pdr) {
            uint8_t type =
                pdr.size() >= sizeof(pldm_pdr_hdr)
                    ? reinterpret_cast<const pldm_pdr_hdr*>(pdr.data())->type
                    : 0;
            if (!Terminus::pdrTypeConsumed(type))
            {
                lg2::info(
                    "processPdrRepositoryChgEvent: tid={TID} ignoring modified PDR type={TYPE} (no derived object)",
                    "TID", tid, "TYPE", type);
                return true;
            }
            return false;
        });

        if (addedPdrs.empty() && modifiedPdrs.empty())
        {
            lg2::info(
                "processPdrRepositoryChgEvent: tid={TID} no actionable PDRs after filtering, nothing to do",
                "TID", tid);
            co_return PLDM_SUCCESS;
        }

        // A MODIFIED record only maps cleanly to a per-object replace when its
        // PDR owns a single object (numeric/state sensor or effecter, or the
        // OEM energy-count sensor). Auxiliary-name, entity-association and
        // other OEM PDRs are referenced by many objects, so fall back to a full
        // rebuild if any modified PDR is not 1:1 replaceable.
        for (const auto& pdr : modifiedPdrs)
        {
            if (!terminus->canReplacePdrIncrementally(pdr))
            {
                uint8_t type =
                    pdr.size() >= sizeof(pldm_pdr_hdr)
                        ? reinterpret_cast<const pldm_pdr_hdr*>(pdr.data())
                              ->type
                        : 0;
                int oemSubType = terminus->oemPdrSubType(pdr);
                lg2::info(
                    "processPdrRepositoryChgEvent: tid={TID} modified PDR type={TYPE} oemSubType={SUB} not 1:1 replaceable, falling back to full rebuild",
                    "TID", tid, "TYPE", type, "SUB", oemSubType);
                co_return co_await processTelemetryRediscoveryEvent(tid);
            }
        }

        // To replace modified objects, the old sensor objects must be fully
        // released (including the polling priority / round-robin references)
        // before re-creation, or the lingering D-Bus interface collides
        // (FileExists). Quiesce polling and drain references, remove the old
        // objects, then let D-Bus settle the async unregistration.
        if (!modifiedPdrs.empty())
        {
            co_await quiesceSensorPolling(terminus, tid);
            terminus->removeModifiedPdrObjects(modifiedPdrs);

            constexpr uint64_t dbusCleanupDelayUsec = 10000000; // 10 seconds
            co_await timer::Sleep(terminusManager.getEvent(),
                                  dbusCleanupDelayUsec, timer::NonPriority);
        }

        // Parse + create objects for both the added and (re-added) modified
        // PDRs. addNewPdrs only creates objects for the newly appended cache
        // entries, so it never re-adds objects that already exist.
        std::vector<std::vector<uint8_t>> newPdrs = std::move(addedPdrs);
        newPdrs.insert(newPdrs.end(), modifiedPdrs.begin(), modifiedPdrs.end());
        size_t applied = terminus->addNewPdrs(newPdrs);

        // Nothing actually changed (every added PDR was already present and no
        // modified PDR was replaced), so skip the association/polling refresh.
        // A terminus re-reports its existing sensors as ADDED on every
        // power-cycle, so this is the common steady-state path.
        if (applied == 0 && modifiedPdrs.empty())
        {
            lg2::info(
                "processPdrRepositoryChgEvent: tid={TID} no change ({N} PDR(s) already present), skipping refresh",
                "TID", tid, "N", newPdrs.size());
            co_return PLDM_SUCCESS;
        }

        // Reconcile inventory associations so the new sensors are linked to
        // their chassis/inventory PDIs.
        co_await terminus->updateAssociations();

        // Rebuild the polling lists so new/replaced sensors are polled; resume
        // polling if it was quiesced for a modified replace.
        terminus->initSensorList = true;
        if (!modifiedPdrs.empty())
        {
            sensorManager.startPolling(tid);
        }

        lg2::info(
            "processPdrRepositoryChgEvent: tid={TID} applied {N} new PDR(s) incrementally and reconciled associations",
            "TID", tid, "N", applied);

        co_return PLDM_SUCCESS;
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "processPdrRepositoryChgEvent: unhandled exception for tid={TID}, {ERROR}",
            "TID", tid, "ERROR", e);
        co_return PLDM_ERROR;
    }
}

exec::task<std::vector<std::vector<uint8_t>>> EventManager::fetchPdrsByHandles(
    tid_t tid, const std::vector<uint32_t>& handles)
{
    std::vector<std::vector<uint8_t>> pdrs;
    for (uint32_t handle : handles)
    {
        std::vector<uint8_t> pdr;
        auto rc = co_await platformManager.fetchSinglePdr(tid, handle, pdr);
        if (rc != PLDM_SUCCESS || pdr.empty())
        {
            lg2::error(
                "fetchPdrsByHandles: failed to fetch PDR handle={HANDLE} for tid={TID}, rc={RC}",
                "HANDLE", handle, "TID", tid, "RC", rc);
            continue;
        }
        pdrs.push_back(std::move(pdr));
    }
    co_return pdrs;
}

exec::task<void> EventManager::quiesceSensorPolling(
    std::shared_ptr<Terminus> terminus, tid_t tid)
{
    // Stop polling - the coroutine will exit asynchronously
    sensorManager.stopPolling(tid);

    // Release the priority / round-robin references to the sensor objects.
    terminus->prioritySensors.clear();
    size_t queueSize = terminus->roundRobinSensors.size();
    for (size_t i = 0; i < queueSize; ++i)
    {
        terminus->roundRobinSensors.pop();
    }

    // Wait until the polling coroutine drops its in-flight references so that
    // erasing the objects afterwards runs their destructors (and the D-Bus
    // unregistration) immediately. Both numeric and state sensors can be held
    // by the polling lists.
    constexpr int maxRetries = 30;              // Up to 15 seconds
    constexpr uint64_t retryDelayUsec = 500000; // 500ms per check
    for (int retry = 0; retry < maxRetries; ++retry)
    {
        int sensorsWithExtraRefs = 0;
        for (const auto& sensor : terminus->numericSensors)
        {
            if (sensor && sensor.use_count() > 1)
            {
                sensorsWithExtraRefs++;
            }
        }
        for (const auto& sensor : terminus->stateSensors)
        {
            if (sensor && sensor.use_count() > 1)
            {
                sensorsWithExtraRefs++;
            }
        }

        if (sensorsWithExtraRefs == 0)
        {
            break;
        }

        if (retry % 4 == 0)
        {
            lg2::info(
                "quiesceSensorPolling: {COUNT} sensor(s) still in use for tid={TID}",
                "COUNT", sensorsWithExtraRefs, "TID", tid);
        }

        co_await timer::Sleep(terminusManager.getEvent(), retryDelayUsec,
                              timer::NonPriority);
    }

    co_return;
}

void EventManager::processTelemetryResumeEvent(tid_t tid)
{
    lg2::info("Processing telemetry resume event for tid={TID}", "TID", tid);

    auto it = termini.find(tid);
    if (it == termini.end())
    {
        lg2::error("processTelemetryResumeEvent: terminus tid={TID} not found",
                   "TID", tid);
        return;
    }

    // Simply restart sensor polling without clearing or reinitializing
    // Assumes PDRs and sensors are still valid
    sensorManager.startPolling(tid);

    lg2::info("Telemetry monitoring resumed for terminus tid={TID}", "TID",
              tid);
}

} // namespace platform_mc
} // namespace pldm
