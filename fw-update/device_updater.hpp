/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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
#include "component_updater.hpp"
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

/** @enum Enumeration to represent the PLDM DeviceUpdater sequence in the
 * firmware update flow
 */
enum class DeviceUpdaterSequence
{
    RequestUpdate,
    PassComponentTable,
    ActivateFirmware,
    RetryRequest,
    Invalid,
    Valid,
    CancelUpdate
};

/** @class DeviceUpdaterState
 *
 *  To manage the sequence of the PLDM DeviceUpdater as part of the firmware
 * update flow.
 */
struct DeviceUpdaterState
{
    DeviceUpdaterState(bool fwDebug = false) :
        current(DeviceUpdaterSequence::RequestUpdate),
        prev(DeviceUpdaterSequence::Valid), fwDebug(fwDebug)
    {}

    /** @brief To get next action of the PLDM sequence as per the flow
     *
     *  @param[in] command - the current sequence in the PLDM DeviceUpdater flow
     *  @param[in] compIndex - current component index, this is applicable only
     *                         when the current action is PassComponentTable and
     *                         ApplyComplete
     *  @param[in] numComps - The number of components applicable for the FD
     *
     */
    DeviceUpdaterSequence nextState(DeviceUpdaterSequence command,
                                    size_t compIndex, size_t numComps)
    {
        if (prev == command)
        {
            lg2::error(
                "DeviceUpdater: Retry Request for command: inCmd = {COMMAND}, "
                "currentSeq ={CURRENTSEQ}, prevSeq = {PREVSEQ}",
                "COMMAND", static_cast<size_t>(command), "CURRENTSEQ",
                static_cast<size_t>(current), "PREVSEQ",
                static_cast<size_t>(prev));
            return DeviceUpdaterSequence::RetryRequest;
        }
        switch (command)
        {
            case DeviceUpdaterSequence::RequestUpdate:
            {
                prev = current;
                current = DeviceUpdaterSequence::PassComponentTable;
                break;
            }
            case DeviceUpdaterSequence::PassComponentTable:
            {
                if (compIndex < numComps)
                {
                    current = DeviceUpdaterSequence::PassComponentTable;
                }
                else
                {
                    prev = current;
                    current = DeviceUpdaterSequence::ActivateFirmware;
                }
                break;
            }
            case DeviceUpdaterSequence::ActivateFirmware:
            {
                prev = current;
                current = DeviceUpdaterSequence::Invalid;
                break;
            }
            default:
            {
                current = DeviceUpdaterSequence::Invalid;
                break;
            }
        };

        if (fwDebug)
        {
            lg2::info("DeviceUpdater: prevSeq = {PREVSEQ}, command = {COMMAND},"
                      " currentSeq = {CURRENTSEQ}, compIndex = {COMPINDEX},"
                      " numComps = {NUMCOMPS}",
                      "PREVSEQ", static_cast<size_t>(prev), "COMMAND",
                      static_cast<size_t>(command), "CURRENTSEQ",
                      static_cast<size_t>(current), "COMPINDEX", compIndex,
                      "NUMCOMPS", numComps);
        }
        return current;
    }

    /** @brief To validate if the command handled by the DeviceUpdater is as per
     *         the expected PLDM DeviceUpdater flow.
     *
     *  @param[in] command - Validate the current sequence of the DeviceUpdater
     * against the command received
     *
     *  @return bool - true if the command received is as expected, false if
     *          the command received is unexpected and return
     *          COMMAND_NOT_EXPECTED by the command handler
     */
    DeviceUpdaterSequence expectedState(DeviceUpdaterSequence command)
    {
        if (prev == command)
        {
            lg2::error(
                "DeviceUpdater Retry Request for command: inCmd = {COMMAND},"
                " currentSeq={CURRENTSEQ}, prevSeq={PREVSEQ}",
                "COMMAND", static_cast<size_t>(command), "CURRENTSEQ",
                static_cast<size_t>(current), "PREVSEQ",
                static_cast<size_t>(prev));
            return DeviceUpdaterSequence::RetryRequest;
        }
        if (command != current)
        {
            lg2::error("DeviceUpdater Unexpected command: inCmd = {COMMAND}, "
                       "currentSeq = {CURRENTSEQ}",
                       "COMMAND", static_cast<size_t>(command), "CURRENTSEQ",
                       static_cast<size_t>(current));
            return DeviceUpdaterSequence::Invalid;
        }
        return DeviceUpdaterSequence::Valid;
    }

    /** @brief To set the state of the PLDM DeviceUpdater, it will be used for
     * handling exceptions in the firmware update flow and for tests
     *
     *  @param[in] state - The state to be set
     *
     *  @return DeviceUpdaterSequence - the current state of the PLDM
     * DeviceUpdater
     */
    DeviceUpdaterSequence set(DeviceUpdaterSequence state)
    {
        prev = current;
        current = state;
        return current;
    }

    DeviceUpdaterSequence current;
    DeviceUpdaterSequence prev;
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
    ~DeviceUpdater()
    {
        // clear device updater coroutine
        if (deviceUpdaterHandle && deviceUpdaterHandle.done())
        {
            deviceUpdaterHandle.destroy();
        }
        componentUpdaterMap.clear();
    }

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
        deviceUpdaterState(fwDebug), eid(eid), package(package),
        compImageInfos(compImageInfos), compInfo(compInfo),
        compIdNameInfo(compIdNameInfo), maxTransferSize(maxTransferSize),
        updateManager(updateManager)
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

    /** @brief Handler for RequestFirmwareData request. Routes the request to
     * component updater. If component updater is not present sends command not
     * expected.
     *
     *  @param[in] request - Request message
     *  @param[in] payload_length - Request message payload length
     *  @return Response - PLDM Response message
     */
    Response requestFwData(const pldm_msg* request, size_t payloadLength);

    /** @brief Handler for TransferComplete request. Routes the request to
     * component updater. If component updater is not present sends command not
     * expected.
     *
     *  @param[in] request - Request message
     *  @param[in] payload_length - Request message payload length
     *  @return Response - PLDM Response message
     */
    Response transferComplete(const pldm_msg* request, size_t payloadLength);

    /** @brief Handler for VerifyComplete request. Routes the request to
     * component updater. If component updater is not present sends command not
     * expected.
     *
     *  @param[in] request - Request message
     *  @param[in] payload_length - Request message payload length
     *  @return Response - PLDM Response message
     */
    Response verifyComplete(const pldm_msg* request, size_t payloadLength);

    /** @brief Handler for ApplyComplete request. Routes the request to
     * component updater. If component updater is not present sends command not
     * expected.
     *
     *  @param[in] request - Request message
     *  @param[in] payload_length - Request message payload length
     *  @return Response - PLDM Response message
     */
    Response applyComplete(const pldm_msg* request, size_t payloadLength);

    /**
     * @brief update component update completion
     *
     * @param[in] compIndex - component index
     * @param[in] compStatus - component status
     */
    requester::Coroutine updateComponentCompletion(const size_t compIndex,
                                                   const ComponentUpdateStatus compStatus);

    /** @brief FirmwareDeviceIDRecord in the fw update package that matches this
     *         firmware device
     */
    const FirmwareDeviceIDRecord& fwDeviceIDRecord;

    /** @brief PLDM UA state machine
     */
    struct DeviceUpdaterState deviceUpdaterState;

  private:
    std::coroutine_handle<> deviceUpdaterHandle;

    /**
     * @brief device update handler
     *
     */
    void deviceUpdaterHandler();

    /**
     * @brief start device updater coroutine
     *
     * @return requester::Coroutine
     */
    requester::Coroutine startDeviceUpdate();

    /**
     * @brief send request update
     *
     * @return requester::Coroutine
     */
    requester::Coroutine sendRequestUpdate();
    /**
     * @brief process request update response
     *
     * @param[in] eid - mctp end point
     * @param[in] response - pldm response
     * @param[in] respMsgLen - response length
     * @return requester::Coroutine
     */
    requester::Coroutine processRequestUpdateResponse(mctp_eid_t eid,
                                                      const pldm_msg* response,
                                                      size_t respMsgLen);

    /** @brief Send PassComponentTable command request
     *
     *  @param[in] compOffset - component offset in compImageInfos
     *  @return requester::Coroutine
     */
    requester::Coroutine sendPassCompTableRequest(size_t offset);

    /**
     * @brief process pass component table response
     *
     * @param[in] eid - mctp end point
     * @param[in] response - pldm response
     * @param[in] respMsgLen - response length
     * @return requester::Coroutine
     */
    requester::Coroutine processPassCompTableResponse(mctp_eid_t eid,
                                                      const pldm_msg* response,
                                                      size_t respMsgLen);

    /**
     * @brief Send ActivateFirmware command request
     *
     * @return requester::Coroutine
     */
    requester::Coroutine sendActivateFirmwareRequest();

    /**
     * @brief process activate firmware response
     *
     * @param[in] eid - mctp end point
     * @param[in] response - pldm response
     * @param[in] respMsgLen - response size
     * @return requester::Coroutine
     */
    requester::Coroutine processActivateFirmwareResponse(
        mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen);

    /** @brief Endpoint ID of the firmware device */
    mctp_eid_t eid;

    /**
     * @brief Timer to handle Update mode idle timeout
     *
     */
    std::unique_ptr<sdbusplus::Timer> updateModeIdleTimer;

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
     * @brief send cancel update request
     *
     */
    requester::Coroutine sendCancelUpdateRequest();
    /**
     * @brief cancel update response handler
     *
     * @param[in] eid - mctp endpoint id
     * @param[in] response - cancel update response
     * @param[in] respMsgLen - response length
     */
    requester::Coroutine processCancelUpdateResponse(mctp_eid_t eid,
                                                     const pldm_msg* response,
                                                     size_t respMsgLen);

    /**
     * @brief List of components successfully updated
     *
     */
    std::vector<ComponentName> successCompNames;

    /**
     * @brief component updater map to hold component updater object and status
     *
     */
    std::map<ComponentIndex, std::pair<std::unique_ptr<ComponentUpdater>, bool>>
        componentUpdaterMap;
};

} // namespace fw_update

} // namespace pldm