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
#include "device_updater.hpp"

#include "activation.hpp"
#include "common/sleep.hpp"
#include "error_handling.hpp"
#include "update_manager.hpp"

#include <libpldm/firmware_update.h>

#include <phosphor-logging/lg2.hpp>

#include <chrono>
#include <functional>

PHOSPHOR_LOG2_USING;

namespace pldm
{

namespace fw_update
{

void DeviceUpdater::startFwUpdateFlow()
{
    pldmRequest = std::make_unique<sdeventplus::source::Defer>(
        updateManager->event,
        std::bind(&DeviceUpdater::deviceUpdaterHandler, this));
}

void DeviceUpdater::deviceUpdaterHandler()
{
    if (deviceUpdaterHandle.has_value())
    {
        auto& [scope, rcOpt] = *deviceUpdaterHandle;
        if (!rcOpt.has_value())
        {
            error("Update already in progress.");
            return;
        }
        stdexec::sync_wait(scope.on_empty());
        deviceUpdaterHandle.reset();
    }
    auto& [scope, rcOpt] = deviceUpdaterHandle.emplace();
    stdexec::start_detached(
        startDeviceUpdate() | stdexec::then([&](int rc) { rcOpt.emplace(rc); }),
        exec::default_task_context<void>(exec::inline_scheduler{}));
}

exec::task<int> DeviceUpdater::startDeviceUpdate()
{
    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
    size_t numComponents = applicableComponents.size();
    auto rc = co_await sendRequestUpdate();
    if (rc)
    {
        error("Error while sending RequestUpdate.");
        updateManager->updateDeviceCompletion(eid, false);
        co_return PLDM_ERROR;
    }
    for (size_t compIndex = 0; compIndex < numComponents; compIndex++)
    {
        rc = co_await sendPassCompTableRequest(compIndex);
        if (rc)
        {
            error("Error while sending PassComponentTable.");
            auto rc = co_await sendCancelUpdateRequest();
            if (rc)
            {
                error("Error while sending CancelUpdate.");
            }
            updateManager->updateDeviceCompletion(eid, false);
            co_return PLDM_ERROR;
        }
    }
    std::unique_ptr<ComponentUpdater> compUpdater =
        std::make_unique<ComponentUpdater>(
            eid, package, fwDeviceIDRecord, compImageInfos, compInfo,
            compIdNameInfo, maxTransferSize, updateManager, this,
            componentIndex);
    componentUpdaterMap.emplace(componentIndex,
                                std::make_pair(std::move(compUpdater), false));
    // start the first component updater, once component update is complete,
    // component updater calls updateComponentCompletion method based on
    // remaining applicable components new component updater will be initiated
    // in updateComponentCompletion method.
    rc = co_await componentUpdaterMap[componentIndex]
             .first->startComponentUpdater();
    if (rc)
    {
        error("Error while initiating component updater for "
              "ComponentIndex={COMPONENTINDEX}.",
              "COMPONENTINDEX", componentIndex);
        co_return PLDM_ERROR;
    }
    co_return PLDM_SUCCESS;
}

exec::task<int> DeviceUpdater::sendRequestUpdate(uint8_t retryCount)
{
    auto instanceId = updateManager->instanceIdDb.next(eid);
    // NumberOfComponents
    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
    numComponents = applicableComponents.size();
    // PackageDataLength
    const auto& fwDevicePkgData =
        std::get<FirmwareDevicePackageData>(fwDeviceIDRecord);
    // ComponentImageSetVersionString
    const auto& compImageSetVersion =
        std::get<ComponentImageSetVersion>(fwDeviceIDRecord);
    variable_field compImgSetVerStrInfo{};
    compImgSetVerStrInfo.ptr =
        reinterpret_cast<const uint8_t*>(compImageSetVersion.data());
    compImgSetVerStrInfo.length =
        static_cast<uint8_t>(compImageSetVersion.size());

    Request request(
        sizeof(pldm_msg_hdr) + sizeof(struct pldm_request_update_req) +
        compImgSetVerStrInfo.length);
    auto requestMsg = new (request.data()) pldm_msg;
    const pldm_msg* response = NULL;
    size_t respMsgLen = 0;

    auto rc = encode_request_update_req(
        instanceId, maxTransferSize, applicableComponents.size(),
        PLDM_FWUP_MIN_OUTSTANDING_REQ, fwDevicePkgData.size(),
        PLDM_STR_TYPE_ASCII, compImgSetVerStrInfo.length, &compImgSetVerStrInfo,
        requestMsg,
        sizeof(struct pldm_request_update_req) + compImgSetVerStrInfo.length);
    if (rc)
    {
        updateManager->instanceIdDb.free(eid, instanceId);
        error("encode_request_update_req failed, EID={EID}, RC={RC}", "EID",
              eid, "RC", rc);
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return rc;
    }

    printBuffer(pldm::utils::Tx, request,
                ("Send RequestUpdate for EID=" + std::to_string(eid)));
    rc = co_await sendRecvPldmMsgOverMctp(updateManager->handler, eid, request,
                                          &response, &respMsgLen);
    if (rc)
    {
        // Handle error scenario
        error("Failed to send request update for endpoint ID '{EID}', response "
              "code '{RC}'",
              "EID", eid, "RC", rc);

        if (rc == PLDM_REQUESTER_MCTP_TRANSPORT_ERROR)
        {
            handleTransportError(updateManager->handler, eid, "RequestUpdate");
        }
        else
        {
            bool logged = queryDeviceStatusAndLog(eid);
            if (!logged)
            {
                if (rc == PLDM_ERROR_NOT_READY)
                {
                    for (size_t compIndex = 0;
                         compIndex < applicableComponents.size(); compIndex++)
                    {
                        auto [messageStatus, oemMessageId, oemMessageError,
                              oemResolution] =
                            getOemMessage(PLDM_REQUEST_UPDATE,
                                          PLDM_FWUP_TIME_OUT);
                        if (messageStatus)
                        {
                            updateManager->createMessageRegistryResourceErrors(
                                eid, fwDeviceIDRecord, compIndex, oemMessageId,
                                oemMessageError, oemResolution);
                        }
                    }
                }
            }
        }
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return rc;
    }
    rc = co_await processRequestUpdateResponse(eid, response, respMsgLen,
                                               retryCount);
    if (rc == PLDM_ERROR_INVALID_DATA)
    {
        if (retryCount < maxDecodeFailureRetries)
        {
            warning(
                "Decode failure for RequestUpdate, retry {RETRY} of {MAX}, EID={EID}",
                "RETRY", retryCount + 1, "MAX", maxDecodeFailureRetries, "EID",
                eid);
            co_return co_await sendRequestUpdate(retryCount + 1);
        }
    }
    if (rc)
    {
        error("Error while processing RequestUpdateResponse");
    }
    co_return rc;
}

exec::task<int> DeviceUpdater::processRequestUpdateResponse(
    mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen,
    uint8_t retryCount)
{
    printBuffer(pldm::utils::Rx, response, respMsgLen,
                ("Received requestUpdate Response from EID=" +
                 std::to_string(eid)));

    uint8_t completionCode = 0;
    uint16_t fdMetaDataLen = 0;
    uint8_t fdWillSendPkgData = 0;

    auto rc = decode_request_update_resp(response, respMsgLen, &completionCode,
                                         &fdMetaDataLen, &fdWillSendPkgData);
    if (rc)
    {
        if (retryCount >= maxDecodeFailureRetries)
        {
            const auto& applicableComponents =
                std::get<ApplicableComponents>(fwDeviceIDRecord);
            for (size_t compIndex = 0; compIndex < applicableComponents.size();
                 compIndex++)
            {
                auto [messageStatus, oemMessageId, oemMessageError,
                      oemResolution] =
                    getOemMessage(PLDM_REQUEST_UPDATE, PLDM_ERROR);
                if (messageStatus)
                {
                    updateManager->createMessageRegistryResourceErrors(
                        eid, fwDeviceIDRecord, compIndex, oemMessageId,
                        oemMessageError, oemResolution);
                }
            }
            error(
                "Failed to decode request update response for endpoint ID '{EID}', "
                "response code '{RC}'",
                "EID", eid, "RC", rc);
            updateManager->updateDeviceCompletion(eid, false);
            deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        }
        co_return PLDM_ERROR_INVALID_DATA;
    }
    if (completionCode)
    {
        const auto& applicableComponents =
            std::get<ApplicableComponents>(fwDeviceIDRecord);
        for (size_t compIndex = 0; compIndex < applicableComponents.size();
             compIndex++)
        {
            updateManager->createMessageRegistry(
                eid, fwDeviceIDRecord, compIndex, transferFailed, "",
                PLDM_REQUEST_UPDATE, completionCode);
        }
        error("Failure in request update response for endpoint ID '{EID}', "
              "completion code '{CC}'",
              "EID", eid, "CC", completionCode);
        updateManager->updateDeviceCompletion(eid, false);
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return PLDM_ERROR;
    }

    deviceUpdaterState.nextState(deviceUpdaterState.current, componentIndex,
                                 numComponents);
    co_return PLDM_SUCCESS;
}

exec::task<int> DeviceUpdater::sendPassCompTableRequest(size_t offset,
                                                        uint8_t retryCount)
{
    pldmRequest.reset();

    auto instanceId = updateManager->instanceIdDb.next(eid);
    // TransferFlag
    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
    uint8_t transferFlag = 0;
    const pldm_msg* response = NULL;
    size_t respMsgLen = 0;
    if (applicableComponents.size() == 1)
    {
        transferFlag = PLDM_START_AND_END;
    }
    else if (offset == 0)
    {
        transferFlag = PLDM_START;
    }
    else if (offset == applicableComponents.size() - 1)
    {
        transferFlag = PLDM_END;
    }
    else
    {
        transferFlag = PLDM_MIDDLE;
    }
    const auto& comp = compImageInfos[applicableComponents[offset]];
    // ComponentClassification
    CompClassification compClassification = std::get<static_cast<size_t>(
        ComponentImageInfoPos::CompClassificationPos)>(comp);
    // ComponentIdentifier
    CompIdentifier compIdentifier =
        std::get<static_cast<size_t>(ComponentImageInfoPos::CompIdentifierPos)>(
            comp);
    // ComponentClassificationIndex
    CompClassificationIndex compClassificationIndex{};
    auto compKey = std::make_pair(compClassification, compIdentifier);
    if (compInfo.contains(compKey))
    {
        auto search = compInfo.find(compKey);
        compClassificationIndex = std::get<0>(search->second);
    }
    else
    {
        updateManager->instanceIdDb.free(eid, instanceId);
        // Handle error scenario
        error("Failed to find component classification '{CLASSIFICATION}' and "
              "identifier '{IDENTIFIER}'",
              "CLASSIFICATION", compClassification, "IDENTIFIER",
              compIdentifier);
        auto errorMsg =
            "The component information in the firmware package does "
            "not match with the device";
        auto resolution =
            "Verify the FW package has devices that are listed in "
            "the Redfish FW Inventory.";
        updateManager->createMessageRegistryResourceErrors(
            eid, fwDeviceIDRecord, componentIndex, resourceErrorDetected,
            errorMsg, resolution);
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return PLDM_ERROR;
    }
    // ComponentComparisonStamp
    CompComparisonStamp compComparisonStamp = std::get<static_cast<size_t>(
        ComponentImageInfoPos::CompComparisonStampPos)>(comp);
    // ComponentVersionString
    const auto& compVersion =
        std::get<static_cast<size_t>(ComponentImageInfoPos::CompVersionPos)>(
            comp);
    variable_field compVerStrInfo{};
    compVerStrInfo.ptr = reinterpret_cast<const uint8_t*>(compVersion.data());
    compVerStrInfo.length = static_cast<uint8_t>(compVersion.size());

    Request request(
        sizeof(pldm_msg_hdr) + sizeof(struct pldm_pass_component_table_req) +
        compVerStrInfo.length);
    auto requestMsg = new (request.data()) pldm_msg;
    auto rc = encode_pass_component_table_req(
        instanceId, transferFlag, compClassification, compIdentifier,
        compClassificationIndex, compComparisonStamp, PLDM_STR_TYPE_ASCII,
        compVerStrInfo.length, &compVerStrInfo, requestMsg,
        sizeof(pldm_pass_component_table_req) + compVerStrInfo.length);
    if (rc)
    {
        updateManager->instanceIdDb.free(eid, instanceId);
        error(
            "Failed to encode pass component table req for endpoint ID '{EID}', "
            "response code '{RC}'",
            "EID", eid, "RC", rc);
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return rc;
    }

    printBuffer(pldm::utils::Tx, request,
                ("Send PassCompTable for EID=" + std::to_string(eid) +
                 " ,ComponentIndex=" + std::to_string(componentIndex)));

    rc = co_await sendRecvPldmMsgOverMctp(updateManager->handler, eid, request,
                                          &response, &respMsgLen);
    if (rc)
    {
        error("Error while sending mctp request for PassCompTable.");

        if (rc == PLDM_REQUESTER_MCTP_TRANSPORT_ERROR)
        {
            handleTransportError(updateManager->handler, eid,
                                 "PassComponentTable");
        }
        else
        {
            bool logged = queryDeviceStatusAndLog(eid);
            if (!logged)
            {
                if (rc == PLDM_ERROR_NOT_READY)
                {
                    auto [messageStatus, oemMessageId, oemMessageError,
                          oemResolution] =
                        getOemMessage(PLDM_PASS_COMPONENT_TABLE,
                                      PLDM_FWUP_TIME_OUT);
                    if (messageStatus)
                    {
                        updateManager->createMessageRegistryResourceErrors(
                            eid, fwDeviceIDRecord, componentIndex, oemMessageId,
                            oemMessageError, oemResolution);
                    }
                }
            }
        }
        co_return rc;
    }

    rc = co_await processPassCompTableResponse(eid, response, respMsgLen,
                                               retryCount);
    if (rc == PLDM_ERROR_INVALID_DATA)
    {
        if (retryCount < maxDecodeFailureRetries)
        {
            warning(
                "Decode failure for PassCompTable, retry {RETRY} of {MAX}, EID={EID}",
                "RETRY", retryCount + 1, "MAX", maxDecodeFailureRetries, "EID",
                eid);
            co_return co_await sendPassCompTableRequest(offset, retryCount + 1);
        }
    }
    if (rc)
    {
        error("Failed to send pass component table request for endpoint ID "
              "'{EID}', response code '{RC}'",
              "EID", eid, "RC", rc);
    }
    co_return rc;
}

exec::task<int> DeviceUpdater::processPassCompTableResponse(
    mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen,
    uint8_t retryCount)
{
    printBuffer(
        pldm::utils::Rx, response, respMsgLen,
        ("Received Response for PassCompTable from EID=" + std::to_string(eid) +
         " ,ComponentIndex=" + std::to_string(componentIndex)));

    uint8_t completionCode = 0;
    uint8_t compResponse = 0;
    uint8_t compResponseCode = 0;

    auto rc =
        decode_pass_component_table_resp(response, respMsgLen, &completionCode,
                                         &compResponse, &compResponseCode);
    if (rc)
    {
        if (retryCount >= maxDecodeFailureRetries)
        {
            const auto& applicableComponents =
                std::get<ApplicableComponents>(fwDeviceIDRecord);
            for (size_t compIndex = 0; compIndex < applicableComponents.size();
                 compIndex++)
            {
                auto [messageStatus, oemMessageId, oemMessageError,
                      oemResolution] =
                    getOemMessage(PLDM_PASS_COMPONENT_TABLE, PLDM_ERROR);
                if (messageStatus)
                {
                    updateManager->createMessageRegistryResourceErrors(
                        eid, fwDeviceIDRecord, compIndex, oemMessageId,
                        oemMessageError, oemResolution);
                }
            }
            error(
                "Failed to decode pass component table response for endpoint ID "
                "'{EID}', response code '{RC}'",
                "EID", eid, "RC", rc);
            updateManager->updateDeviceCompletion(eid, false);
            deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        }
        co_return PLDM_ERROR_INVALID_DATA;
    }
    if (completionCode)
    {
        // Handle error scenario
        error(
            "Failed to pass component table response for endpoint ID '{EID}', "
            "completion code '{CC}'",
            "EID", eid, "CC", completionCode);
        auto [messageStatus, oemMessageId, oemMessageError, oemResolution] =
            getOemMessage(PLDM_PASS_COMPONENT_TABLE, completionCode);
        if (messageStatus)
        {
            updateManager->createMessageRegistryResourceErrors(
                eid, fwDeviceIDRecord, componentIndex, oemMessageId,
                oemMessageError, oemResolution);
        }
        updateManager->updateDeviceCompletion(eid, false);
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return PLDM_ERROR;
    }
    if (compResponse)
    {
        info("In PassComponentTable, componentResponse is non-zero. Component "
             "may be updateable EID={EID}, ComponentResponse={CR}, "
             "ComponentResponseCode= {CRC}",
             "EID", eid, "CR", compResponse, "CRC", compResponseCode);
    }
    deviceUpdaterState.nextState(deviceUpdaterState.current, componentIndex,
                                 numComponents);
    co_return PLDM_SUCCESS;
}

Response DeviceUpdater::requestFwData(const pldm_msg* request,
                                      size_t payloadLength)
{
    if (componentUpdaterMap.contains(componentIndex))
    {
        return componentUpdaterMap[componentIndex].first->requestFwData(
            request, payloadLength);
    }
    else
    {
        return sendCommandNotExpectedResponse(request, payloadLength);
    }
}

Response DeviceUpdater::transferComplete(const pldm_msg* request,
                                         size_t payloadLength)
{
    if (componentUpdaterMap.contains(componentIndex))
    {
        return componentUpdaterMap[componentIndex].first->transferComplete(
            request, payloadLength);
    }
    else
    {
        return sendCommandNotExpectedResponse(request, payloadLength);
    }
}

Response DeviceUpdater::verifyComplete(const pldm_msg* request,
                                       size_t payloadLength)
{
    if (componentUpdaterMap.contains(componentIndex))
    {
        return componentUpdaterMap[componentIndex].first->verifyComplete(
            request, payloadLength);
    }
    else
    {
        return sendCommandNotExpectedResponse(request, payloadLength);
    }
}

Response DeviceUpdater::applyComplete(const pldm_msg* request,
                                      size_t payloadLength)
{
    if (componentUpdaterMap.contains(componentIndex))
    {
        return componentUpdaterMap[componentIndex].first->applyComplete(
            request, payloadLength);
    }
    else
    {
        return sendCommandNotExpectedResponse(request, payloadLength);
    }
}

exec::task<int> DeviceUpdater::sendActivateFirmwareRequest(uint8_t retryCount)
{
    pldmRequest.reset();
    auto instanceId = updateManager->instanceIdDb.next(eid);
    Request request(
        sizeof(pldm_msg_hdr) + sizeof(struct pldm_activate_firmware_req));
    auto requestMsg = new (request.data()) pldm_msg;
    const pldm_msg* response = NULL;
    size_t respMsgLen = 0;

    bool useSelfContained = isLiveActivationSupported();
    uint8_t activationPolicy =
        useSelfContained ? PLDM_ACTIVATE_SELF_CONTAINED_COMPONENTS
                         : PLDM_NOT_ACTIVATE_SELF_CONTAINED_COMPONENTS;

    auto rc =
        encode_activate_firmware_req(instanceId, activationPolicy, requestMsg,
                                     sizeof(pldm_activate_firmware_req));
    if (rc)
    {
        updateManager->instanceIdDb.free(eid, instanceId);
        error("Failed to encode activate firmware req for endpoint ID '{EID}', "
              "response code '{RC}'",
              "EID", eid, "RC", rc);
        co_return rc;
    }

    printBuffer(pldm::utils::Tx, request,
                ("Send ActivateFirmware for EID=" + std::to_string(eid)));
    rc = co_await sendRecvPldmMsgOverMctp(updateManager->handler, eid, request,
                                          &response, &respMsgLen);
    if (rc)
    {
        error(
            "Error while sending mctp request for ActivateFirmware. EID={EID}",
            "EID", eid);
        if (rc == PLDM_REQUESTER_MCTP_TRANSPORT_ERROR)
        {
            handleTransportError(updateManager->handler, eid,
                                 "ActivateFirmware");
        }
        else
        {
            bool logged = queryDeviceStatusAndLog(eid);

            updateManager->updateDeviceCompletion(eid, false);
            deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
            const auto& applicableComponents =
                std::get<ApplicableComponents>(fwDeviceIDRecord);
            for (size_t compIndex = 0; compIndex < applicableComponents.size();
                 compIndex++)
            {
                if (!logged)
                {
                    updateManager->createMessageRegistry(
                        eid, fwDeviceIDRecord, compIndex, activateFailed, "",
                        PLDM_ACTIVATE_FIRMWARE, PLDM_FWUP_TIME_OUT);
                }
                else
                {
                    updateManager->createMessageRegistry(
                        eid, fwDeviceIDRecord, compIndex, activateFailed);
                }
            }
        }
        co_return rc;
    }
    rc = co_await processActivateFirmwareResponse(eid, response, respMsgLen,
                                                  retryCount);
    if (rc == PLDM_ERROR_INVALID_DATA)
    {
        if (retryCount < maxDecodeFailureRetries)
        {
            warning(
                "Decode failure for ActivateFirmware, retry {RETRY} of {MAX}, EID={EID}",
                "RETRY", retryCount + 1, "MAX", maxDecodeFailureRetries, "EID",
                eid);
            co_return co_await sendActivateFirmwareRequest(retryCount + 1);
        }
    }
    if (rc)
    {
        error(
            "Failed to send activate firmware request for endpoint ID '{EID}', "
            "response code '{RC}'",
            "EID", eid, "RC", rc);
    }
    co_return rc;
}

exec::task<int> DeviceUpdater::processActivateFirmwareResponse(
    mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen,
    uint8_t retryCount)
{
    printBuffer(pldm::utils::Rx, response, respMsgLen,
                ("Received ActivateFirmware Response from EID=" +
                 std::to_string(eid)));

    uint8_t completionCode = 0;
    uint16_t estimatedTimeForActivation = 0;

    auto rc = decode_activate_firmware_resp(
        response, respMsgLen, &completionCode, &estimatedTimeForActivation);
    if (rc)
    {
        if (retryCount >= maxDecodeFailureRetries)
        {
            const auto& applicableComponents =
                std::get<ApplicableComponents>(fwDeviceIDRecord);
            for (size_t compIndex = 0; compIndex < applicableComponents.size();
                 compIndex++)
            {
                updateManager->createMessageRegistry(
                    eid, fwDeviceIDRecord, compIndex, activateFailed, "",
                    PLDM_ACTIVATE_FIRMWARE, PLDM_ERROR);
            }
            error("Failed to decode activate firmware response for endpoint ID "
                  "'{EID}', response code '{RC}'",
                  "EID", eid, "RC", rc);
            updateManager->updateDeviceCompletion(eid, false);
            deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        }
        co_return PLDM_ERROR_INVALID_DATA;
    }

    // On receiving ActivateFirmware response success/failure make the UA state
    // to Invalid to further not responds to any PLDM Type 5 requests from FD.
    deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);

    if (completionCode)
    {
        const auto& applicableComponents =
            std::get<ApplicableComponents>(fwDeviceIDRecord);

        if (completionCode == PLDM_FWUP_SELF_CONTAINED_ACTIVATION_NOT_PERMITTED)
        {
            error(
                "Self-contained activation not permitted for endpoint ID '{EID}'",
                "EID", eid);
        }

        for (size_t compIndex = 0; compIndex < applicableComponents.size();
             compIndex++)
        {
            updateManager->createMessageRegistry(eid, fwDeviceIDRecord,
                                                 compIndex, activateFailed);
        }
        error("Failed to activate firmware response for endpoint ID '{EID}', "
              "completion code '{CC}'",
              "EID", eid, "CC", completionCode);
        updateManager->updateDeviceCompletion(eid, false);
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return PLDM_ERROR;
    }

    bool selfContainedActivation = isLiveActivationSupported();
    if (selfContainedActivation)
    {
        if (estimatedTimeForActivation > 0)
        {
            info("Self-contained activation in progress for EID={EID}, "
                 "estimated time: {TIME} seconds",
                 "EID", eid, "TIME", estimatedTimeForActivation);

            co_await waitForSelfContainedActivation(estimatedTimeForActivation);
        }
        else
        {
            info("Self-contained activation with zero estimated time for "
                 "EID={EID}, checking status immediately",
                 "EID", eid);

            auto status = co_await pollSelfContainedActivationStatus();

            if (status == ActivationPollStatus::Success)
            {
                info("Self-contained activation successful for EID={EID}",
                     "EID", eid);

                const auto& applicableComponents =
                    std::get<ApplicableComponents>(fwDeviceIDRecord);
                for (size_t compIndex = 0;
                     compIndex < applicableComponents.size(); compIndex++)
                {
                    if (componentUpdaterMap[compIndex].second == true)
                    {
                        updateManager->createMessageRegistry(
                            eid, fwDeviceIDRecord, compIndex,
                            activateSuccessful);
                    }
                }

                updateManager->updateDeviceCompletion(eid, true,
                                                      successCompNames);

                if (updateManager &&
                    updateManager->refreshSingleEndpointCallback)
                {
                    info("Refreshing firmware version for EID={EID} after "
                         "activation",
                         "EID", eid);
                    co_await updateManager->refreshSingleEndpointCallback(
                        eid, true);
                }

                deviceUpdaterState.nextState(deviceUpdaterState.current,
                                             componentIndex, numComponents);
            }
            else
            {
                error("Self-contained activation failed for EID={EID}", "EID",
                      eid);

                const auto& applicableComponents =
                    std::get<ApplicableComponents>(fwDeviceIDRecord);
                for (size_t compIndex = 0;
                     compIndex < applicableComponents.size(); compIndex++)
                {
                    updateManager->createMessageRegistry(
                        eid, fwDeviceIDRecord, compIndex, activateFailed);
                }

                updateManager->updateDeviceCompletion(eid, false);
                deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
            }
        }
    }
    else
    {
        const auto& applicableComponents =
            std::get<ApplicableComponents>(fwDeviceIDRecord);
        for (size_t compIndex = 0; compIndex < applicableComponents.size();
             compIndex++)
        {
            if (componentUpdaterMap[compIndex].second == true)
            {
                updateManager->createMessageRegistry(
                    eid, fwDeviceIDRecord, compIndex, awaitToActivate,
                    updateManager->getActivationMethod(
                        componentActivationModifications));
            }
        }

        updateManager->updateDeviceCompletion(eid, true, successCompNames);
        deviceUpdaterState.nextState(deviceUpdaterState.current, componentIndex,
                                     numComponents);
    }

    co_return PLDM_SUCCESS;
}

exec::task<int> DeviceUpdater::updateComponentCompletion(
    const size_t compIndex, const ComponentUpdateStatus compStatus)
{
    if (compStatus == ComponentUpdateStatus::UpdateComplete)
    {
        componentUpdaterMap[compIndex].second = true;
    }
    else
    {
        componentUpdaterMap[compIndex].second = false;
    }
    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
    if (compStatus == ComponentUpdateStatus::UpdateComplete)
    {
        successCompNames.emplace_back(updateManager->getComponentName(
            eid, fwDeviceIDRecord, componentIndex));
    }
    if (compIndex < applicableComponents.size() - 1)
    {
        updateManager->updateActivationProgress(); // for previous component
        componentIndex++;
        std::unique_ptr<ComponentUpdater> compUpdater =
            std::make_unique<ComponentUpdater>(
                eid, package, fwDeviceIDRecord, compImageInfos, compInfo,
                compIdNameInfo, maxTransferSize, updateManager, this,
                componentIndex);
        componentUpdaterMap.emplace(
            componentIndex, std::make_pair(std::move(compUpdater), false));
        auto rc = co_await componentUpdaterMap[componentIndex]
                      .first->startComponentUpdater();
        if (rc)
        {
            error("Error starting component updater for index {INDEX}", "INDEX",
                  componentIndex);
            co_return rc;
        }
        co_return PLDM_SUCCESS;
    }
    else
    {
        for (const auto& compUpdater : componentUpdaterMap)
        {
            // Activate firmware if atleast one component update is success.
            if (compUpdater.second.second == true)
            {
                auto rc = co_await sendActivateFirmwareRequest();
                if (rc)
                {
                    error("Error while sending ActivateFirmware.");
                    co_return PLDM_ERROR;
                }
                co_return PLDM_SUCCESS;
            }
        }
        // None of the component update is success, cancel the update
        auto rc = co_await sendCancelUpdateRequest();
        if (rc)
        {
            error("Error while sending CancelUpdate.");
            updateManager->updateDeviceCompletion(eid, false);
            co_return PLDM_ERROR;
        }
        if (compStatus != ComponentUpdateStatus::UpdateFailed)
        {
            updateManager->updateDeviceCompletion(eid, true);
        }
        else
        {
            updateManager->updateDeviceCompletion(eid, false);
        }
        co_return PLDM_SUCCESS;
    }
}

exec::task<int> DeviceUpdater::sendCancelUpdateRequest()
{
    deviceUpdaterState.set(DeviceUpdaterSequence::CancelUpdate);
    auto instanceId = updateManager->instanceIdDb.next(eid);
    Request request(sizeof(pldm_msg_hdr));
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    const pldm_msg* response = NULL;
    size_t respMsgLen = 0;

    auto rc = encode_cancel_update_req(instanceId, requestMsg,
                                       PLDM_CANCEL_UPDATE_REQ_BYTES);
    if (rc)
    {
        updateManager->instanceIdDb.free(eid, instanceId);
        error("encode_cancel_update_req failed, EID={EID}, RC={RC}", "EID", eid,
              "RC", rc);
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        updateManager->updateDeviceCompletion(eid, false);
        co_return rc;
    }

    printBuffer(pldm::utils::Tx, request,
                ("Send CancelUpdate for EID=" + std::to_string(eid)));
    rc = co_await sendRecvPldmMsgOverMctp(updateManager->handler, eid, request,
                                          &response, &respMsgLen);
    if (rc)
    {
        error("Error while sending mctp request for CancelUpdate. EID={EID}",
              "EID", eid);

        if (rc == PLDM_REQUESTER_MCTP_TRANSPORT_ERROR)
        {
            handleTransportError(updateManager->handler, eid, "CancelUpdate");
        }
        else
        {
            [[maybe_unused]] bool logged = queryDeviceStatusAndLog(eid);
        }
        co_return rc;
    }
    rc = co_await processCancelUpdateResponse(eid, response, respMsgLen);
    if (rc)
    {
        error("Error while processing CancelUpdate Response. EID={EID}", "EID",
              eid);
    }
    co_return rc;
}

exec::task<int> DeviceUpdater::processCancelUpdateResponse(
    mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen)
{
    printBuffer(pldm::utils::Rx, response, respMsgLen,
                ("Received CancelUpdate Response from EID=" +
                 std::to_string(eid)));

    uint8_t completionCode = 0;
    bool8_t nonFunctioningComponentIndication;
    bitfield64_t nonFunctioningComponentBitmap{0};
    auto rc = decode_cancel_update_resp(response, respMsgLen, &completionCode,
                                        &nonFunctioningComponentIndication,
                                        &nonFunctioningComponentBitmap);
    if (rc)
    {
        error("Decoding CancelUpdate response failed, EID={EID}, CC={CC}",
              "EID", eid, "CC", completionCode);
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return rc;
    }
    if (completionCode && completionCode != PLDM_FWUP_NOT_IN_UPDATE_MODE)
    {
        error("CancelUpdate response failed with error, EID={EID}, CC={CC}",
              "EID", eid, "CC", completionCode);
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return PLDM_ERROR;
    }
    co_return PLDM_SUCCESS;
}

bool DeviceUpdater::isLiveActivationSupported() const
{
    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);

    if (compInfo.empty())
    {
        return false;
    }

    bool userRequestsImmediate = updateManager->isApplyTimeImmediate();
    bool packageRequestsSelfContained = false;

    for (const auto& compIndex : applicableComponents)
    {
        const auto& comp = compImageInfos[compIndex];
        auto compClassification = std::get<static_cast<size_t>(
            ComponentImageInfoPos::CompClassificationPos)>(comp);
        auto compIdentifier = std::get<static_cast<size_t>(
            ComponentImageInfoPos::CompIdentifierPos)>(comp);
        auto compKey = std::make_pair(compClassification, compIdentifier);

        auto compInfoIter = compInfo.find(compKey);
        if (compInfoIter == compInfo.end())
        {
            return false;
        }

        auto activationMethods = std::get<2>(compInfoIter->second);
        if (!(activationMethods & (1 << PLDM_ACTIVATION_SELF_CONTAINED)))
        {
            return false;
        }

        auto reqCompActivationMethod = std::get<static_cast<size_t>(
            ComponentImageInfoPos::ReqCompActivationMethodPos)>(comp);
        if (reqCompActivationMethod.test(PLDM_ACTIVATION_SELF_CONTAINED))
        {
            packageRequestsSelfContained = true;
        }
    }

    if (!(componentActivationModifications.value &
          (1 << PLDM_ACTIVATION_SELF_CONTAINED)))
    {
        return false;
    }

    if (!userRequestsImmediate && !packageRequestsSelfContained)
    {
        return false;
    }

    return true;
}

exec::task<void> DeviceUpdater::waitForSelfContainedActivation(
    uint16_t estimatedTime)
{
    uint16_t numPolls = (estimatedTime / activationPollInterval.count()) + 1;
    uint64_t pollIntervalUsec =
        std::chrono::duration_cast<std::chrono::microseconds>(
            activationPollInterval)
            .count();

    info("Starting polling for self-contained activation, EID={EID}, "
         "estimated time: {TIME} seconds, poll interval: {INTERVAL} seconds, "
         "num polls: {NUMPOLLS}",
         "EID", eid, "TIME", estimatedTime, "INTERVAL",
         activationPollInterval.count(), "NUMPOLLS", numPolls);

    for (uint16_t pollCount = 0; pollCount < numPolls; pollCount++)
    {
        auto rc = co_await timer::Sleep(updateManager->event, pollIntervalUsec,
                                        timer::TimerEventPriority::NonPriority);
        if (rc != PLDM_SUCCESS)
        {
            error("Poll timer failed for self-contained activation for "
                  "endpoint ID {EID}",
                  "EID", eid);

            const auto& applicableComponents =
                std::get<ApplicableComponents>(fwDeviceIDRecord);
            for (size_t compIndex = 0; compIndex < applicableComponents.size();
                 compIndex++)
            {
                updateManager->createMessageRegistry(eid, fwDeviceIDRecord,
                                                     compIndex, activateFailed);
            }

            updateManager->updateDeviceCompletion(eid, false);
            deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
            co_return;
        }

        auto status = co_await pollSelfContainedActivationStatus();

        if (status == ActivationPollStatus::Success)
        {
            info("Self-contained activation successful for EID={EID} after "
                 "{POLLS} polls",
                 "EID", eid, "POLLS", pollCount + 1);

            const auto& applicableComponents =
                std::get<ApplicableComponents>(fwDeviceIDRecord);
            for (size_t compIndex = 0; compIndex < applicableComponents.size();
                 compIndex++)
            {
                if (componentUpdaterMap[compIndex].second == true)
                {
                    updateManager->createMessageRegistry(
                        eid, fwDeviceIDRecord, compIndex, activateSuccessful);
                }
            }

            if (updateManager && updateManager->refreshSingleEndpointCallback)
            {
                info("Refreshing firmware version for EID={EID} after "
                     "activation",
                     "EID", eid);
                co_await updateManager->refreshSingleEndpointCallback(
                    eid, true);
            }

            updateManager->updateDeviceCompletion(eid, true, successCompNames);
            deviceUpdaterState.nextState(deviceUpdaterState.current,
                                         componentIndex, numComponents);
            co_return;
        }
        else if (status == ActivationPollStatus::Failed)
        {
            error("Self-contained activation failed during polling for "
                  "EID={EID}",
                  "EID", eid);

            const auto& applicableComponents =
                std::get<ApplicableComponents>(fwDeviceIDRecord);
            for (size_t compIndex = 0; compIndex < applicableComponents.size();
                 compIndex++)
            {
                updateManager->createMessageRegistry(eid, fwDeviceIDRecord,
                                                     compIndex, activateFailed);
            }

            updateManager->updateDeviceCompletion(eid, false);
            deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
            co_return;
        }
        // status == InProgress, continue polling
    }

    error("Self-contained activation timed out for EID={EID}", "EID", eid);

    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
    for (size_t compIndex = 0; compIndex < applicableComponents.size();
         compIndex++)
    {
        updateManager->createMessageRegistry(eid, fwDeviceIDRecord, compIndex,
                                             activateFailed);
    }

    updateManager->updateDeviceCompletion(eid, false);
    deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);

    co_return;
}

exec::task<ActivationPollStatus>
    DeviceUpdater::pollSelfContainedActivationStatus()
{
    auto instanceId = updateManager->instanceIdDb.next(eid);
    Request request(sizeof(pldm_msg_hdr) + PLDM_GET_STATUS_REQ_BYTES);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    auto rc = encode_get_status_req(instanceId, requestMsg,
                                    PLDM_GET_STATUS_REQ_BYTES);
    if (rc)
    {
        updateManager->instanceIdDb.free(eid, instanceId);
        error("Failed to encode get status req for endpoint ID '{EID}', "
              "response code '{RC}'",
              "EID", eid, "RC", rc);
        co_return ActivationPollStatus::Failed;
    }

    if (updateManager->fwDebug)
    {
        printBuffer(pldm::utils::Tx, request,
                    ("Send GetStatus (poll) for EID=" + std::to_string(eid)));
    }

    const pldm_msg* response = nullptr;
    size_t respMsgLen = 0;

    printBuffer(pldm::utils::Tx, request,
                ("Send GetStatus for EID=" + std::to_string(eid)));
    rc = co_await sendRecvPldmMsgOverMctp(updateManager->handler, eid, request,
                                          &response, &respMsgLen);

    if (rc)
    {
        error("Error while sending GetStatus request during Self activation"
              " EID={EID}, ComponentIndex={COMPONENTINDEX}",
              "EID", eid, "COMPONENTINDEX", componentIndex);

        bool logged = queryDeviceStatusAndLog(eid);
        if (!logged)
        {
            {
                auto [messageStatus, oemMessageId, oemMessageError,
                      oemResolution] =
                    getOemMessage(PLDM_GET_STATUS, COMMAND_TIMEOUT);
                if (messageStatus)
                {
                    updateManager->createMessageRegistryResourceErrors(
                        eid, fwDeviceIDRecord, componentIndex, oemMessageId,
                        oemMessageError, oemResolution);
                }
            }
        }
        co_return ActivationPollStatus::Failed;
    }

    printBuffer(pldm::utils::Rx, response, respMsgLen,
                ("Received GetStatus Response from EID=" +
                 std::to_string(eid)));

    uint8_t completionCode = 0;
    uint8_t currentState = 0;
    uint8_t previousState = 0;
    uint8_t auxState = 0;
    uint8_t auxStateStatus = 0;
    uint8_t progressPercent = 0;
    uint8_t reasonCode = 0;
    bitfield32_t updateOptionFlagsEnabled{};

    rc = decode_get_status_resp(
        response, respMsgLen, &completionCode, &currentState, &previousState,
        &auxState, &auxStateStatus, &progressPercent, &reasonCode,
        &updateOptionFlagsEnabled);
    if (rc)
    {
        error("Failed to decode get status poll response for endpoint ID "
              "'{EID}', response code '{RC}'",
              "EID", eid, "RC", rc);
        co_return ActivationPollStatus::Failed;
    }

    if (completionCode)
    {
        error("GetStatus poll response failed with completion code '{CC}', "
              "EID={EID}",
              "CC", completionCode, "EID", eid);
        co_return ActivationPollStatus::Failed;
    }

    if (currentState == PLDM_FD_STATE_IDLE &&
        auxState == PLDM_FD_IDLE_LEARN_COMPONENTS_READ_XFER)
    {
        info("Activation poll: success detected for EID={EID}", "EID", eid);
        co_return ActivationPollStatus::Success;
    }
    else if (currentState == PLDM_FD_STATE_ACTIVATE &&
             auxState == PLDM_FD_OPERATION_IN_PROGRESS)
    {
        info("Activation poll: still in progress for EID={EID}", "EID", eid);
        co_return ActivationPollStatus::InProgress;
    }
    else
    {
        error("Activation poll returned unexpected state for endpoint ID "
              "{EID}, FD state={STATE}, auxState={AUXSTATE}, "
              "auxStateStatus={AUXSTATUS}",
              "EID", eid, "STATE", currentState, "AUXSTATE", auxState,
              "AUXSTATUS", auxStateStatus);
        co_return ActivationPollStatus::Failed;
    }
}

} // namespace fw_update

} // namespace pldm
