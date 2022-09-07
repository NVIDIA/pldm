#include "event_manager.hpp"

#include "libpldm/utils.h"

#include "terminus_manager.hpp"

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
            std::cerr << "Failed to decode sensor event data, rc= "
                      << static_cast<unsigned>(rc) << "\n";
            return rc;
        }
        switch (sensorEventClassType)
        {
            case PLDM_NUMERIC_SENSOR_STATE:
            case PLDM_STATE_SENSOR_STATE:
            case PLDM_SENSOR_OP_STATE:
            default:
                std::cout << "unhandled sensor event, class type="
                          << unsigned(sensorEventClassType) << "\n";
                platformEventStatus = PLDM_EVENT_LOGGING_REJECTED;
                break;
        }
    }
    else if (eventClass == PLDM_MESSAGE_POLL_EVENT)
    {
        deferredPldmMessagePollEvent =
            std::make_unique<sdeventplus::source::Defer>(
                event,
                std::bind(
                    std::mem_fn(
                        &EventManager::processDeferredPldmMessagePollEvent),
                    this, tid));
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
            std::cerr << "Failed to decode CPER event data, rc= "
                      << static_cast<unsigned>(rc) << "\n";
            return rc;
        }

        // save event data to file
        std::string dirName{"/var/cper"};
        auto dirStatus = fs::status(dirName);
        if (fs::exists(dirStatus))
        {
            if (!fs::is_directory(dirStatus))
            {
                std::cerr << "Failed to create " << dirName << "directory\n";
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
            std::cerr << "failed to generate temp file:" << std::strerror(errno)
                      << "\n";
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
            auto err = errno;
            std::cerr << "failed to save CPER to " << fileName << ":"
                      << std::strerror(err) << "\n";
            return PLDM_ERROR;
        }
    }
    else
    {
        std::cout << "unhandled event class:"
                  << static_cast<unsigned>(eventClass) << "\n";
        platformEventStatus = PLDM_EVENT_LOGGING_REJECTED;
    }
    return PLDM_SUCCESS;
}

void EventManager::processDeferredPldmMessagePollEvent(tid_t tid)
{
    deferredPldmMessagePollEvent.reset();
    pollForPlatformEvent(tid);
}

void EventManager::pollForPlatformEvent(tid_t tid)
{
    auto it = termini.find(tid);
    if (it != termini.end())
    {
        auto& terminus = it->second;
        if (terminus->pollForPlatformEventTaskHandle)
        {
            if (terminus->pollForPlatformEventTaskHandle.done())
            {
                terminus->pollForPlatformEventTaskHandle.destroy();
                auto co =
                    pollForPlatformEventTask(tid, terminus->maxBufferSize);
                terminus->pollForPlatformEventTaskHandle = co.handle;
                if (terminus->pollForPlatformEventTaskHandle.done())
                {
                    terminus->pollForPlatformEventTaskHandle = nullptr;
                }
            }
        }
        else
        {
            auto co = pollForPlatformEventTask(tid, terminus->maxBufferSize);
            terminus->pollForPlatformEventTaskHandle = co.handle;
            if (terminus->pollForPlatformEventTaskHandle.done())
            {
                terminus->pollForPlatformEventTaskHandle = nullptr;
            }
        }
    }
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
            co_return rc;
        }

        if (completionCode != PLDM_SUCCESS)
        {
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
                        std::cerr
                            << "pollForPlatformEventMessageTask: event message(tid:"
                            << static_cast<unsigned>(tid)
                            << ",eventId:" << static_cast<unsigned>(eventId)
                            << ") checksum error\n";
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
        co_return rc;
    }
    co_return completionCode;
}

void EventManager::createCperDumpEntry(const std::string& dataType,
                                       const std::string& dataPath)
{
    using namespace sdbusplus::xyz::openbmc_project::Logging::server;

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
            std::cerr << "Failed to create D-Bus Dump entry, ERROR=" << e.what()
                      << "\n";
        }
    };

    std::map<std::string, std::string> addData;
    addData["CPER_TYPE"] = dataType;
    addData["CPER_PATH"] = dataPath;
    createDump(addData);
    return;
}
} // namespace platform_mc
} // namespace pldm
