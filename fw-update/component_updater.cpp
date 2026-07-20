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
#include "component_updater.hpp"

#include "libpldm/firmware_update.h"

#include "activation.hpp"
#include "error_handling.hpp"
#include "fw_update_utility.hpp"
#include "update_manager.hpp"

#include <phosphor-logging/lg2.hpp>

#include <format>
#include <functional>

namespace pldm
{

namespace fw_update
{

exec::task<int> ComponentUpdater::sendRecvPldmMsgOverMctp(
    mctp_eid_t eid, Request& request, const pldm_msg** responseMsg,
    size_t* responseLen)
{
    int rc = 0;
    try
    {
        std::tie(rc, *responseMsg, *responseLen) =
            co_await updateManager->handler.sendRecvMsg(eid,
                                                        std::move(request));
    }
    catch (const sdbusplus::exception_t& e)
    {
        error("Send and Receive PLDM message over MCTP throw error - {ERROR}.",
              "ERROR", e);
        co_return PLDM_ERROR;
    }
    catch (const int& e)
    {
        error(
            "Send and Receive PLDM message over MCTP throw int error - {ERROR}.",
            "ERROR", e);
        co_return PLDM_ERROR;
    }

    co_return rc;
}

exec::task<int> ComponentUpdater::startComponentUpdater()
{
    componentUpdateStartTime = std::chrono::steady_clock::now();
    auto rc = co_await sendUpdateComponentRequest(componentIndex);
    if (rc)
    {
        error("Error while sending component update request."
              " EID={EID}, ComponentIndex={COMPONENTINDEX}",
              "EID", eid, "COMPONENTINDEX", componentIndex);
    }
    co_return rc;
}

exec::task<int> ComponentUpdater::sendUpdateComponentRequest(size_t offset,
                                                             uint8_t retryCount)
{
    pldmRequest.reset();

    auto instanceId = updateManager->instanceIdDb.next(eid);
    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
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
        // Handle error scenario
        error(
            "Failed to find component classification '{CLASSIFICATION}' and identifier '{IDENTIFIER}'",
            "CLASSIFICATION", compClassification, "IDENTIFIER", compIdentifier);
    }

    const auto& compOptions =
        std::get<static_cast<size_t>(ComponentImageInfoPos::CompOptionsPos)>(
            comp);
    // UpdateOptionFlags
    bitfield32_t updateOptionFlags = {0};
    updateOptionFlags.bits.bit0 =
        updateManager->forceUpdate || compOptions.test(forceUpdateBit);
    // ComponentVersion
    const auto& compVersion = std::get<7>(comp);
    variable_field compVerStrInfo{};
    compVerStrInfo.ptr = reinterpret_cast<const uint8_t*>(compVersion.data());
    compVerStrInfo.length = static_cast<uint8_t>(compVersion.size());

    Request request(
        sizeof(pldm_msg_hdr) + sizeof(struct pldm_update_component_req) +
        compVerStrInfo.length);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    const pldm_msg* response = NULL;
    size_t respMsgLen = 0;
    auto rc = encode_update_component_req(
        instanceId, compClassification, compIdentifier, compClassificationIndex,
        std::get<static_cast<size_t>(
            ComponentImageInfoPos::CompComparisonStampPos)>(comp),
        std::get<static_cast<size_t>(ComponentImageInfoPos::CompSizePos)>(comp),
        updateOptionFlags, PLDM_STR_TYPE_ASCII, compVerStrInfo.length,
        &compVerStrInfo, requestMsg,
        sizeof(pldm_update_component_req) + compVerStrInfo.length);
    if (rc)
    {
        updateManager->instanceIdDb.free(eid, instanceId);
        error(
            "Failed to encode update component req for endpoint ID '{EID}', response code '{RC}'",
            "EID", eid, "RC", rc);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        co_return PLDM_ERROR;
    }

    printBuffer(pldm::utils::Tx, request,
                ("Send UpdateComponent for EID=" + std::to_string(eid) +
                 " ,ComponentIndex=" + std::to_string(componentIndex)));
    rc = co_await sendRecvPldmMsgOverMctp(eid, request, &response, &respMsgLen);
    if (rc)
    {
        error("Error while sending mctp request for ComponentUpdate."
              " EID={EID}, ComponentIndex={COMPONENTINDEX}",
              "EID", eid, "COMPONENTINDEX", componentIndex);

        if (rc == PLDM_REQUESTER_MCTP_TRANSPORT_ERROR)
        {
            handleTransportError(updateManager->handler, eid,
                                 "UpdateComponent");
        }
        else
        {
            bool logged = queryDeviceStatusAndLog(eid);
            if (!logged)
            {
                if (rc == PLDM_ERROR_NOT_READY)
                {
                    updateManager->createMessageRegistry(
                        eid, fwDeviceIDRecord, componentIndex, transferFailed,
                        "", PLDM_UPDATE_COMPONENT, COMMAND_TIMEOUT);
                }
            }
        }
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event,
            std::bind(&ComponentUpdater::updateComponentComplete, this,
                      ComponentUpdateStatus::UpdateFailed));
        co_return rc;
    }
    rc = co_await processUpdateComponentResponse(eid, response, respMsgLen,
                                                 retryCount);
    if (rc == PLDM_ERROR_INVALID_DATA)
    {
        if (retryCount < maxDecodeFailureRetries)
        {
            warning(
                "Decode failure for UpdateComponent, retry {RETRY} of {MAX}, EID={EID}",
                "RETRY", retryCount + 1, "MAX", maxDecodeFailureRetries, "EID",
                eid);
            co_return co_await sendUpdateComponentRequest(offset,
                                                          retryCount + 1);
        }
    }
    if (rc)
    {
        error("Error while processing component update response."
              " EID={EID}, ComponentIndex={COMPONENTINDEX}",
              "EID", eid, "COMPONENTINDEX", componentIndex);
    }
    co_return rc;
}

exec::task<int> ComponentUpdater::processUpdateComponentResponse(
    mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen,
    uint8_t retryCount)
{
    printBuffer(pldm::utils::Rx, response, respMsgLen,
                ("Received Response for UpdateComponent from EID=" +
                 std::to_string(eid) +
                 " ,ComponentIndex=" + std::to_string(componentIndex)));

    uint8_t completionCode = 0;
    uint8_t compCompatibilityResp = 0;
    uint8_t compCompatibilityRespCode = 0;
    bitfield32_t updateOptionFlagsEnabled{};
    uint16_t timeBeforeReqFWData = 0;

    auto rc = decode_update_component_resp(
        response, respMsgLen, &completionCode, &compCompatibilityResp,
        &compCompatibilityRespCode, &updateOptionFlagsEnabled,
        &timeBeforeReqFWData);
    if (rc)
    {
        if (retryCount >= maxDecodeFailureRetries)
        {
            auto [messageStatus, oemMessageId, oemMessageError, oemResolution] =
                getOemMessage(PLDM_UPDATE_COMPONENT, PLDM_ERROR);
            if (messageStatus)
            {
                updateManager->createMessageRegistryResourceErrors(
                    eid, fwDeviceIDRecord, componentIndex, oemMessageId,
                    oemMessageError, oemResolution);
            }
            error(
                "Failed to decode update request response for endpoint ID '{EID}', response code '{RC}'",
                "EID", eid, "RC", rc);
            componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
            pldmRequest = std::make_unique<sdeventplus::source::Defer>(
                updateManager->event,
                std::bind(&ComponentUpdater::updateComponentComplete, this,
                          ComponentUpdateStatus::UpdateFailed));
        }
        co_return PLDM_ERROR_INVALID_DATA;
    }
    if (completionCode)
    {
        error(
            "UpdateComponent response failed with error completion code,"
            " EID={EID}, CC={CC}, compCompatibilityResp={CCR}, compCompatibilityRespCode= {CCRC}",
            "EID", eid, "CC", completionCode, "CCR", compCompatibilityResp,
            "CCRC", compCompatibilityRespCode);
        updateManager->createMessageRegistry(
            eid, fwDeviceIDRecord, componentIndex, transferFailed, "",
            PLDM_UPDATE_COMPONENT, completionCode);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event,
            std::bind(&ComponentUpdater::updateComponentComplete, this,
                      ComponentUpdateStatus::UpdateFailed));
        co_return PLDM_ERROR;
    }
    if (compCompatibilityResp)
    {
        error(
            "In UpdateComponent response, ComponentCompatibilityResponse is non-zero EID={EID}, RC= {RC}, CompletionCode= {CC}, compCompatibilityResp={CCR}, compCompatibilityRespCode= {CCRC}",
            "EID", eid, "RC", rc, "CC", completionCode, "CCR",
            compCompatibilityResp, "CCRC", compCompatibilityRespCode);

        auto [messageStatus, oemMessageId, oemMessageError, oemResolution] =
            getCompCompatibilityMessage(PLDM_UPDATE_COMPONENT,
                                        compCompatibilityRespCode);
        if (messageStatus)
        {
            updateManager->createMessageRegistryResourceErrors(
                eid, fwDeviceIDRecord, componentIndex, oemMessageId,
                oemMessageError, oemResolution);
        }
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        if (compCompatibilityRespCode ==
            PLDM_CCRC_COMP_COMPARISON_STAMP_IDENTICAL)
        {
            pldmRequest = std::make_unique<sdeventplus::source::Defer>(
                updateManager->event,
                std::bind(&ComponentUpdater::updateComponentComplete, this,
                          ComponentUpdateStatus::UpdateSkipped));
        }
        // Set updateComponentComplete to UpdateFailed when
        // compCompatibilityRespCode is either
        // PLDM_CCRC_COMP_COMPARISON_STAMP_LOWER or any value other than
        // PLDM_CCRC_COMP_COMPARISON_STAMP_IDENTICAL and
        // PLDM_CCRC_NO_RESPONSE_CODE
        else
        {
            pldmRequest = std::make_unique<sdeventplus::source::Defer>(
                updateManager->event,
                std::bind(&ComponentUpdater::updateComponentComplete, this,
                          ComponentUpdateStatus::UpdateFailed));
        }
        co_return PLDM_ERROR;
    }

    componentUpdaterState.nextState(componentUpdaterState.current);

    updateManager->createMessageRegistry(eid, fwDeviceIDRecord, componentIndex,
                                         transferringToComponent);

    // Start timer waiting for first RequestFirmwareData from FD
    createRequestFwDataTimer();
    reqFwDataTimer->start(std::chrono::seconds(updateTimeoutSeconds), false);

    co_return PLDM_SUCCESS;
}

Response ComponentUpdater::requestFwData(const pldm_msg* request,
                                         size_t payloadLength)
{
    uint8_t completionCode = PLDM_SUCCESS;
    uint32_t offset = 0;
    uint32_t length = 0;
    Response response(sizeof(pldm_msg_hdr) + sizeof(completionCode), 0);
    auto responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    auto rc = decode_request_firmware_data_req(request, payloadLength, &offset,
                                               &length);
    if (rc)
    {
        /* Since the UA does not dictate the retries for the FD, the UA could
         send
         * multiple invalid/corrupted requests thereby flooding the task log.

            auto [messageStatus, oemMessageId, oemMessageError, oemResolution] =
                getOemMessage(PLDM_REQUEST_FIRMWARE_DATA, PLDM_ERROR);
            if (messageStatus)
            {
                updateManager->createMessageRegistryResourceErrors(
                    eid, fwDeviceIDRecord, componentIndex, oemMessageId,
                    oemMessageError, oemResolution);
            }

        */
        error(
            "Failed to decode request firmware data request for endpoint ID '{EID}', response code '{RC}'",
            "EID", eid, "RC", rc);
        rc = encode_request_firmware_data_resp(
            request->hdr.instance_id, PLDM_ERROR_INVALID_DATA, responseMsg,
            sizeof(completionCode));
        if (rc)
        {
            error(
                "Failed to encode request firmware date response for endpoint ID '{EID}', response code '{RC}'",
                "EID", eid, "RC", rc);
        }
        return response;
    }

    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
    const auto& comp = compImageInfos[applicableComponents[componentIndex]];
    auto compOffset = std::get<5>(comp);
    auto compSize = std::get<6>(comp);
    if (updateManager->fwDebug)
    {
        info("EID={EID}, ComponentIndex={COMPONENTINDEX}, Offset="
             "{OFFSET}, Length={LENGTH}",
             "EID", eid, "COMPONENTINDEX", componentIndex, "OFFSET", offset,
             "LENGTH", length);
    }

    auto expectedResult = componentUpdaterState.expectedState(
        ComponentUpdaterSequence::RequestFirmwareData);

    if (expectedResult == ComponentUpdaterSequence::Invalid)
    {
        if (componentUpdaterState.current ==
            ComponentUpdaterSequence::UpdateComponent)
        {
            error(
                "RequestFirmwareData received while UA still in UpdateComponent state. "
                "UA and FD are out of sync (UpdateComponent response likely lost). "
                "Responding with command not expected. EID={EID}, ComponentIndex={COMPONENTINDEX}",
                "EID", eid, "COMPONENTINDEX", componentIndex);
        }

        return sendCommandNotExpectedResponse(request, payloadLength);
    }
    else if (expectedResult == ComponentUpdaterSequence::RetryRequest)
    {
        info("Retry request for RequestFirmwareData. EID={EID}, "
             "ComponentIndex={COMPONENTINDEX}",
             "EID", eid, "COMPONENTINDEX", componentIndex);
    }

    if (length < PLDM_FWUP_BASELINE_TRANSFER_SIZE || length > maxTransferSize)
    {
        error("RequestFirmwareData reported PLDM_FWUP_INVALID_TRANSFER_LENGTH, "
              "EID={EID}, offset={OFFSET}, length={LENGTH}",
              "EID", eid, "OFFSET", offset, "LENGTH", length);
        rc = encode_request_firmware_data_resp(
            request->hdr.instance_id, PLDM_FWUP_INVALID_TRANSFER_LENGTH,
            responseMsg, sizeof(completionCode));
        if (rc)
        {
            error(
                "Failed to encode request firmware date response for endpoint ID '{EID}', response code '{RC}'",
                "EID", eid, "RC", rc);
        }
        return response;
    }

    if (offset + length > compSize + PLDM_FWUP_BASELINE_TRANSFER_SIZE)
    {
        error("RequestFirmwareData reported PLDM_FWUP_DATA_OUT_OF_RANGE, "
              "EID={EID}, offset={OFFSET}, length={LENGTH}",
              "EID", eid, "OFFSET", offset, "LENGTH", length);
        rc = encode_request_firmware_data_resp(
            request->hdr.instance_id, PLDM_FWUP_DATA_OUT_OF_RANGE, responseMsg,
            sizeof(completionCode));
        if (rc)
        {
            error(
                "Failed to encode request firmware date response for endpoint ID '{EID}', response code '{RC}'",
                "EID", eid, "RC", rc);
        }
        return response;
    }
    else if (offset + length >= compSize)
    {
        info("Last chunk of firmware data sent for EID={EID}, "
             "ComponentIndex={COMPONENTINDEX}. Starting UA_T6 timer.",
             "EID", eid, "COMPONENTINDEX", componentIndex);

        if (reqFwDataTimer)
        {
            reqFwDataTimer->stop();
            reqFwDataTimer.reset();
        }

        createCompleteCommandsTimeoutTimer();

        if (completeCommandsTimeoutTimer)
        {
            completeCommandsTimeoutTimer->start(
                std::chrono::seconds(completeCommandsTimeoutSeconds), false);
        }
    }

    handleLogging(offset, length);

    size_t padBytes = 0;
    if (offset + length > compSize)
    {
        padBytes = offset + length - compSize;
    }

    response.resize(sizeof(pldm_msg_hdr) + sizeof(completionCode) + length);
    responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    package.seekg(compOffset + offset);
    package.read(
        reinterpret_cast<char*>(
            response.data() + sizeof(pldm_msg_hdr) + sizeof(completionCode)),
        length - padBytes);
    rc = encode_request_firmware_data_resp(
        request->hdr.instance_id, completionCode, responseMsg,
        sizeof(completionCode));
    if (rc)
    {
        error(
            "Encoding RequestFirmwareData response failed, EID={EID}, RC={RC}",
            "EID", eid, "RC", rc);
        return response;
    }

    // Only restart reqFwDataTimer if we haven't transitioned to the
    // complete commands phase
    if (!completeCommandsTimeoutTimer)
    {
        if (!reqFwDataTimer)
        {
            // Timer should have been created in processUpdateComponentResponse
            // If it doesn't exist, something went wrong
            error(
                "Endpoint ID '{EID}' in unexpected state: missing RequestFirmwareData timer.",
                "EID", eid);
            componentUpdaterState.set(
                ComponentUpdaterSequence::CancelUpdateComponent);
            stdexec::start_detached(
                sendcancelUpdateComponentRequest(),
                exec::default_task_context<void>(exec::inline_scheduler{}));
            return sendCommandNotExpectedResponse(request, payloadLength);
        }

        reqFwDataTimer->start(std::chrono::seconds(updateTimeoutSeconds),
                              false);
    }

    return response;
}

Response ComponentUpdater::transferComplete(const pldm_msg* request,
                                            size_t payloadLength)
{
    uint8_t completionCode = PLDM_SUCCESS;
    Response response(sizeof(pldm_msg_hdr) + sizeof(completionCode), 0);
    auto responseMsg = reinterpret_cast<pldm_msg*>(response.data());

    printBuffer(pldm::utils::Rx, request, payloadLength,
                ("Received transferComplete from EID=" + std::to_string(eid) +
                 ", ComponentIndex=" + std::to_string(componentIndex)));

    uint8_t transferResult = 0;
    auto rc =
        decode_transfer_complete_req(request, payloadLength, &transferResult);
    if (rc)
    {
        auto [messageStatus, oemMessageId, oemMessageError, oemResolution] =
            getOemMessage(PLDM_TRANSFER_COMPLETE, PLDM_ERROR);
        if (messageStatus)
        {
            updateManager->createMessageRegistryResourceErrors(
                eid, fwDeviceIDRecord, componentIndex, oemMessageId,
                oemMessageError, oemResolution);
        }
        error(
            "Failed to decode TransferComplete request for endpoint ID '{EID}', response code '{RC}'",
            "EID", eid, "RC", rc);
        rc = encode_transfer_complete_resp(request->hdr.instance_id,
                                           PLDM_ERROR_INVALID_DATA, responseMsg,
                                           sizeof(completionCode));
        if (rc)
        {
            error(
                "Failed to encode TransferComplete response for endpoint ID '{EID}', response code '{RC}'",
                "EID", eid, "RC", rc);
        }
        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event, [this](EventBase&) {
                completeFailedStatusHandler(transferFailed,
                                            PLDM_TRANSFER_COMPLETE,
                                            PLDM_ERROR_INVALID_DATA);
            });
        return response;
    }

    if (componentUpdaterState.expectedState(
            ComponentUpdaterSequence::TransferComplete) ==
        ComponentUpdaterSequence::Invalid)
    {
        // A rejected command must not arm the complete-commands timer; no
        // accepted handler would ever stop it.
        return sendCommandNotExpectedResponse(request, payloadLength);
    }

    if (!completeCommandsTimeoutTimer)
    {
        error(
            "Received Transfer complete request from endpoint ID '{EID}' before all the data has been transferred",
            "EID", eid);

        if (reqFwDataTimer)
        {
            reqFwDataTimer->stop();
            reqFwDataTimer.reset();
        }

        createCompleteCommandsTimeoutTimer();

        if (completeCommandsTimeoutTimer)
        {
            completeCommandsTimeoutTimer->start(
                std::chrono::seconds(completeCommandsTimeoutSeconds), false);
        }
    }
    if (componentUpdaterState.expectedState(
            ComponentUpdaterSequence::TransferComplete) ==
        ComponentUpdaterSequence::RetryRequest)
    {
        error("Retry request for Transfer complete, EID={EID}, "
              "ComponentIndex={COMPONENTINDEX}",
              "EID", eid, "COMPONENTINDEX", componentIndex);
        rc = encode_transfer_complete_resp(request->hdr.instance_id,
                                           completionCode, responseMsg,
                                           sizeof(completionCode));
        if (rc)
        {
            error(
                "Encoding TransferComplete response failed, EID={EID}, RC={RC}",
                "EID", eid, "RC", rc);
        }
        return response;
    }

    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
    const auto& comp = compImageInfos[applicableComponents[componentIndex]];
    const auto& compVersion = std::get<7>(comp);

    // Print final average inter-request time for this component
    if (interRequestSamplesGlobal > 0)
    {
        double avgMs =
            static_cast<double>(totalInterRequestTimeGlobal.count()) /
            static_cast<double>(interRequestSamplesGlobal);
        info(
            "FinalAvgInterReqMs={AVGMS}, Samples={SAMPLES}, EID={EID}, ComponentIndex={COMPONENTINDEX}",
            "AVGMS", avgMs, "SAMPLES", interRequestSamplesGlobal, "EID", eid,
            "COMPONENTINDEX", componentIndex);
    }

    if (transferResult == PLDM_FWUP_TRANSFER_SUCCESS)
    {
        info(
            "Component endpoint ID '{EID}' and version '{COMPONENT_VERSION}' transfer complete.",
            "EID", eid, "COMPONENT_VERSION", compVersion);
        componentUpdaterState.nextState(componentUpdaterState.current);
    }
    else
    {
        componentUpdaterState.nextState(componentUpdaterState.current);
        // verify the status once by sending GetStatus before failing the update
        error(
            "Failure in transfer of the component endpoint ID '{EID}' and version '{COMPONENT_VERSION}' with transfer result - {RESULT}",
            "EID", eid, "COMPONENT_VERSION", compVersion, "RESULT",
            transferResult);
        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event, [this, transferResult](EventBase&) {
                completeFailedStatusHandler(
                    transferFailed, PLDM_TRANSFER_COMPLETE, transferResult);
            });
    }

    rc = encode_transfer_complete_resp(request->hdr.instance_id, completionCode,
                                       responseMsg, sizeof(completionCode));
    if (rc)
    {
        error(
            "Failed to encode transfer complete response of endpoint ID '{EID}', response code '{RC}'",
            "EID", eid, "RC", rc);
        return response;
    }
    return response;
}

Response ComponentUpdater::verifyComplete(const pldm_msg* request,
                                          size_t payloadLength)
{
    uint8_t completionCode = PLDM_SUCCESS;
    Response response(sizeof(pldm_msg_hdr) + sizeof(completionCode), 0);
    auto responseMsg = reinterpret_cast<pldm_msg*>(response.data());

    printBuffer(pldm::utils::Rx, request, payloadLength,
                ("Received verifyComplete from EID=" + std::to_string(eid) +
                 ", ComponentIndex=" + std::to_string(componentIndex)));

    uint8_t verifyResult = 0;
    auto rc = decode_verify_complete_req(request, payloadLength, &verifyResult);
    if (rc)
    {
        auto [messageStatus, oemMessageId, oemMessageError,
              oemResolution] = getOemMessage(PLDM_VERIFY_COMPLETE, PLDM_ERROR);
        if (messageStatus)
        {
            updateManager->createMessageRegistryResourceErrors(
                eid, fwDeviceIDRecord, componentIndex, oemMessageId,
                oemMessageError, oemResolution);
        }
        error(
            "Failed to decode verify complete request of endpoint ID '{EID}', response code '{RC}'",
            "EID", eid, "RC", rc);
        rc = encode_verify_complete_resp(request->hdr.instance_id,
                                         PLDM_ERROR_INVALID_DATA, responseMsg,
                                         sizeof(completionCode));
        if (rc)
        {
            error(
                "Failed to encode verify complete response of endpoint ID '{EID}', response code '{RC}'.",
                "EID", eid, "RC", rc);
        }
        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event, [this](EventBase&) {
                completeFailedStatusHandler(verificationFailed,
                                            PLDM_VERIFY_COMPLETE,
                                            PLDM_ERROR_INVALID_DATA);
            });
        return response;
    }

    if (componentUpdaterState.expectedState(
            ComponentUpdaterSequence::VerifyComplete) ==
        ComponentUpdaterSequence::Invalid)
    {
        return sendCommandNotExpectedResponse(request, payloadLength);
    }
    if (componentUpdaterState.expectedState(
            ComponentUpdaterSequence::VerifyComplete) ==
        ComponentUpdaterSequence::RetryRequest)
    {
        error("Retry request for Verify complete, EID={EID}, "
              "ComponentIndex={COMPONENTINDEX}",
              "EID", eid, "COMPONENTINDEX", componentIndex);
        rc = encode_verify_complete_resp(request->hdr.instance_id,
                                         completionCode, responseMsg,
                                         sizeof(completionCode));
        if (rc)
        {
            error("Encoding VerifyComplete response failed, EID={EID}, RC={RC}",
                  "EID", eid, "RC", rc);
        }
        return response;
    }

    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
    const auto& comp = compImageInfos[applicableComponents[componentIndex]];
    const auto& compVersion = std::get<7>(comp);

    if (verifyResult == PLDM_FWUP_VERIFY_SUCCESS)
    {
        info(
            "Component endpoint ID '{EID}' and version '{COMPONENT_VERSION}' verification complete.",
            "EID", eid, "COMPONENT_VERSION", compVersion);
        componentUpdaterState.nextState(componentUpdaterState.current);
    }
    else
    {
        error(
            "Failed to verify component endpoint ID '{EID}' and version '{COMPONENT_VERSION}' with verify result - {RESULT}",
            "EID", eid, "COMPONENT_VERSION", compVersion, "RESULT",
            verifyResult);
        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event, [this, verifyResult](EventBase&) {
                completeFailedStatusHandler(verificationFailed,
                                            PLDM_VERIFY_COMPLETE, verifyResult);
            });
    }

    rc = encode_verify_complete_resp(request->hdr.instance_id, completionCode,
                                     responseMsg, sizeof(completionCode));
    if (rc)
    {
        error(
            "Failed to encode verify complete response for endpoint ID '{EID}', response code - {RC}",
            "EID", eid, "RC", rc);
        return response;
    }
    return response;
}

void ComponentUpdater::completeFailedStatusHandler(
    const std::string& messageId, pldm_firmware_update_commands command,
    uint8_t result)
{
    if (deviceUpdater != nullptr &&
        deviceUpdater->isTimeoutCancellationRequested())
    {
        return;
    }

    updateManager->createMessageRegistry(eid, fwDeviceIDRecord, componentIndex,
                                         messageId, "", command, result);
    componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
    if (completeCommandsTimeoutTimer)
    {
        completeCommandsTimeoutTimer->stop();
        completeCommandsTimeoutTimer.reset();
    }
    if (discoverMctpTerminusTaskHandle.has_value())
    {
        auto& [scope, rcOpt] = *discoverMctpTerminusTaskHandle;
        if (!rcOpt.has_value())
        {
            return;
        }
        stdexec::sync_wait(scope.on_empty());
        discoverMctpTerminusTaskHandle.reset();
    }
    auto& [scope, rcOpt] = discoverMctpTerminusTaskHandle.emplace();
    stdexec::start_detached(
        sendcancelUpdateComponentRequest() |
            stdexec::then([&](int rc) { rcOpt.emplace(rc); }),
        exec::default_task_context<void>(exec::inline_scheduler{}));
}

void ComponentUpdater::applyCompleteSucceededStatusHandler(
    const std::string& compVersion, bitfield16_t compActivationModification)
{
    if (deviceUpdater == nullptr ||
        deviceUpdater->isTimeoutCancellationRequested())
    {
        return;
    }

    logComponentUpdateDuration();

    deviceUpdater->accumulateActivationModifications(
        compActivationModification);

    updateManager->createMessageRegistry(eid, fwDeviceIDRecord, componentIndex,
                                         updateSuccessful);
    info(
        "Component endpoint ID '{EID}' with '{COMPONENT_VERSION}' apply complete.",
        "EID", eid, "COMPONENT_VERSION", compVersion);
    // Restore the deferred event
    pldmRequest = std::make_unique<sdeventplus::source::Defer>(
        updateManager->event,
        std::bind(&ComponentUpdater::updateComponentComplete, this,
                  ComponentUpdateStatus::UpdateComplete));

    if (completeCommandsTimeoutTimer)
    {
        completeCommandsTimeoutTimer->stop();
        completeCommandsTimeoutTimer.reset();
    }
}

Response ComponentUpdater::applyComplete(const pldm_msg* request,
                                         size_t payloadLength)
{
    pldmRequest.reset();
    uint8_t completionCode = PLDM_SUCCESS;
    Response response(sizeof(pldm_msg_hdr) + sizeof(completionCode), 0);
    auto responseMsg = reinterpret_cast<pldm_msg*>(response.data());

    printBuffer(pldm::utils::Rx, request, payloadLength,
                ("Received applyComplete from EID=" + std::to_string(eid) +
                 ", ComponentIndex=" + std::to_string(componentIndex)));

    uint8_t applyResult = 0;
    bitfield16_t compActivationModification{};

    auto rc = decode_apply_complete_req(request, payloadLength, &applyResult,
                                        &compActivationModification);
    if (rc)
    {
        auto [messageStatus, oemMessageId, oemMessageError,
              oemResolution] = getOemMessage(PLDM_APPLY_COMPLETE, PLDM_ERROR);
        if (messageStatus)
        {
            updateManager->createMessageRegistryResourceErrors(
                eid, fwDeviceIDRecord, componentIndex, oemMessageId,
                oemMessageError, oemResolution);
        }
        error(
            "Failed to decode apply complete request for endpoint ID '{EID}', response code '{RC}'",
            "EID", eid, "RC", rc);
        rc = encode_apply_complete_resp(request->hdr.instance_id,
                                        PLDM_ERROR_INVALID_DATA, responseMsg,
                                        sizeof(completionCode));
        if (rc)
        {
            error(
                "Failed to encode apply complete response for endpoint ID '{EID}', response code '{RC}'",
                "EID", eid, "RC", rc);
        }
        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event, [this](EventBase&) {
                completeFailedStatusHandler(applyFailed, PLDM_APPLY_COMPLETE,
                                            PLDM_ERROR_INVALID_DATA);
            });
        return response;
    }

    if (componentUpdaterState.expectedState(
            ComponentUpdaterSequence::ApplyComplete) ==
        ComponentUpdaterSequence::Invalid)
    {
        return sendCommandNotExpectedResponse(request, payloadLength);
    }
    if (componentUpdaterState.expectedState(
            ComponentUpdaterSequence::ApplyComplete) ==
        ComponentUpdaterSequence::RetryRequest)
    {
        error("Retry request for apply complete, EID={EID}, "
              "ComponentIndex={COMPONENTINDEX}",
              "EID", eid, "COMPONENTINDEX", componentIndex);
        rc =
            encode_apply_complete_resp(request->hdr.instance_id, completionCode,
                                       responseMsg, sizeof(completionCode));
        if (rc)
        {
            error("Encoding ApplyComplete response failed, EID={EID}, RC={RC}",
                  "EID", eid, "RC", rc);
        }
        return response;
    }

    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
    const auto& comp = compImageInfos[applicableComponents[componentIndex]];
    const auto& compVersion = std::get<7>(comp);

    if (applyResult == PLDM_FWUP_APPLY_SUCCESS ||
        applyResult == PLDM_FWUP_APPLY_SUCCESS_WITH_ACTIVATION_METHOD)
    {
        auto validateApplyStatusSuccess = [this, applyResult, compVersion,
                                           compActivationModification](
                                              uint8_t currentFDState) {
            if (currentFDState == PLDM_FD_STATE_READY_XFER)
            {
                applyCompleteSucceededStatusHandler(compVersion,
                                                    compActivationModification);
            }
            else
            {
                error(
                    "Failed to apply component endpoint ID '{EID}' and version '{COMPONENT_VERSION}', error - {ERROR}",
                    "EID", eid, "COMPONENT_VERSION", compVersion, "ERROR",
                    applyResult);
                completeFailedStatusHandler(applyFailed, PLDM_APPLY_COMPLETE,
                                            applyResult);
            }
        };

        pendingPostResponseAction = [this, validateApplyStatusSuccess]() {
            GetStatus(validateApplyStatusSuccess);
        };
    }
    else
    {
        // verify the status once by sending GetStatus before failing the update
        error(
            "Failed to apply component endpoint ID '{EID}' and version '{COMPONENT_VERSION}', error - {ERROR}",
            "EID", eid, "COMPONENT_VERSION", compVersion, "ERROR", applyResult);
        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event, [this, applyResult](EventBase&) {
                completeFailedStatusHandler(applyFailed, PLDM_APPLY_COMPLETE,
                                            applyResult);
            });
    }

    rc = encode_apply_complete_resp(request->hdr.instance_id, completionCode,
                                    responseMsg, sizeof(completionCode));
    if (rc)
    {
        error(
            "Failed to encode apply complete response for endpoint ID '{EID}', response code '{RC}'",
            "EID", eid, "RC", rc);
        return response;
    }
    return response;
}

void ComponentUpdater::onResponseSendComplete(bool success)
{
    if (!pendingPostResponseAction)
    {
        return;
    }

    if (success)
    {
        auto action = std::move(pendingPostResponseAction);
        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event,
            [action = std::move(action)](EventBase&) { action(); });
    }
    else
    {
        warning("Response send failed for EID={EID}, "
                "FD is expected to retry the command",
                "EID", eid);
        pendingPostResponseAction = nullptr;
    }
}

void ComponentUpdater::stopComponentUpdateTimers()
{
    pendingPostResponseAction = nullptr;
    pldmRequest.reset();

    if (reqFwDataTimer)
    {
        reqFwDataTimer->stop();
        reqFwDataTimer.reset();
    }

    if (completeCommandsTimeoutTimer)
    {
        completeCommandsTimeoutTimer->stop();
        completeCommandsTimeoutTimer.reset();
    }
}

void ComponentUpdater::createRequestFwDataTimer()
{
    reqFwDataTimer = std::make_unique<sdbusplus::Timer>([this]() -> void {
        if (deviceUpdater != nullptr &&
            deviceUpdater->isTimeoutCancellationRequested())
        {
            return;
        }

        error(
            "RequestFWData timed out. No command received from FD within the expected time of {EXPECTEDTIME}s. EID={EID}, "
            "ComponentIndex={COMPONENTINDEX}",
            "EID", eid, "COMPONENTINDEX", componentIndex, "EXPECTEDTIME",
            updateTimeoutSeconds);

        auto queryAndCancelTask = [this]() -> exec::task<void> {
            // Check for transport errors first
            handleTransportError(updateManager->handler, eid,
                                 "RequestFirmwareData");

            // Then check device status
            bool logged = queryDeviceStatusAndLog(eid);
            if (!logged)
            {
                updateManager->createMessageRegistry(
                    eid, fwDeviceIDRecord, componentIndex, transferFailed, "",
                    PLDM_REQUEST_FIRMWARE_DATA, PLDM_FWUP_TIME_OUT);
            }
            else
            {
                updateManager->createMessageRegistry(
                    eid, fwDeviceIDRecord, componentIndex, transferFailed);
            }

            componentUpdaterState.set(
                ComponentUpdaterSequence::CancelUpdateComponent);
            if (discoverMctpTerminusTaskHandle.has_value())
            {
                auto& [scope, rcOpt] = *discoverMctpTerminusTaskHandle;
                if (!rcOpt.has_value())
                {
                    co_return;
                }
                stdexec::sync_wait(scope.on_empty());
                discoverMctpTerminusTaskHandle.reset();
            }
            auto& [scope, rcOpt] = discoverMctpTerminusTaskHandle.emplace();
            stdexec::start_detached(
                sendcancelUpdateComponentRequest() |
                    stdexec::then([&](int rc) { rcOpt.emplace(rc); }),
                exec::default_task_context<void>(exec::inline_scheduler{}));
        };

        stdexec::start_detached(
            queryAndCancelTask(),
            exec::default_task_context<void>(exec::inline_scheduler{}));
    });
}

void ComponentUpdater::createCompleteCommandsTimeoutTimer()
{
    completeCommandsTimeoutTimer = std::make_unique<
        sdbusplus::Timer>([this]() -> void {
        if (deviceUpdater != nullptr &&
            deviceUpdater->isTimeoutCancellationRequested())
        {
            return;
        }

        pldm_firmware_update_commands timedOutCommand{};
        std::string commandName{};
        std::string stateFailedMessageId{};

        if (componentUpdaterState.current ==
                ComponentUpdaterSequence::TransferComplete or
            componentUpdaterState.current ==
                ComponentUpdaterSequence::RequestFirmwareData)
        {
            timedOutCommand = PLDM_TRANSFER_COMPLETE;
            commandName = "TransferComplete";
            stateFailedMessageId = transferFailed;
        }
        else if (componentUpdaterState.current ==
                 ComponentUpdaterSequence::VerifyComplete)
        {
            timedOutCommand = PLDM_VERIFY_COMPLETE;
            commandName = "VerifyComplete";
            stateFailedMessageId = verificationFailed;
        }
        else if (componentUpdaterState.current ==
                 ComponentUpdaterSequence::ApplyComplete)
        {
            timedOutCommand = PLDM_APPLY_COMPLETE;
            commandName = "ApplyComplete";
            stateFailedMessageId = applyFailed;
        }

        if (commandName.empty())
        {
            error(
                "Complete commands timeout fired in unexpected state {STATE}, "
                "ignoring stale timer. EID={EID}, ComponentIndex={COMPONENTINDEX}",
                "STATE", static_cast<int>(componentUpdaterState.current), "EID",
                eid, "COMPONENTINDEX", componentIndex);
            return;
        }

        error("{CMD} Command Timeout. EID={EID}, "
              "ComponentIndex={COMPONENTINDEX}",
              "CMD", commandName, "EID", eid, "COMPONENTINDEX", componentIndex);

        auto queryAndCancelTask = [this, timedOutCommand, commandName,
                                   stateFailedMessageId]() -> exec::task<void> {
            // Check for transport errors first
            handleTransportError(updateManager->handler, eid, commandName);

            // Then check device status
            bool logged = queryDeviceStatusAndLog(eid);
            if (logged)
            {
                updateManager->createMessageRegistry(
                    eid, fwDeviceIDRecord, componentIndex,
                    stateFailedMessageId);
            }
            else
            {
                updateManager->createMessageRegistry(
                    eid, fwDeviceIDRecord, componentIndex, stateFailedMessageId,
                    "", timedOutCommand, PLDM_FWUP_TIME_OUT);
            }

            componentUpdaterState.set(
                ComponentUpdaterSequence::CancelUpdateComponent);
            if (discoverMctpTerminusTaskHandle.has_value())
            {
                auto& [scope, rcOpt] = *discoverMctpTerminusTaskHandle;
                if (!rcOpt.has_value())
                {
                    co_return;
                }
                stdexec::sync_wait(scope.on_empty());
                discoverMctpTerminusTaskHandle.reset();
            }
            auto& [scope, rcOpt] = discoverMctpTerminusTaskHandle.emplace();
            stdexec::start_detached(
                sendcancelUpdateComponentRequest() |
                    stdexec::then([&](int rc) { rcOpt.emplace(rc); }),
                exec::default_task_context<void>(exec::inline_scheduler{}));
        };

        stdexec::start_detached(
            queryAndCancelTask(),
            exec::default_task_context<void>(exec::inline_scheduler{}));
    });
}

exec::task<int> ComponentUpdater::sendcancelUpdateComponentRequest()
{
    pldmRequest.reset();
    auto instanceId = updateManager->instanceIdDb.next(eid);
    Request request(sizeof(pldm_msg_hdr));
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    const pldm_msg* response = NULL;
    size_t respMsgLen = 0;

    auto rc = encode_cancel_update_component_req(
        instanceId, requestMsg, PLDM_CANCEL_UPDATE_COMPONENT_REQ_BYTES);
    if (rc)
    {
        updateManager->instanceIdDb.free(eid, instanceId);
        error("encode_cancel_update_component_req failed, EID={EID}, "
              "ComponentIndex={COMPONENTINDEX}, RC={RC}",
              "EID", eid, "COMPONENTINDEX", componentIndex, "RC", rc);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        updateComponentComplete(ComponentUpdateStatus::UpdateFailed);
        co_return PLDM_ERROR;
    }

    printBuffer(pldm::utils::Tx, request,
                ("Send CancelUpdateComponentRequest for EID=" +
                 std::to_string(eid)));

    rc = co_await sendRecvPldmMsgOverMctp(eid, request, &response, &respMsgLen);
    if (rc)
    {
        error("Error while sending mctp request for ComponentUpdate."
              " EID={EID}, ComponentIndex={COMPONENTINDEX}",
              "EID", eid, "COMPONENTINDEX", componentIndex);

        if (rc == PLDM_REQUESTER_MCTP_TRANSPORT_ERROR)
        {
            handleTransportError(updateManager->handler, eid,
                                 "CancelUpdateComponent");
        }
        else
        {
            [[maybe_unused]] bool logged = queryDeviceStatusAndLog(eid);
        }

        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        updateComponentComplete(ComponentUpdateStatus::UpdateFailed);
        co_return rc;
    }
    rc = co_await processCancelUpdateComponentResponse(eid, response,
                                                       respMsgLen);
    if (rc)
    {
        error("Error while processing cancel update response."
              " EID={EID}, ComponentIndex={COMPONENTINDEX}",
              "EID", eid, "COMPONENTINDEX", componentIndex);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
    }
    // update the status of update
    updateComponentComplete(ComponentUpdateStatus::UpdateFailed);
    co_return rc;
}

exec::task<int> ComponentUpdater::processCancelUpdateComponentResponse(
    mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen)
{
    printBuffer(pldm::utils::Rx, response, respMsgLen,
                ("Received CancelUpdateComponent Response from EID=" +
                 std::to_string(eid)));

    uint8_t completionCode = 0;
    auto rc = decode_cancel_update_component_resp(response, respMsgLen,
                                                  &completionCode);
    if (rc)
    {
        error("Decoding CancelUpdateComponent response failed, EID={EID}, "
              "ComponentIndex={COMPONENTINDEX}, CC={CC}",
              "EID", eid, "COMPONENTINDEX", componentIndex, "CC",
              completionCode);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        co_return rc;
    }
    if (completionCode)
    {
        error("CancelUpdateComponent response failed with error, EID={EID}, "
              "ComponentIndex={COMPONENTINDEX}, CC={CC}",
              "EID", eid, "COMPONENTINDEX", componentIndex, "CC",
              completionCode);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        co_return PLDM_ERROR;
    }
    co_return PLDM_SUCCESS;
}

void ComponentUpdater::updateComponentComplete(ComponentUpdateStatus status)
{
    if (deviceUpdater != nullptr &&
        deviceUpdater->isTimeoutCancellationRequested())
    {
        return;
    }

    if (updateCompletionCoHandle.has_value())
    {
        auto& [scope, rcOpt] = *updateCompletionCoHandle;
        if (!rcOpt.has_value())
        {
            return;
        }
        stdexec::sync_wait(scope.on_empty());
        updateCompletionCoHandle.reset();
    }
    auto& [scope, rcOpt] = updateCompletionCoHandle.emplace();
    stdexec::start_detached(
        deviceUpdater->updateComponentCompletion(componentIndex, status) |
            stdexec::then([&](int rc) { rcOpt.emplace(rc); }),
        exec::default_task_context<void>(exec::inline_scheduler{}));
}

void ComponentUpdater::GetStatus(std::function<void(uint8_t)> getStatusCallback)
{
    pldmRequest.reset();
    if (getStatusTaskHandle.has_value())
    {
        auto& [scope, rcOpt] = *getStatusTaskHandle;
        if (!rcOpt.has_value())
        {
            return;
        }
        getStatusTaskHandle.reset();
    }

    auto& [scope, rcOpt] = getStatusTaskHandle.emplace();
    stdexec::start_detached(
        sendGetStatusRequest(getStatusCallback) |
            stdexec::then([&](int rc) { rcOpt.emplace(rc); }),
        exec::default_task_context<void>(exec::inline_scheduler{}));
}

exec::task<int> ComponentUpdater::sendGetStatusRequest(
    std::function<void(uint8_t)> getStatusCallback, uint8_t retryCount)
{
    auto instanceId = updateManager->instanceIdDb.next(eid);
    Request request(sizeof(pldm_msg_hdr));
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    const pldm_msg* response = NULL;
    size_t respMsgLen = 0;

    auto rc = encode_get_status_req(instanceId, requestMsg,
                                    PLDM_GET_STATUS_REQ_BYTES);
    if (rc)
    {
        updateManager->instanceIdDb.free(eid, instanceId);
        error("encode_get_status_req failed, EID={EID}, "
              "ComponentIndex={COMPONENTINDEX}, RC={RC}",
              "EID", eid, "COMPONENTINDEX", componentIndex, "RC", rc);
        getStatusCallback(0);
        co_return rc;
    }
    printBuffer(pldm::utils::Tx, request,
                ("Send GetStatusRequest for EID=" + std::to_string(eid)));

    rc = co_await sendRecvPldmMsgOverMctp(eid, request, &response, &respMsgLen);
    if (rc)
    {
        error("Error while sending mctp request for ComponentUpdate."
              " EID={EID}, ComponentIndex={COMPONENTINDEX}",
              "EID", eid, "COMPONENTINDEX", componentIndex);

        if (rc == PLDM_REQUESTER_MCTP_TRANSPORT_ERROR)
        {
            handleTransportError(updateManager->handler, eid, "GetStatus");
        }
        else
        {
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
        }
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        getStatusCallback(0);
        co_return rc;
    }

    uint8_t currentFDState = 0;
    uint8_t progressPercent = 0x65;
    rc = co_await processGetStatusResponse(
        eid, response, respMsgLen, currentFDState, progressPercent, retryCount);
    if (rc == PLDM_ERROR_INVALID_DATA)
    {
        if (retryCount < maxDecodeFailureRetries)
        {
            warning(
                "Decode failure for GetStatus, retry {RETRY} of {MAX}, EID={EID}",
                "RETRY", retryCount + 1, "MAX", maxDecodeFailureRetries, "EID",
                eid);
            co_return co_await sendGetStatusRequest(getStatusCallback,
                                                    retryCount + 1);
        }
    }
    if (rc)
    {
        error("Error while processing get request response."
              " EID={EID}, ComponentIndex={COMPONENTINDEX}",
              "EID", eid, "COMPONENTINDEX", componentIndex);
    }

    getStatusCallback(currentFDState);
    co_return rc;
}

exec::task<int> ComponentUpdater::processGetStatusResponse(
    mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen,
    uint8_t& currentFDState, uint8_t& progressPercent, uint8_t retryCount)
{
    printBuffer(pldm::utils::Rx, response, respMsgLen,
                ("Received GetStatus Response from EID=" +
                 std::to_string(eid)));

    uint8_t completionCode = 0;
    uint8_t previousState = 0;
    uint8_t auxState = 0;
    uint8_t auxStateStatus = 0;
    uint8_t reasonCode = 0;
    bitfield32_t updateOptionFlagsEnabled{0};
    auto rc = decode_get_status_resp(
        response, respMsgLen, &completionCode, &currentFDState, &previousState,
        &auxState, &auxStateStatus, &progressPercent, &reasonCode,
        &updateOptionFlagsEnabled);
    if (rc)
    {
        if (retryCount >= maxDecodeFailureRetries)
        {
            auto [messageStatus, oemMessageId, oemMessageError,
                  oemResolution] = getOemMessage(PLDM_GET_STATUS, PLDM_ERROR);
            if (messageStatus)
            {
                updateManager->createMessageRegistryResourceErrors(
                    eid, fwDeviceIDRecord, componentIndex, oemMessageId,
                    oemMessageError, oemResolution);
            }
            error("Decoding GetStatus response failed, EID={EID}, "
                  "ComponentIndex={COMPONENTINDEX}, CC={CC}",
                  "EID", eid, "COMPONENTINDEX", componentIndex, "CC",
                  completionCode);
            componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        }
        co_return PLDM_ERROR_INVALID_DATA;
    }
    if (completionCode)
    {
        error("GetStatus response failed with error, EID={EID}, "
              "ComponentIndex={COMPONENTINDEX}, CC={CC}",
              "EID", eid, "COMPONENTINDEX", componentIndex, "CC",
              completionCode);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        co_return PLDM_ERROR;
    }
    co_return PLDM_SUCCESS;
}

void ComponentUpdater::handleLogging(uint32_t offset, uint32_t length)
{
    constexpr uint64_t oneMegaByte = 1024ULL * 1024ULL;

    if (offset == 0)
    {
        info(
            "Firmware Update started for component {COMPONENTINDEX} on endpoint id {EID} at offset {OFFSET} with length {LENGTH}",
            "COMPONENTINDEX", componentIndex, "EID", eid, "OFFSET", offset,
            "LENGTH", length);
    }

    auto now = std::chrono::steady_clock::now();
    if (lastRequestTime != std::chrono::steady_clock::time_point{})
    {
        auto delta = (now - lastRequestTime);
        totalInterRequestTime +=
            std::chrono::duration_cast<std::chrono::milliseconds>(delta);
        interRequestSamples++;
        totalInterRequestTimeGlobal +=
            std::chrono::duration_cast<std::chrono::milliseconds>(delta);
        interRequestSamplesGlobal++;
    }
    lastRequestTime = now;

    if (offset == lastOffsetRequested)
    {
        info(
            "Retry EID={EID}, ComponentIndex={COMPONENTINDEX}, Offset={OFFSET} Length={LENGTH}",
            "EID", eid, "COMPONENTINDEX", componentIndex, "OFFSET", offset,
            "LENGTH", length);
    }
    else
    {
        if (offset > nextExpectedOffset)
        {
            info(
                "OutOfOrderRequest EID={EID}, ComponentIndex={COMPONENTINDEX}, RequestedOffset={REQUESTED}, Length={LENGTH}, SkippedStart={SKIPSTART}, SkippedEnd={SKIPEND}",
                "EID", eid, "COMPONENTINDEX", componentIndex, "REQUESTED",
                offset, "LENGTH", length, "SKIPSTART", nextExpectedOffset,
                "SKIPEND", offset - 1);
        }

        if (offset < nextExpectedOffset)
        {
            info(
                "OutOfOrderRequest EID={EID}, ComponentIndex={COMPONENTINDEX}, RequestedOffset={REQUESTED}, Length={LENGTH}, ExpectedOffset={EXPECTED}",
                "EID", eid, "COMPONENTINDEX", componentIndex, "REQUESTED",
                offset, "LENGTH", length, "EXPECTED", nextExpectedOffset);
        }
        lastOffsetRequested = offset;
    }
    nextExpectedOffset = offset + length;

    if (interRequestSamples > 0 && (offset % oneMegaByte) == 0)
    {
        uint64_t mbIndex = static_cast<uint64_t>(offset) / oneMegaByte;
        if (mbIndex != lastAvgPrintedMBIndex)
        {
            double avgMs = static_cast<double>(totalInterRequestTime.count()) /
                           static_cast<double>(interRequestSamples);
            info(
                "AvgInterReqMs={AVGMS}, Samples={SAMPLES}, EID={EID}, ComponentIndex={COMPONENTINDEX}, Offset={OFFSET}",
                "AVGMS", avgMs, "SAMPLES", interRequestSamples, "EID", eid,
                "COMPONENTINDEX", componentIndex, "OFFSET", offset);
            lastAvgPrintedMBIndex = mbIndex;
        }
        interRequestSamples = 0;
        totalInterRequestTime = std::chrono::milliseconds{0};
    }
}

void ComponentUpdater::logComponentUpdateDuration()
{
    if (componentUpdateStartTime == std::chrono::steady_clock::time_point{})
    {
        return;
    }

    auto endTime = std::chrono::steady_clock::now();
    auto durationSec =
        std::chrono::duration<double>(endTime - componentUpdateStartTime)
            .count();

    std::string compName =
        updateManager->getComponentName(eid, fwDeviceIDRecord, componentIndex);

    std::string durationStr = std::format("{:.0f} s", durationSec);

    info(
        "Component update time: {UPDATE_TIME}, EID={EID}, ComponentIndex={COMPONENTINDEX}, ComponentName={COMPNAME}",
        "UPDATE_TIME", durationStr, "EID", eid, "COMPONENTINDEX",
        componentIndex, "COMPNAME", compName);

    createLogEntry(componentUpdateTime, compName, durationStr, "");
}

} // namespace fw_update

} // namespace pldm
