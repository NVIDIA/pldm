#include "event_manager.hpp"

#include "libpldm/utils.h"
#include "platform.h"

#include "terminus_manager.hpp"

#include <phosphor-logging/lg2.hpp>
#include <xyz/openbmc_project/Logging/Entry/server.hpp>

#include <cerrno>

namespace pldm
{
namespace platform_mc
{
namespace fs = std::filesystem;

int EventManager::handlePlatformEvent(tid_t tid, uint8_t eventClass,
                                      const uint8_t* eventData,
                                      size_t eventDataSize,
                                      uint8_t& platformEventStatus)
{
    platformEventStatus = PLDM_EVENT_NO_LOGGING;
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
            lg2::error("Failed to decode sensor event data, rc={RC}.", "RC",
                       rc);
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
                uint8_t sensorOffset;
                uint8_t eventState;
                uint8_t previousEventState;
                rc = decode_state_sensor_data(
                    eventData + eventClassDataOffset,
                    eventDataSize - eventClassDataOffset, &sensorOffset,
                    &eventState, &previousEventState);
                if (rc == PLDM_SUCCESS)
                {
                    auto it = termini.find(tid);
                    if (it != termini.end())
                    {
                        auto terminus = get<1>(*it);
                        terminus->handleStateSensorEvent(sensorId, sensorOffset,
                                                         eventState);
                    }
                    else
                    {
                        lg2::info(
                            "received a state sensor event,sid={SID}, with invalid tid={TID}",
                            "SID", sensorId, "TID", tid);
                    }
                }
                else
                {
                    lg2::error(
                        "failed to decode received state sensor event,sid={SID}.",
                        "SID", sensorId);
                }
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
        lg2::info("received poll event tid={TID}", "TID", tid);
        auto it = termini.find(tid);
        if (it != termini.end())
        {
            termini[tid]->pollEvent = true;
        }
    }
    else if (eventClass == PLDM_OEM_EVENT_CLASS_0xFA)
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

        // save event data to file
        std::string dirName{"/var/cper"};
        auto dirStatus = fs::status(dirName);
        if (fs::exists(dirStatus))
        {
            if (!fs::is_directory(dirStatus))
            {
                lg2::error("Failed to create {DIRNAME} directory", "DIRNAME",
                           dirName);
                return PLDM_ERROR;
            }
        }
        else
        {
            fs::create_directory(dirName);
        }

        std::string fileName{dirName + "/cper-XXXXXX"};
        auto fd = mkstemp(fileName.data());
        if (fd < 0)
        {
            lg2::error("Failed to generate temp file:{ERRORNO}", "ERRORNO",
                       std::strerror(errno));
            return PLDM_ERROR;
        }
        close(fd);

        std::ofstream ofs;
        ofs.exceptions(std::ofstream::failbit | std::ofstream::badbit |
                       std::ofstream::eofbit);

        try
        {
            platformEventStatus = PLDM_EVENT_ACCEPTED_FOR_LOGGING;
            ofs.open(fileName);
            ofs.write(reinterpret_cast<const char*>(eventData), eventDataSize);
            if (formatType == PLDM_FORMAT_TYPE_CPER)
            {
                createCperDumpEntry("CPER", fileName);
            }
            else
            {
                createCperDumpEntry("CPERSection", fileName);
            }
            ofs.close();
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to save CPER to {FILENAME}, {ERROR}.",
                       "FILENAME", fileName, "ERROR", e);
            return PLDM_ERROR;
        }
    }
    else
    {
        lg2::info("unhandled event, event class={EVENTCLASS}", "EVENTCLASS",
                  eventClass);
        platformEventStatus = PLDM_EVENT_LOGGING_REJECTED;
    }
    return PLDM_SUCCESS;
}

requester::Coroutine
    EventManager::pollForPlatformEventTask(tid_t tid, uint16_t maxBufferSize)
{
    uint8_t rc = 0;
    uint8_t transferOperationFlag = PLDM_GET_FIRSTPART;
    uint32_t dataTransferHandle = 0;
    uint32_t eventIdToAcknowledge = 0;

    uint8_t completionCode = 0;
    uint8_t eventTid = 0;
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
            tid, transferOperationFlag, dataTransferHandle,
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
            if (transferFlag == PLDM_START || transferFlag == PLDM_MIDDLE)
            {
                transferOperationFlag = PLDM_GET_NEXTPART;
                dataTransferHandle = nextDataTransferHandle;
                eventIdToAcknowledge = 0xffff;
            }
            else
            {
                uint8_t platformEventStatus = PLDM_EVENT_NO_LOGGING;
                if (transferFlag == PLDM_START_AND_END)
                {
                    handlePlatformEvent(
                        eventTid, eventClass, eventMessage.data(),
                        eventMessage.size(), platformEventStatus);
                }
                else if (transferFlag == PLDM_END)
                {
                    if (eventDataIntegrityChecksum ==
                        crc32(eventMessage.data(), eventMessage.size()))
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

requester::Coroutine EventManager::pollForPlatformEventMessage(
    tid_t tid, uint8_t transferOperationFlag, uint32_t dataTransferHandle,
    uint16_t eventIdToAcknowledge, uint8_t& completionCode, uint8_t& eventTid,
    uint16_t& eventId, uint32_t& nextDataTransferHandle, uint8_t& transferFlag,
    uint8_t& eventClass, uint32_t& eventDataSize,
    std::vector<uint8_t>& eventData, uint32_t& eventDataIntegrityChecksum)
{
    Request request(sizeof(pldm_msg_hdr) +
                    PLDM_POLL_FOR_PLATFORM_EVENT_MESSAGE_REQ_BYTES);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    auto rc = encode_poll_for_platform_event_message_req(
        0, 0x01, transferOperationFlag, dataTransferHandle,
        eventIdToAcknowledge, requestMsg);
    if (rc)
    {
        lg2::error(
            "encode_poll_for_platform_event_message_req tid={TID} rc={RC}",
            "TID", tid, "RC", rc);
        co_return rc;
    }

    const pldm_msg* responseMsg = NULL;
    size_t responseLen = 0;
    rc = co_await terminusManager.SendRecvPldmMsg(tid, request, &responseMsg,
                                                  &responseLen);
    if (rc)
    {
        co_return rc;
    }

    rc = decode_poll_for_platform_event_message_resp(
        responseMsg, responseLen, &completionCode, &eventTid, &eventId,
        &nextDataTransferHandle, &transferFlag, &eventClass, &eventDataSize,
        eventData.data(), &eventDataIntegrityChecksum);
    if (rc)
    {
        lg2::error(
            "decode_poll_for_platform_event_message_resp tid={TID} rc={RC} responseLen={RLEN}",
            "TID", tid, "RC", rc, "RLEN", responseLen);
        co_return rc;
    }
    co_return completionCode;
}

void EventManager::createCperDumpEntry(const std::string& dataType,
                                       const std::string& dataPath)
{
    auto createDump = [](std::map<std::string, std::string>& addData) {
        static constexpr auto dumpObjPath =
            "/xyz/openbmc_project/dump/faultlog";
        static constexpr auto dumpInterface = "xyz.openbmc_project.Dump.Create";
        auto& bus = pldm::utils::DBusHandler::getBus();

        try
        {
            auto service = pldm::utils::DBusHandler().getService(dumpObjPath,
                                                                 dumpInterface);
            auto method = bus.new_method_call(service.c_str(), dumpObjPath,
                                              dumpInterface, "CreateDump");
            method.append(addData);
            bus.call_noreply(method);
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to create D-Bus Dump entry, {ERROR}.", "ERROR",
                       e);
        }
    };

    std::map<std::string, std::string> addData;
    addData["CPER_TYPE"] = dataType;
    addData["CPER_PATH"] = dataPath;
    createDump(addData);
    return;
}

void EventManager::createSensorThresholdLogEntry(const std::string& messageId,
                                                 const std::string& sensorName,
                                                 const double reading,
                                                 const double threshold)
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

    addData["REDFISH_MESSAGE_ARGS"] = sensorName + "," +
                                      std::to_string(reading) + "," +
                                      std::to_string(threshold);

    if (messageId == SensorThresholdWarningLowGoingHigh ||
        messageId == SensorThresholdWarningHighGoingLow)
    {
        level = Level::Informational;
    }
    else if (messageId == SensorThresholdWarningLowGoingLow ||
             messageId == SensorThresholdWarningHighGoingHigh ||
             messageId == SensorThresholdCriticalLowGoingHigh ||
             messageId == SensorThresholdCriticalHighGoingLow)
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
    uint8_t eventState = 0;
    uint8_t previousEventState = 0;
    uint8_t sensorDataSize = 0;
    uint32_t presentReading;
    decode_numeric_sensor_data(sensorData, sensorDataLength, &eventState,
                               &previousEventState, &sensorDataSize,
                               &presentReading);

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
            std::string messageId =
                getSensorThresholdMessageId(previousEventState, eventState);
            double threshold = std::numeric_limits<double>::quiet_NaN();
            double reading = std::numeric_limits<double>::quiet_NaN();
            switch (eventState)
            {
                case PLDM_SENSOR_LOWERFATAL:
                case PLDM_SENSOR_LOWERCRITICAL:
                    threshold = sensor->getThresholdLowerCritical();
                    break;
                case PLDM_SENSOR_UPPERFATAL:
                case PLDM_SENSOR_UPPERCRITICAL:
                    threshold = sensor->getThresholdUpperCritical();
                    break;
                case PLDM_SENSOR_LOWERWARNING:
                    threshold = sensor->getThresholdLowerWarning();
                    break;
                case PLDM_SENSOR_UPPERWARNING:
                    threshold = sensor->getThresholdUpperWarning();
                    break;
                default:
                    break;
            }
            switch (sensorDataSize)
            {
                case PLDM_SENSOR_DATA_SIZE_UINT8:
                    reading = static_cast<double>(
                        static_cast<uint8_t>(presentReading));
                    break;
                case PLDM_SENSOR_DATA_SIZE_SINT8:
                    reading = static_cast<double>(
                        static_cast<int8_t>(presentReading));
                    break;
                case PLDM_SENSOR_DATA_SIZE_UINT16:
                    reading = static_cast<double>(
                        static_cast<uint16_t>(presentReading));
                    break;
                case PLDM_SENSOR_DATA_SIZE_SINT16:
                    reading = static_cast<double>(
                        static_cast<int16_t>(presentReading));
                    break;
                case PLDM_SENSOR_DATA_SIZE_UINT32:
                    reading = static_cast<double>(
                        static_cast<uint32_t>(presentReading));
                    break;
                case PLDM_SENSOR_DATA_SIZE_SINT32:
                    reading = static_cast<double>(
                        static_cast<int32_t>(presentReading));
                    break;
                default:
                    break;
            }
            createSensorThresholdLogEntry(
                messageId, sensor->sensorName,
                sensor->unitModifier(sensor->conversionFormula(reading)),
                threshold);
        }
    }
}

std::string
    EventManager::getSensorThresholdMessageId(uint8_t previousEventState,
                                              uint8_t eventState)
{
    switch (previousEventState)
    {
        case PLDM_SENSOR_UPPERFATAL:
        case PLDM_SENSOR_UPPERCRITICAL:
            switch (eventState)
            {
                case PLDM_SENSOR_UPPERFATAL:
                case PLDM_SENSOR_UPPERCRITICAL:
                    return SensorThresholdCriticalHighGoingHigh;
                case PLDM_SENSOR_UPPERWARNING:
                    return SensorThresholdCriticalHighGoingLow;
                case PLDM_SENSOR_NORMAL:
                    return SensorThresholdWarningHighGoingLow;
                case PLDM_SENSOR_LOWERWARNING:
                    return SensorThresholdWarningLowGoingLow;
                case PLDM_SENSOR_LOWERCRITICAL:
                case PLDM_SENSOR_LOWERFATAL:
                    return SensorThresholdCriticalLowGoingLow;
                default:
                    break;
            }
            break;
        case PLDM_SENSOR_UPPERWARNING:
            switch (eventState)
            {
                case PLDM_SENSOR_UPPERFATAL:
                case PLDM_SENSOR_UPPERCRITICAL:
                    return SensorThresholdCriticalHighGoingHigh;
                case PLDM_SENSOR_UPPERWARNING:
                    return SensorThresholdWarningHighGoingHigh;
                case PLDM_SENSOR_NORMAL:
                    return SensorThresholdWarningHighGoingLow;
                case PLDM_SENSOR_LOWERWARNING:
                    return SensorThresholdWarningLowGoingLow;
                case PLDM_SENSOR_LOWERCRITICAL:
                case PLDM_SENSOR_LOWERFATAL:
                    return SensorThresholdCriticalLowGoingLow;
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
                    return SensorThresholdCriticalHighGoingHigh;
                case PLDM_SENSOR_UPPERWARNING:
                    return SensorThresholdWarningHighGoingHigh;
                case PLDM_SENSOR_NORMAL:
                    break;
                case PLDM_SENSOR_LOWERWARNING:
                    return SensorThresholdWarningLowGoingLow;
                case PLDM_SENSOR_LOWERCRITICAL:
                case PLDM_SENSOR_LOWERFATAL:
                    return SensorThresholdCriticalLowGoingLow;
                default:
                    break;
            }
            break;
        case PLDM_SENSOR_LOWERWARNING:
            switch (eventState)
            {
                case PLDM_SENSOR_UPPERFATAL:
                case PLDM_SENSOR_UPPERCRITICAL:
                    return SensorThresholdCriticalHighGoingHigh;
                case PLDM_SENSOR_UPPERWARNING:
                    return SensorThresholdWarningHighGoingHigh;
                case PLDM_SENSOR_NORMAL:
                    return SensorThresholdWarningLowGoingHigh;
                case PLDM_SENSOR_LOWERWARNING:
                    return SensorThresholdWarningLowGoingLow;
                case PLDM_SENSOR_LOWERCRITICAL:
                case PLDM_SENSOR_LOWERFATAL:
                    return SensorThresholdCriticalLowGoingLow;
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
                    return SensorThresholdCriticalHighGoingHigh;
                case PLDM_SENSOR_UPPERWARNING:
                    return SensorThresholdWarningHighGoingHigh;
                case PLDM_SENSOR_NORMAL:
                    return SensorThresholdWarningLowGoingHigh;
                case PLDM_SENSOR_LOWERWARNING:
                    return SensorThresholdCriticalLowGoingHigh;
                case PLDM_SENSOR_LOWERCRITICAL:
                case PLDM_SENSOR_LOWERFATAL:
                    return SensorThresholdCriticalLowGoingLow;
                default:
                    break;
            }
            break;
    }
    return std::string{};
}

} // namespace platform_mc
} // namespace pldm
