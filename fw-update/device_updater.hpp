#pragma once

#include "common/types.hpp"
#include "requester/handler.hpp"
#include "requester/request.hpp"

#include <sdbusplus/timer.hpp>
#include <sdeventplus/event.hpp>
#include <sdeventplus/source/event.hpp>
#include <phosphor-logging/lg2.hpp>

#include <fstream>

namespace pldm
{

namespace fw_update
{

class UpdateManager;

/** @enum Enumeration to represent the PLDM UA sequence in the firmware update
 *        flow
 */
enum class UASequence
{
    RequestUpdate,
    PassComponentTable,
    UpdateComponent,
    RequestFirmwareData,
    TransferComplete,
    VerifyComplete,
    ApplyComplete,
    ActivateFirmware,
    CancelUpdateComponent,
    Invalid,
};

/** @class UAState
 *
 *  To manage the sequence of the PLDM UA as part of the firmware update flow.
 */
struct UAState
{
    UAState(bool fwDebug = false) :
        current(UASequence::RequestUpdate), fwDebug(fwDebug)
    {}

    /** @brief To get next action of the PLDM sequence as per the flow
     *
     *  @param[in] command - the current sequence in the PLDM UA flow
     *  @param[in] compIndex - current component index, this is applicable only
     *                         when the current action is PassComponentTable and
     *                         ApplyComplete
     *  @param[in] numComps - The number of components applicable for the FD
     *
     */
    UASequence nextState(UASequence command, size_t compIndex, size_t numComps)
    {
        auto prevSeq = current;
        switch (command)
        {
            case UASequence::RequestUpdate:
            {
                current = UASequence::PassComponentTable;
                break;
            }
            case UASequence::PassComponentTable:
            {
                if (compIndex < numComps)
                {
                    current = UASequence::PassComponentTable;
                }
                else
                {
                    current = UASequence::UpdateComponent;
                }
                break;
            }
            case UASequence::UpdateComponent:
            {
                current = UASequence::RequestFirmwareData;
                break;
            }
            case UASequence::RequestFirmwareData:
            {
                current = UASequence::TransferComplete;
                break;
            }
            case UASequence::TransferComplete:
            {
                current = UASequence::VerifyComplete;
                break;
            }
            case UASequence::VerifyComplete:
            {
                current = UASequence::ApplyComplete;
                break;
            }
            case UASequence::ApplyComplete:
            {
                if (compIndex < numComps)
                {
                    current = UASequence::UpdateComponent;
                }
                else
                {
                    current = UASequence::ActivateFirmware;
                }
                break;
            }
            case UASequence::ActivateFirmware:
            {
                current = UASequence::Invalid;
                break;
            }
            default:
            {
                current = UASequence::Invalid;
                break;
            }
        };

        if (fwDebug)
        {
            lg2::info(
                "prevSeq = {PREVSEQ}, command = {COMMAND}, currentSeq = {CURRENTSEQ}, compIndex = {COMPINDEX}, numComps = {NUMCOMPS}",
                "PREVSEQ", static_cast<size_t>(prevSeq), "COMMAND",
                static_cast<size_t>(command), "CURRENTSEQ",
                static_cast<size_t>(current), "COMPINDEX", compIndex,
                "NUMCOMPS", numComps);
        }

        return current;
    }

    /** @brief To validate if the command handled by the DeviceUpdater is as per
     *         the expected PLDM UA flow.
     *
     *  @param[in] command - Validate the current sequence of the UA against the
     *                     command received
     *
     *  @return bool - true if the command received is as expected, false if
     *          the command received is unexpected and return
     *          COMMAND_NOT_EXPECTED by the command handler
     */
    bool expectedState(UASequence command)
    {
        if ((current == UASequence::RequestFirmwareData) &&
            (command == UASequence::TransferComplete))
        {
            current = UASequence::TransferComplete;
            return true;
        }
        else
        {
            if (command != current)
            {
                lg2::error(
                    "Unexpected command: inCmd = {COMMAND}, currentSeq = {CURRENTSEQ}",
                    "COMMAND", static_cast<size_t>(command), "CURRENTSEQ",
                    static_cast<size_t>(current));
            }
            return (command == current);
        }
    }

    /** @brief To set the state of the PLDM UA, it will be used for handling
     *         exceptions in the firmware update flow and for tests
     *
     *  @param[in] state - The state to be set
     *
     *  @return UASequence - the current state of the PLDM UA
     */
    UASequence set(UASequence state)
    {
        current = state;
        return current;
    }

    UASequence current;
    bool fwDebug;
};

/** @class DeviceUpdater
 *
 *  DeviceUpdater orchestrates the firmware update of the firmware device
 * and updates the UpdateManager about the status once it is complete.
 */
class DeviceUpdater
{
  public:
    DeviceUpdater() = delete;
    DeviceUpdater(const DeviceUpdater&) = delete;
    DeviceUpdater(DeviceUpdater&&) = default;
    DeviceUpdater& operator=(const DeviceUpdater&) = delete;
    DeviceUpdater& operator=(DeviceUpdater&&) = default;
    ~DeviceUpdater() = default;

    /** @brief Constructor
     *
     *  @param[in] eid - Endpoint ID of the firmware device
     *  @param[in] package - File stream for firmware update package
     *  @param[in] fwDeviceIDRecord - FirmwareDeviceIDRecord in the fw update
     *                                package that matches this firmware device
     *  @param[in] compImageInfos - Component image information for all the
     *                              components in the fw update package
     *  @param[in] compInfo - Component info for the components in this FD
     *                        derived from GetFirmwareParameters response
     *  @param[in] compIdNameInfo - Component name info for components
     *                              applicable for the FD
     *  @param[in] maxTransferSize - Maximum size in bytes of the variable
     *                               payload allowed to be requested by the FD
     *  @param[in] updateManager - To update the status of fw update of the
     *                             device
     */
    explicit DeviceUpdater(mctp_eid_t eid, std::ifstream& package,
                           const FirmwareDeviceIDRecord& fwDeviceIDRecord,
                           const ComponentImageInfos& compImageInfos,
                           const ComponentInfo& compInfo,
                           const ComponentIdNameMap& compIdNameInfo,
                           uint32_t maxTransferSize,
                           UpdateManager* updateManager, bool fwDebug) :
        fwDeviceIDRecord(fwDeviceIDRecord),
        uaState(fwDebug), eid(eid), package(package),
        compImageInfos(compImageInfos), compInfo(compInfo),
        compIdNameInfo(compIdNameInfo), maxTransferSize(maxTransferSize),
        updateManager(updateManager), reqFwDataTimer(nullptr)
    {}

    /** @brief Start the firmware update flow for the FD
     *
     *  To start the update flow RequestUpdate command is sent to the FD.
     *
     */
    void startFwUpdateFlow();

    /** @brief Handler for RequestUpdate command response
     *
     *  The response of the RequestUpdate is processed and if the response
     *  is success, send PassComponentTable request to FD.
     *
     *  @param[in] eid - Remote MCTP endpoint
     *  @param[in] response - PLDM response message
     *  @param[in] respMsgLen - Response message length
     */
    void requestUpdate(mctp_eid_t eid, const pldm_msg* response,
                       size_t respMsgLen);

    /** @brief Handler for PassComponentTable command response
     *
     *  The response of the PassComponentTable is processed. If the response
     *  indicates component can be updated, continue with either a) or b).
     *
     *  a. Send PassComponentTable request for the next component if
     *     applicable
     *  b. UpdateComponent command to request updating a specific
     *     firmware component
     *
     *  If the response indicates component may be updateable, continue
     *  based on the policy in DeviceUpdateOptionFlags.
     *
     *  @param[in] eid - Remote MCTP endpoint
     *  @param[in] response - PLDM response message
     *  @param[in] respMsgLen - Response message length
     */
    void passCompTable(mctp_eid_t eid, const pldm_msg* response,
                       size_t respMsgLen);

    /** @brief Handler for UpdateComponent command response
     *
     *  The response of the UpdateComponent is processed and will wait for
     *  FD to request the firmware data.
     *
     *  @param[in] eid - Remote MCTP endpoint
     *  @param[in] response - PLDM response message
     *  @param[in] respMsgLen - Response message length
     */
    void updateComponent(mctp_eid_t eid, const pldm_msg* response,
                         size_t respMsgLen);

    /** @brief Handler for RequestFirmwareData request
     *
     *  @param[in] request - Request message
     *  @param[in] payload_length - Request message payload length
     *  @return Response - PLDM Response message
     */
    Response requestFwData(const pldm_msg* request, size_t payloadLength);

    /** @brief Handler for TransferComplete request
     *
     *  @param[in] request - Request message
     *  @param[in] payload_length - Request message payload length
     *  @return Response - PLDM Response message
     */
    Response transferComplete(const pldm_msg* request, size_t payloadLength);

    /** @brief Handler for VerifyComplete request
     *
     *  @param[in] request - Request message
     *  @param[in] payload_length - Request message payload length
     *  @return Response - PLDM Response message
     */
    Response verifyComplete(const pldm_msg* request, size_t payloadLength);

    /** @brief Handler for ApplyComplete request
     *
     *  @param[in] request - Request message
     *  @param[in] payload_length - Request message payload length
     *  @return Response - PLDM Response message
     */
    Response applyComplete(const pldm_msg* request, size_t payloadLength);

    /** @brief Handler for ActivateFirmware command response
     *
     *  The response of the ActivateFirmware is processed and will update the
     *  UpdateManager with the completion of the firmware update.
     *
     *  @param[in] eid - Remote MCTP endpoint
     *  @param[in] response - PLDM response message
     *  @param[in] respMsgLen - Response message length
     */
    void activateFirmware(mctp_eid_t eid, const pldm_msg* response,
                          size_t respMsgLen);

    /** @brief FirmwareDeviceIDRecord in the fw update package that matches this
     *         firmware device
     */
    const FirmwareDeviceIDRecord& fwDeviceIDRecord;

    /** @brief PLDM UA state machine
     */
    struct UAState uaState;

  private:
    /** @brief Send PassComponentTable command request
     *
     *  @param[in] compOffset - component offset in compImageInfos
     */
    void sendPassCompTableRequest(size_t offset);

    /** @brief Send UpdateComponent command request
     *
     *  @param[in] compOffset - component offset in compImageInfos
     */
    void sendUpdateComponentRequest(size_t offset);

    /** @brief Send ActivateFirmware command request */
    void sendActivateFirmwareRequest();

    /** @brief Endpoint ID of the firmware device */
    mctp_eid_t eid;

    /** @brief File stream for firmware update package */
    std::ifstream& package;

    /** @brief Component image information for all the components in the fw
     *         update package
     */
    const ComponentImageInfos& compImageInfos;

    /** @brief Component info for the components in this FD derived from
     *         GetFirmwareParameters response
     */
    const ComponentInfo& compInfo;

    /** @brief Component name info for components applicable for the FD.
     */
    const ComponentIdNameMap& compIdNameInfo;

    /** @brief Maximum size in bytes of the variable payload to be requested by
     *         the FD via RequestFirmwareData command
     */
    uint32_t maxTransferSize;

    /** @brief To update the status of fw update of the FD */
    UpdateManager* updateManager;

    /** @brief Component index is used to track the current component being
     *         updated if multiple components are applicable for the FD.
     *         It is also used to keep track of the next component in
     *         PassComponentTable
     */
    size_t componentIndex = 0;

    size_t numComponents = 0;

    /** @brief To send a PLDM request after the current command handling */
    std::unique_ptr<sdeventplus::source::Defer> pldmRequest;

    /**
     * @brief Print debug logs for firmware update when firmware debug option is
     enabled
     *        This variant of printBuffer takes integer vector as an input
     *
     * @param[in] isTx - True if the buffer is an outgoing PLDM message, false
     if the buffer is an incoming PLDM message
     * @param[in] buffer - integer vector buffer to log
     * @param[in] message - Message string for logging
     */
    void printBuffer(bool isTx, const std::vector<uint8_t>& buffer,
                     const std::string& message);

    /**
     * @brief Print debug logs for firmware update when firmware debug option is
     * enabled. This variant of printBuffer takes pldm_msg* buffer as an input.
     *
     * @param[in] isTx - True if the buffer is an outgoing PLDM message, false
     * if the buffer is an incoming PLDM message
     * @param[in] buffer - pldm message buffer to log
     * @param[in] bufferLen - pldm message buffer length
     * @param[in] message - Message string for logging
     */
    void printBuffer(bool isTx, const pldm_msg* buffer, size_t bufferLen,
                     const std::string& message);

    /**
     * @brief Timeout in seconds for the UA to cancel the component update if no
     * command is received from the FD during component image transfer stage
     *
     */
    auto static constexpr updateTimeoutSeconds = 60;
    /**
     * @brief map to hold component update status. true - success, false -
     * cancelled
     *
     */
    std::map<size_t, bool> componentUpdateStatus;
    /**
     * @brief timer to handle RequestFirmwareData timeout(UA_T2)
     *
     */
    std::unique_ptr<phosphor::Timer> reqFwDataTimer;

    /**
     * @brief timeout handler for requestFirmwareData timeout (UA_T2)
     *
     */
    void createRequestFwDataTimer();

    /**
     * @brief send cancel update component request
     *
     */
    void sendcancelUpdateComponentRequest();
    /**
     * @brief cancel update component response handler
     *
     * @param[in] eid - mctp endpoint id
     * @param[in] response - cancel update response
     * @param[in] respMsgLen - response length
     */
    void cancelUpdateComponent(mctp_eid_t eid, const pldm_msg* response,
                               size_t respMsgLen);

    /** @brief Send COMMAND_NOT_EXPECTED response sent by UA when it receives
     *         a command from the FD out of sequence from when it is expected.
     *
     *  @param[in] request - PLDM request message
     *  @param[in] requestLen - PLDM request message length
     */
    Response sendCommandNotExpectedResponse(const pldm_msg* request,
                                            size_t requestLen);

    /**
     * @brief List of components successfully updated
     *
     */
    std::vector<ComponentName> successCompNames;
};

} // namespace fw_update

} // namespace pldm