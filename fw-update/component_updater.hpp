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

#include "common/types.hpp"
#include "fw_update_utility.hpp"
#include "requester/handler.hpp"
#include "requester/request.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/timer.hpp>
#include <sdeventplus/event.hpp>
#include <sdeventplus/source/event.hpp>

#include <fstream>

namespace pldm
{

namespace fw_update
{

class UpdateManager;
class DeviceUpdater;

/** @enum Enumeration to represent the PLDM ComponentUpdater sequence in the
 * firmware update flow
 */
enum class ComponentUpdaterSequence
{
    UpdateComponent,
    RequestFirmwareData,
    TransferComplete,
    VerifyComplete,
    ApplyComplete,
    CancelUpdateComponent,
    Invalid,
    RetryRequest,
    Valid
};

/** @enum Enumeration to represent the PLDM ComponentUpdateStatus
 */
enum class ComponentUpdateStatus
{
    UpdateFailed,
    UpdateComplete,
    UpdateSkipped
};

/** @class ComponentUpdaterState
 *
 *  To manage the sequence of the PLDM ComponentUpdater as part of the firmware
 * update flow.
 */
struct ComponentUpdaterState
{
    ComponentUpdaterState(bool fwDebug = false) :
        prev(ComponentUpdaterSequence::UpdateComponent),
        current(ComponentUpdaterSequence::UpdateComponent), fwDebug(fwDebug)
    {}

    /** @brief To get next action of the PLDM sequence as per the flow
     *
     *  @param[in] command - the current sequence in the PLDM ComponentUpdater
     * flow
     *
     */
    ComponentUpdaterSequence nextState(ComponentUpdaterSequence command)
    {
        switch (command)
        {
            case ComponentUpdaterSequence::UpdateComponent:
            {
                prev = current;
                current = ComponentUpdaterSequence::RequestFirmwareData;
                break;
            }
            case ComponentUpdaterSequence::RequestFirmwareData:
            {
                prev = current;
                current = ComponentUpdaterSequence::TransferComplete;
                break;
            }
            case ComponentUpdaterSequence::TransferComplete:
            {
                prev = current;
                current = ComponentUpdaterSequence::VerifyComplete;
                break;
            }
            case ComponentUpdaterSequence::VerifyComplete:
            {
                prev = current;
                current = ComponentUpdaterSequence::ApplyComplete;
                break;
            }
            case ComponentUpdaterSequence::ApplyComplete:
            {
                // Next state can be either update or activate firmware. Which
                // will be decided by device updater
                prev = ComponentUpdaterSequence::ApplyComplete;
                break;
            }
            default:
            {
                current = ComponentUpdaterSequence::Invalid;
                break;
            }
        };

        if (fwDebug)
        {
            lg2::info("ComponentUpdater:prevSeq = {PREVSEQ}, command = "
                      "{COMMAND}, currentSeq = {CURRENTSEQ}",
                      "PREVSEQ", static_cast<size_t>(current), "COMMAND",
                      static_cast<size_t>(command), "CURRENTSEQ",
                      static_cast<size_t>(current));
        }
        return current;
    }

    /** @brief To validate if the command handled by the DeviceUpdater is as per
     *         the expected PLDM ComponentUpdater flow.
     *
     *  @param[in] command - Validate the current sequence of the
     * ComponentUpdater against the command received
     *
     *  @return ComponentUpdaterSequence - retry, valid or invalid states
     */
    ComponentUpdaterSequence expectedState(ComponentUpdaterSequence command)
    {
        if ((current == ComponentUpdaterSequence::RequestFirmwareData) &&
            (command == ComponentUpdaterSequence::TransferComplete))
        {
            current = ComponentUpdaterSequence::TransferComplete;
            return ComponentUpdaterSequence::Valid;
        }
        else
        {
            if (command == prev)
            {
                lg2::error("ComponentUpdater Retry Request: inCmd = {COMMAND}, "
                           "currentSeq = {CURRENTSEQ}",
                           "COMMAND", static_cast<size_t>(command),
                           "CURRENTSEQ", static_cast<size_t>(current));
                return ComponentUpdaterSequence::RetryRequest;
            }
            if (command != current)
            {
                lg2::error(
                    "ComponentUpdater Unexpected command: inCmd = {COMMAND}, "
                    "currentSeq = {CURRENTSEQ}",
                    "COMMAND", static_cast<size_t>(command), "CURRENTSEQ",
                    static_cast<size_t>(current));
                return ComponentUpdaterSequence::Invalid;
            }
            return ComponentUpdaterSequence::Valid;
        }
    }

    /** @brief To set the state of the PLDM ComponentUpdater, it will be used
     * for handling exceptions in the firmware update flow and for tests
     *
     *  @param[in] state - The state to be set
     *
     *  @return ComponentUpdaterSequence - the current state of the PLDM
     * ComponentUpdater
     */
    ComponentUpdaterSequence set(ComponentUpdaterSequence state)
    {
        current = state;
        return current;
    }

    ComponentUpdaterSequence prev;
    ComponentUpdaterSequence current;
    bool fwDebug;
};

/** @class ComponentUpdater
 *
 *  ComponentUpdater orchestrates the firmware update of the firmware device
 * and updates the UpdateManager about the status once it is complete.
 */
class ComponentUpdater
{
  public:
    ComponentUpdater() = delete;
    ComponentUpdater(const ComponentUpdater&) = delete;
    ComponentUpdater(ComponentUpdater&&) = default;
    ComponentUpdater& operator=(const ComponentUpdater&) = delete;
    ComponentUpdater& operator=(ComponentUpdater&&) = default;
    ~ComponentUpdater() = default;

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
     *  @param[in] deviceUpdater - To update the status of device
     *  @param[in] componentIndex - component index
     */
    explicit ComponentUpdater(mctp_eid_t eid, std::ifstream& package,
                              const FirmwareDeviceIDRecord& fwDeviceIDRecord,
                              const ComponentImageInfos& compImageInfos,
                              const ComponentInfo& compInfo,
                              const ComponentIdNameMap& compIdNameInfo,
                              uint32_t maxTransferSize,
                              UpdateManager* updateManager,
                              DeviceUpdater* deviceUpdater,
                              size_t componentIndex, bool fwDebug) :
        fwDeviceIDRecord(fwDeviceIDRecord),
        componentUpdaterState(fwDebug), eid(eid), package(package),
        compImageInfos(compImageInfos), compInfo(compInfo),
        compIdNameInfo(compIdNameInfo), maxTransferSize(maxTransferSize),
        updateManager(updateManager), deviceUpdater(deviceUpdater),
        componentIndex(componentIndex), reqFwDataTimer(nullptr),
        completeCommandsTimeoutTimer(nullptr)
    {}

    /**
     * @brief start component updater
     *
     * @return requester::Coroutine
     */
    requester::Coroutine startComponentUpdater();

    /** @brief Send UpdateComponent command request
     *
     *  @param[in] compOffset - component offset in compImageInfos
     *  @return requester::Coroutine
     */
    requester::Coroutine sendUpdateComponentRequest(size_t offset);

    /** @brief Handler for UpdateComponent command response
     *
     *  The response of the UpdateComponent is processed and will wait for
     *  FD to request the firmware data.
     *
     *  @param[in] eid - Remote MCTP endpoint
     *  @param[in] response - PLDM response message
     *  @param[in] respMsgLen - Response message length
     */
    int processUpdateComponentResponse(mctp_eid_t eid, const pldm_msg* response,
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

    /**
     * @brief timeout handler for requestFirmwareData timeout (UA_T2)
     *
     */
    void createRequestFwDataTimer();

    /**
     * @brief send cancel update component request
     *
     * @return requester::Coroutine
     */
    requester::Coroutine sendcancelUpdateComponentRequest();
    /**
     * @brief cancel update component response handler
     *
     * @param[in] eid - mctp endpoint id
     * @param[in] response - cancel update response
     * @param[in] respMsgLen - response length
     */
    int processCancelUpdateComponentResponse(mctp_eid_t eid,
                                             const pldm_msg* response,
                                             size_t respMsgLen);

    /** @brief FirmwareDeviceIDRecord in the fw update package that matches this
     *         firmware device
     */
    const FirmwareDeviceIDRecord& fwDeviceIDRecord;

    /** @brief PLDM ComponentUpdater state machine
     */
    struct ComponentUpdaterState componentUpdaterState;

    /**
     * @brief get status command to get FD status
     *
     * @param[in] getStatusCallback - to handle post transfer, verify and apply
     * complete failures
     * @return requester::Coroutine
     */
    requester::Coroutine
        GetStatus(std::function<void(uint8_t)> getStatusCallback);

    /**
     * @brief process get status response
     *
     * @param[in] eid
     * @param[in] response
     * @param[in] respMsgLen
     * @param[out] currentFDState
     * @param[out] progressPercent
     * @return requester::Coroutine
     */
    int processGetStatusResponse(mctp_eid_t eid, const pldm_msg* response,
                                 size_t respMsgLen, uint8_t& currentFDState,
                                 uint8_t& progressPercent);

    /**
     * @brief run get status coroutine
     *
     * @param getStatusCallback
     */
    void handleComponentUpdateFailure(std::function<void()> failureCallback);

  private:
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

    /**
     *  @brief To update the status of fw update of the FD */
    UpdateManager* updateManager;

    /**
     *  @brief To update the status of fw update of the component */
    DeviceUpdater* deviceUpdater;

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
     * @brief Timeout in seconds for the UA to cancel the component update if no
     * command is received from the FD during component image transfer stage
     *
     */
    auto static constexpr updateTimeoutSeconds = 60;
    /**
     * @brief timer to handle RequestFirmwareData timeout(UA_T2)
     *
     */
    std::unique_ptr<sdbusplus::Timer> reqFwDataTimer;

    /* cancel component update coroutine handler */
    std::coroutine_handle<> cancelCompUpdateHandle = nullptr;

    /* cancel component update coroutine handler */
    std::coroutine_handle<> updateCompletionCoHandle = nullptr;

    /**
     * @brief update complete handler
     *
     * @param[in] status
     */
    void updateComponentComplete(ComponentUpdateStatus status);

    /* Complete commands timout(UA_T6) in seconds. Default value is 600 as per
     * the spec*/
    auto static constexpr completeCommandsTimeoutSeconds = 600;
    /**
     * @brief Complete Commands Timeout Timer handler
     *
     */
    void createCompleteCommandsTimeoutTimer();

    /** @brief Handler for the Failed Status of the ApplyComplete request.
     *
     *  @param[in] applyResult - Apply Result
     *  @return None
     */
    void applyCompleteFailedStatusHandler(uint8_t applyResult);

    /** @brief Handler for the Succeeded Status of the ApplyComplete request.
     *
     *  @param[in] compVersion - Component Version
     *  @param[in] compActivationModification - Component Activation
     * Modification
     *  @return None
     */
    void applyCompleteSucceededStatusHandler(
        const std::string& compVersion,
        bitfield16_t compActivationModification);

    /**
     * @brief UA_T6 complete command timout timer
     *
     */
    std::unique_ptr<sdbusplus::Timer> completeCommandsTimeoutTimer;
};

} // namespace fw_update

} // namespace pldm