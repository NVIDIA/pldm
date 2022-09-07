#pragma once

#include "libpldm/platform.h"
#include "libpldm/requester/pldm.h"

#include "common/types.hpp"
#include "numeric_sensor.hpp"
#include "pldmd/dbus_impl_requester.hpp"
#include "requester/handler.hpp"
#include "terminus.hpp"

namespace pldm
{
namespace platform_mc
{

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
        sdeventplus::Event& event, TerminusManager& terminusManager,
        std::map<mctp_eid_t, std::shared_ptr<Terminus>>& termini) :
        event(event),
        terminusManager(terminusManager), termini(termini){};

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

  private:
    /** @brief Start a coroutine for polling all events from terminus
     *
     *  @param[in] tid - the tid of terminus to be polled
     */
    void pollForPlatformEvent(tid_t tid);

    /** @brief A Coroutine to poll all events from terminus
     *
     *  @param[in] dstTid - the destination TID
     */
    requester::Coroutine pollForPlatformEventTask(tid_t tid,
                                                  uint16_t maxBufferSize);

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

    void createCperDumpEntry(const std::string& dataType,
                             const std::string& dataPath);

    void processDeferredPldmMessagePollEvent(uint8_t tid);

    sdeventplus::Event& event;

    /** @brief Reference of terminusManager */
    TerminusManager& terminusManager;

    /** @brief List of discovered termini */
    std::map<tid_t, std::shared_ptr<Terminus>>& termini;

    std::unique_ptr<sdeventplus::source::Defer> deferredPldmMessagePollEvent;
};
} // namespace platform_mc
} // namespace pldm
