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
#include "update_manager.hpp"

#include <phosphor-logging/lg2.hpp>

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
    auto rc = co_await sendUpdateComponentRequest(componentIndex);
    if (rc)
    {
        error("Error while sending component update request."
              " EID={EID}, ComponentIndex={COMPONENTINDEX}",
              "EID", eid, "COMPONENTINDEX", componentIndex);
    }
    co_return rc;
}

exec::task<int> ComponentUpdater::sendUpdateComponentRequest(size_t offset)
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
                 " ,ComponentIndex=" + std::to_string(componentIndex)),
                updateManager->fwDebug);
    rc = co_await sendRecvPldmMsgOverMctp(eid, request, &response, &respMsgLen);
    if (rc)
    {
        updateManager->createMessageRegistry(
            eid, fwDeviceIDRecord, componentIndex, transferFailed, "",
            PLDM_UPDATE_COMPONENT, COMMAND_TIMEOUT);
        error("Error while sending mctp request for ComponentUpdate."
              " EID={EID}, ComponentIndex={COMPONENTINDEX}",
              "EID", eid, "COMPONENTINDEX", componentIndex);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event,
            std::bind(&ComponentUpdater::updateComponentComplete, this,
                      ComponentUpdateStatus::UpdateFailed));
        co_return rc;
    }
    rc = processUpdateComponentResponse(eid, response, respMsgLen);
    if (rc)
    {
        error("Error while processing component update response."
              " EID={EID}, ComponentIndex={COMPONENTINDEX}",
              "EID", eid, "COMPONENTINDEX", componentIndex);
        co_return rc;
    }
    co_return rc;
}

int ComponentUpdater::processUpdateComponentResponse(
    mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen)
{
    if (response == nullptr || !respMsgLen)
    {
        updateManager->createMessageRegistry(
            eid, fwDeviceIDRecord, componentIndex, transferFailed, "",
            PLDM_UPDATE_COMPONENT, PLDM_FWUP_TIME_OUT);
        error(
            "No response received for update component with endpoint ID {EID}",
            "EID", eid);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event,
            std::bind(&ComponentUpdater::updateComponentComplete, this,
                      ComponentUpdateStatus::UpdateFailed));
        return PLDM_ERROR;
    }

    printBuffer(pldm::utils::Rx, response, respMsgLen,
                ("Received Response for UpdateComponent from EID=" +
                 std::to_string(eid) +
                 " ,ComponentIndex=" + std::to_string(componentIndex)),
                updateManager->fwDebug);

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
        error(
            "Failed to decode update request response for endpoint ID '{EID}', response code '{RC}'",
            "EID", eid, "RC", rc);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        return rc;
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
        return PLDM_ERROR;
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
        return PLDM_ERROR;
    }

    componentUpdaterState.nextState(componentUpdaterState.current);

    updateManager->createMessageRegistry(eid, fwDeviceIDRecord, componentIndex,
                                         transferringToComponent);
    return PLDM_SUCCESS;
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
        error(
            "Failed to decode request firmware date request for endpoint ID '{EID}', response code '{RC}'",
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

    if (componentUpdaterState.expectedState(
            ComponentUpdaterSequence::RequestFirmwareData) ==
        ComponentUpdaterSequence::Invalid)
    {
        return sendCommandNotExpectedResponse(request, payloadLength);
    }
    if (componentUpdaterState.expectedState(
            ComponentUpdaterSequence::RequestFirmwareData) ==
        ComponentUpdaterSequence::RetryRequest)
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
    if (!reqFwDataTimer)
    {
        if (offset != 0)
        {
            warning("First data request is not at offset 0");
        }

        // create timer for first request
        createRequestFwDataTimer();
    }

    if (reqFwDataTimer)
    {
        reqFwDataTimer->start(std::chrono::seconds(updateTimeoutSeconds),
                              false);
    }
    else
    {
        error(
            "Failed to start timer for handling RequestFirmwareData, EID={EID}, RC={RC}",
            "EID", eid, "RC", rc);
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
                 ", ComponentIndex=" + std::to_string(componentIndex)),
                updateManager->fwDebug);

    uint8_t transferResult = 0;
    auto rc =
        decode_transfer_complete_req(request, payloadLength, &transferResult);
    if (rc)
    {
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
        return response;
    }

    if (componentUpdaterState.expectedState(
            ComponentUpdaterSequence::TransferComplete) ==
        ComponentUpdaterSequence::Invalid)
    {
        return sendCommandNotExpectedResponse(request, payloadLength);
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
    if (reqFwDataTimer)
    {
        reqFwDataTimer->stop();
        reqFwDataTimer.reset();
    }
    // create and start UA_T6 timer
    info("Progress percent is not supported. Starting UA_T6 timer");
    createCompleteCommandsTimeoutTimer();
    completeCommandsTimeoutTimer->start(
        std::chrono::seconds(completeCommandsTimeoutSeconds), false);

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
        if (updateManager->fwDebug)
        {
            info(
                "Component endpoint ID '{EID}' and version '{COMPONENT_VERSION}' transfer complete.",
                "EID", eid, "COMPONENT_VERSION", compVersion);
        }
        componentUpdaterState.nextState(componentUpdaterState.current);
    }
    else
    {
        componentUpdaterState.nextState(componentUpdaterState.current);
        // verify the status once by sending GetStatus before failing the update
        auto transferFailedStatusHandler = [this, transferResult,
                                            compVersion]() {
            // TransferComplete Failed
            updateManager->createMessageRegistry(
                eid, fwDeviceIDRecord, componentIndex, transferFailed, "",
                PLDM_TRANSFER_COMPLETE, transferResult);
            error(
                "Failure in transfer of the component endpoint ID '{EID}' and version '{COMPONENT_VERSION}' with transfer result - {RESULT}",
                "EID", eid, "COMPONENT_VERSION", compVersion, "RESULT",
                transferResult);
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
        };
        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event,
            std::bind(&ComponentUpdater::handleComponentUpdateFailure, this,
                      transferFailedStatusHandler));
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
                 ", ComponentIndex=" + std::to_string(componentIndex)),
                updateManager->fwDebug);

    uint8_t verifyResult = 0;
    auto rc = decode_verify_complete_req(request, payloadLength, &verifyResult);
    if (rc)
    {
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
        if (updateManager->fwDebug)
        {
            info(
                "Component endpoint ID '{EID}' and version '{COMPONENT_VERSION}' verification complete.",
                "EID", eid, "COMPONENT_VERSION", compVersion);
        }
        componentUpdaterState.nextState(componentUpdaterState.current);
    }
    else
    {
        auto verifyFailedStatusHandler = [this, verifyResult, compVersion]() {
            // VerifyComplete Failed
            updateManager->createMessageRegistry(
                eid, fwDeviceIDRecord, componentIndex, verificationFailed, "",
                PLDM_VERIFY_COMPLETE, verifyResult);
            error(
                "Failed to verify component endpoint ID '{EID}' and version '{COMPONENT_VERSION}' with transfer result - '{RESULT}'",
                "EID", eid, "COMPONENT_VERSION", compVersion, "RESULT",
                verifyResult);
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
        };
        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event,
            std::bind(&ComponentUpdater::handleComponentUpdateFailure, this,
                      verifyFailedStatusHandler));
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

void ComponentUpdater::applyCompleteFailedStatusHandler(uint8_t applyResult)
{
    updateManager->createMessageRegistry(
        eid, fwDeviceIDRecord, componentIndex, applyFailed, "",
        PLDM_APPLY_COMPLETE, applyResult);
    error(
        "Failed to apply component endpoint ID '{EID}' and version '{COMPONENT_VERSION}', error - {ERROR}",
        "EID", eid, "ERROR", applyResult);
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
    if (!updateManager->isStageOnlyUpdate)
    {
        updateManager->createMessageRegistry(eid, fwDeviceIDRecord,
                                             componentIndex, updateSuccessful);
    }
    else
    {
        updateManager->createMessageRegistry(eid, fwDeviceIDRecord,
                                             componentIndex, stageSuccessful);
    }
    if (updateManager->fwDebug)
    {
        info(
            "Component endpoint ID '{EID}' with '{COMPONENT_VERSION}' apply complete.",
            "EID", eid, "COMPONENT_VERSION", compVersion);
    }
    if (!updateManager->isStageOnlyUpdate)
    {
        updateManager->createMessageRegistry(
            eid, fwDeviceIDRecord, componentIndex, awaitToActivate,
            updateManager->getActivationMethod(compActivationModification));
    }
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
                 ", ComponentIndex=" + std::to_string(componentIndex)),
                updateManager->fwDebug);

    uint8_t applyResult = 0;
    bitfield16_t compActivationModification{};

    auto rc = decode_apply_complete_req(request, payloadLength, &applyResult,
                                        &compActivationModification);
    if (rc)
    {
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
        auto validateApplyStatusSuccess =
            [this, applyResult, compVersion,
             compActivationModification](uint8_t currentFDState) {
                if (currentFDState == PLDM_FD_STATE_READY_XFER)
                {
                    applyCompleteSucceededStatusHandler(
                        compVersion, compActivationModification);
                }
                else
                {
                    applyCompleteFailedStatusHandler(applyResult);
                }
            };

        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event,
            [this, validateApplyStatusSuccess](EventBase&) {
                GetStatus(validateApplyStatusSuccess);
            });
    }
    else
    {
        // verify the status once by sending GetStatus before failing the update
        auto applyFailedStatusHandler = [this, applyResult]() {
            // ApplyComplete Failed
            applyCompleteFailedStatusHandler(applyResult);
        };
        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event,
            std::bind(&ComponentUpdater::handleComponentUpdateFailure, this,
                      applyFailedStatusHandler));
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

void ComponentUpdater::createRequestFwDataTimer()
{
    reqFwDataTimer = std::make_unique<sdbusplus::Timer>([this]() -> void {
        if (updateManager->fwDebug)
        {
            error(
                "RequestFWData timed out. No command received from FD within the expected time of {EXPECTEDTIME}s. EID={EID}, "
                "ComponentIndex={COMPONENTINDEX}",
                "EID", eid, "COMPONENTINDEX", componentIndex, "EXPECTEDTIME",
                updateTimeoutSeconds);
        }
        updateManager->createMessageRegistry(
            eid, fwDeviceIDRecord, componentIndex, transferFailed, "",
            PLDM_REQUEST_FIRMWARE_DATA, PLDM_FWUP_TIME_OUT);
        componentUpdaterState.set(
            ComponentUpdaterSequence::CancelUpdateComponent);
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
    });
}

void ComponentUpdater::createCompleteCommandsTimeoutTimer()
{
    completeCommandsTimeoutTimer =
        std::make_unique<sdbusplus::Timer>([this]() -> void {
            if (updateManager->fwDebug)
            {
                error("Complete Commands Timeout. EID={EID}, "
                      "ComponentIndex={COMPONENTINDEX}",
                      "EID", eid, "COMPONENTINDEX", componentIndex);
            }
            updateManager->createMessageRegistry(
                eid, fwDeviceIDRecord, componentIndex, transferFailed, "",
                PLDM_APPLY_COMPLETE, PLDM_FWUP_TIME_OUT);
            componentUpdaterState.set(
                ComponentUpdaterSequence::CancelUpdateComponent);
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
            return;
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

    printBuffer(
        pldm::utils::Tx, request,
        ("Send CancelUpdateComponentRequest for EID=" + std::to_string(eid)),
        updateManager->fwDebug);

    rc = co_await sendRecvPldmMsgOverMctp(eid, request, &response, &respMsgLen);
    if (rc)
    {
        error("Error while sending mctp request for ComponentUpdate."
              " EID={EID}, ComponentIndex={COMPONENTINDEX}",
              "EID", eid, "COMPONENTINDEX", componentIndex);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        updateComponentComplete(ComponentUpdateStatus::UpdateFailed);
        co_return rc;
    }
    rc = processCancelUpdateComponentResponse(eid, response, respMsgLen);
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

int ComponentUpdater::processCancelUpdateComponentResponse(
    mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen)
{
    if (response == nullptr || !respMsgLen)
    {
        error("No response received for CancelUpdateComponent, EID={EID}",
              "EID", eid);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        return PLDM_ERROR;
    }

    printBuffer(pldm::utils::Rx, response, respMsgLen,
                ("Received CancelUpdateComponent Response from EID=" +
                 std::to_string(eid)),
                updateManager->fwDebug);

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
        return rc;
    }
    if (completionCode)
    {
        error("CancelUpdateComponent response failed with error, EID={EID}, "
              "ComponentIndex={COMPONENTINDEX}, CC={CC}",
              "EID", eid, "COMPONENTINDEX", componentIndex, "CC",
              completionCode);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        return PLDM_ERROR;
    }
    return PLDM_SUCCESS;
}

void ComponentUpdater::updateComponentComplete(ComponentUpdateStatus status)
{
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
    std::function<void(uint8_t)> getStatusCallback)
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
                ("Send GetStatusRequest for EID=" + std::to_string(eid)),
                updateManager->fwDebug);

    rc = co_await sendRecvPldmMsgOverMctp(eid, request, &response, &respMsgLen);
    if (rc)
    {
        auto [messageStatus, oemMessageId, oemMessageError,
              oemResolution] = getOemMessage(PLDM_GET_STATUS, COMMAND_TIMEOUT);
        if (messageStatus)
        {
            updateManager->createMessageRegistryResourceErrors(
                eid, fwDeviceIDRecord, componentIndex, oemMessageId,
                oemMessageError, oemResolution);
        }
        error("Error while sending mctp request for ComponentUpdate."
              " EID={EID}, ComponentIndex={COMPONENTINDEX}",
              "EID", eid, "COMPONENTINDEX", componentIndex);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        getStatusCallback(0);
        co_return rc;
    }

    uint8_t currentFDState = 0;
    uint8_t progressPercent = 0x65;
    rc = processGetStatusResponse(eid, response, respMsgLen, currentFDState,
                                  progressPercent);
    if (rc)
    {
        error("Error while processing get request response."
              " EID={EID}, ComponentIndex={COMPONENTINDEX}",
              "EID", eid, "COMPONENTINDEX", componentIndex);
    }

    getStatusCallback(currentFDState);
    co_return rc;
}

int ComponentUpdater::processGetStatusResponse(
    mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen,
    uint8_t& currentFDState, uint8_t& progressPercent)
{
    if (response == nullptr || !respMsgLen)
    {
        error("No response received for GetStatus, EID={EID}", "EID", eid);
        return PLDM_ERROR;
    }

    printBuffer(pldm::utils::Rx, response, respMsgLen,
                ("Received GetStatus Response from EID=" + std::to_string(eid)),
                updateManager->fwDebug);

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
        error("Decoding GetStatus response failed, EID={EID}, "
              "ComponentIndex={COMPONENTINDEX}, CC={CC}",
              "EID", eid, "COMPONENTINDEX", componentIndex, "CC",
              completionCode);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        return rc;
    }
    if (completionCode)
    {
        error("GetStatus response failed with error, EID={EID}, "
              "ComponentIndex={COMPONENTINDEX}, CC={CC}",
              "EID", eid, "COMPONENTINDEX", componentIndex, "CC",
              completionCode);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        return PLDM_ERROR;
    }
    return PLDM_SUCCESS;
}

void ComponentUpdater::handleComponentUpdateFailure(
    std::function<void()> failureCallback)
{
    failureCallback();
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

} // namespace fw_update

} // namespace pldm
