#include "component_updater.hpp"

#include "libpldm/firmware_update.h"

#include "activation.hpp"
#include "update_manager.hpp"

#include <phosphor-logging/lg2.hpp>

#include <functional>

namespace pldm
{

namespace fw_update
{

requester::Coroutine ComponentUpdater::startComponentUpdater()
{
    auto rc = co_await sendUpdateComponentRequest(componentIndex);
    if (rc)
    {
        lg2::error("Error while sending component update request."
                   " EID={EID}, ComponentIndex={COMPONENTINDEX}",
                   "EID", eid, "COMPONENTINDEX", componentIndex);
    }
    co_return rc;
}

requester::Coroutine ComponentUpdater::sendUpdateComponentRequest(size_t offset)
{
    pldmRequest.reset();

    auto instanceId = updateManager->requester.getInstanceId(eid);
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

    const auto& compOptions = std::get<static_cast<size_t>(ComponentImageInfoPos::CompOptionsPos)>(comp);
    // UpdateOptionFlags
    bitfield32_t updateOptionFlags = {0};
    updateOptionFlags.bits.bit0 = updateManager->forceUpdate || compOptions.test(forceUpdateBit);
    // ComponentVersion
    const auto& compVersion = std::get<7>(comp);
    variable_field compVerStrInfo{};
    compVerStrInfo.ptr = reinterpret_cast<const uint8_t*>(compVersion.data());
    compVerStrInfo.length = static_cast<uint8_t>(compVersion.size());

    Request request(sizeof(pldm_msg_hdr) +
                    sizeof(struct pldm_update_component_req) +
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
        updateManager->requester.markFree(eid, instanceId);
        lg2::error("encode_update_component_req failed, EID={EID}, RC={RC}",
                   "EID", eid, "RC", rc);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        co_return PLDM_ERROR;
    }

    printBuffer(pldm::utils::Tx, request,
                ("Send UpdateComponent for EID=" + std::to_string(eid) +
                 " ,ComponentIndex=" + std::to_string(componentIndex)),
                updateManager->fwDebug);
    rc = co_await SendRecvPldmMsgOverMctp(updateManager->handler, eid, request,
                                          &response, &respMsgLen);
    if (rc)
    {
        lg2::error("Error while sending mctp request for ComponentUpdate."
                   " EID={EID}, ComponentIndex={COMPONENTINDEX}",
                   "EID", eid, "COMPONENTINDEX", componentIndex);
        co_return rc;
    }
    rc = processUpdateComponentResponse(eid, response, respMsgLen);
    if (rc)
    {
        lg2::error("Error while processing component update response."
                   " EID={EID}, ComponentIndex={COMPONENTINDEX}",
                   "EID", eid, "COMPONENTINDEX", componentIndex);
        co_return rc;
    }
    co_return rc;
}

int ComponentUpdater::processUpdateComponentResponse(mctp_eid_t eid,
                                                     const pldm_msg* response,
                                                     size_t respMsgLen)
{
    if (response == nullptr || !respMsgLen)
    {
        updateManager->createMessageRegistry(
            eid, fwDeviceIDRecord, componentIndex, transferFailed, "",
            PLDM_UPDATE_COMPONENT, COMMAND_TIMEOUT);
        lg2::error("No response received for updateComponent, EID={EID}", "EID",
                   eid);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event,
            std::bind(&ComponentUpdater::updateComponentComplete, this, ComponentUpdateStatus::UpdateFailed));
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
        lg2::error(
            "Decoding UpdateComponent response failed, EID={EID}, RC={RC}",
            "EID", eid, "RC", rc);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        return rc;
    }
    if (completionCode)
    {
        lg2::error(
            "UpdateComponent response failed with error completion code,"
            " EID={EID}, CC={CC}, compCompatibilityResp={CCR}, compCompatibilityRespCode= {CCRC}",
            "EID", eid, "CC", completionCode, "CCR", compCompatibilityResp,
            "CCRC", compCompatibilityRespCode);
        updateManager->createMessageRegistry(
            eid, fwDeviceIDRecord, componentIndex, transferFailed, "",
            PLDM_UPDATE_COMPONENT, PLDM_FWUP_INVALID_STATE_FOR_COMMAND);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event,
            std::bind(&ComponentUpdater::updateComponentComplete, this, ComponentUpdateStatus::UpdateFailed));
        return PLDM_ERROR;
    }
    if (compCompatibilityResp)
    {
        lg2::error(
            "In UpdateComponent response, ComponentCompatibilityResponse is non-zero EID={EID}, RC= {RC}, CompletionCode= {CC}, compCompatibilityResp={CCR}, compCompatibilityRespCode= {CCRC}",
            "EID", eid, "RC", rc, "CC", completionCode, "CCR",
            compCompatibilityResp, "CCRC", compCompatibilityRespCode);

        auto [messageStatus, oemMessageId, oemMessageError, oemResolution] =
            getCompCompatibilityMessage(PLDM_UPDATE_COMPONENT, compCompatibilityRespCode);
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
        lg2::error(
            "Decoding RequestFirmwareData request failed, EID={EID}, RC={RC}",
            "EID", eid, "RC", rc);
        rc = encode_request_firmware_data_resp(
            request->hdr.instance_id, PLDM_ERROR_INVALID_DATA, responseMsg,
            sizeof(completionCode));
        if (rc)
        {
            lg2::error(
                "Encoding RequestFirmwareData response failed, EID={EID}, RC={RC}",
                "EID", eid, "RC", rc);
        }
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        return response;
    }

    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
    const auto& comp = compImageInfos[applicableComponents[componentIndex]];
    auto compOffset = std::get<5>(comp);
    auto compSize = std::get<6>(comp);
    if (updateManager->fwDebug)
    {
        lg2::info("EID={EID}, ComponentIndex={COMPONENTINDEX}, Offset="
                  "{OFFSET}, Length={LENGTH}",
                  "EID", eid, "COMPONENTINDEX", componentIndex, "OFFSET",
                  offset, "LENGTH", length);
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
        lg2::info("Retry request for RequestFirmwareData. EID={EID}, "
                  "ComponentIndex={COMPONENTINDEX}",
                  "EID", eid, "COMPONENTINDEX", componentIndex);
    }

    if (length < PLDM_FWUP_BASELINE_TRANSFER_SIZE || length > maxTransferSize)
    {
        lg2::error(
            "RequestFirmwareData reported PLDM_FWUP_INVALID_TRANSFER_LENGTH, "
            "EID={EID}, offset={OFFSET}, length={LENGTH}",
            "EID", eid, "OFFSET", offset, "LENGTH", length);
        rc = encode_request_firmware_data_resp(
            request->hdr.instance_id, PLDM_FWUP_INVALID_TRANSFER_LENGTH,
            responseMsg, sizeof(completionCode));
        if (rc)
        {
            lg2::error(
                "Encoding RequestFirmwareData response failed, EID={EID}, RC={RC}",
                "EID", eid, "RC", rc);
        }
        return response;
    }

    if (offset + length > compSize + PLDM_FWUP_BASELINE_TRANSFER_SIZE)
    {
        lg2::error("RequestFirmwareData reported PLDM_FWUP_DATA_OUT_OF_RANGE, "
                   "EID={EID}, offset={OFFSET}, length={LENGTH}",
                   "EID", eid, "OFFSET", offset, "LENGTH", length);
        rc = encode_request_firmware_data_resp(
            request->hdr.instance_id, PLDM_FWUP_DATA_OUT_OF_RANGE, responseMsg,
            sizeof(completionCode));
        if (rc)
        {
            lg2::error(
                "Encoding RequestFirmwareData response failed, EID={EID}, RC={RC}",
                "EID", eid, "RC", rc);
        }
        return response;
    }

    size_t padBytes = 0;
    if (offset + length > compSize)
    {
        padBytes = offset + length - compSize;
    }

    response.resize(sizeof(pldm_msg_hdr) + sizeof(completionCode) + length);
    responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    package.seekg(compOffset + offset);
    package.read(reinterpret_cast<char*>(response.data() +
                                         sizeof(pldm_msg_hdr) +
                                         sizeof(completionCode)),
                 length - padBytes);
    rc = encode_request_firmware_data_resp(request->hdr.instance_id,
                                           completionCode, responseMsg,
                                           sizeof(completionCode));
    if (rc)
    {
        lg2::error(
            "Encoding RequestFirmwareData response failed, EID={EID}, RC={RC}",
            "EID", eid, "RC", rc);
        return response;
    }
    if (!reqFwDataTimer)
    {
        if (offset != 0)
        {
            lg2::warning("First data request is not at offset 0");
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
        lg2::error(
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
        lg2::error(
            "Decoding TransferComplete request failed, EID={EID}, RC={RC}",
            "EID", eid, "RC", rc);
        rc = encode_transfer_complete_resp(request->hdr.instance_id,
                                           PLDM_ERROR_INVALID_DATA, responseMsg,
                                           sizeof(completionCode));
        if (rc)
        {
            lg2::error(
                "Encoding TransferComplete response failed, EID={EID}, RC={RC}",
                "EID", eid, "RC", rc);
        }
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
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
        lg2::error("Retry request for Transfer complete, EID={EID}, "
                   "ComponentIndex={COMPONENTINDEX}",
                   "EID", eid, "COMPONENTINDEX", componentIndex);
        rc = encode_transfer_complete_resp(request->hdr.instance_id,
                                           completionCode, responseMsg,
                                           sizeof(completionCode));
        if (rc)
        {
            lg2::error(
                "Encoding TransferComplete response failed, EID={EID}, RC={RC}",
                "EID", eid, "RC", rc);
        }
        return response;
    }
    reqFwDataTimer->stop();
    reqFwDataTimer.reset();

    // create and start UA_T6 timer
    lg2::info("Progress percent is not supported. Starting UA_T6 timer");
    createCompleteCommandsTimeoutTimer();
    completeCommandsTimeoutTimer->start(
        std::chrono::seconds(completeCommandsTimeoutSeconds), false);

    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
    const auto& comp = compImageInfos[applicableComponents[componentIndex]];
    const auto& compVersion = std::get<7>(comp);

    if (transferResult == PLDM_FWUP_TRANSFER_SUCCESS)
    {
        if (updateManager->fwDebug)
        {
            lg2::info("Component Transfer complete, EID={EID}, "
                      "COMPONENT_VERSION={COMPONENT_VERSION}",
                      "EID", eid, "COMPONENT_VERSION", compVersion);
        }
        componentUpdaterState.nextState(componentUpdaterState.current);
    }
    else
    {
        componentUpdaterState.nextState(componentUpdaterState.current);
        // verify the status once by sending GetStatus before failing the update
        auto transferFailedStatusHandler = [this, transferResult]() {
            // TransferComplete Failed
            updateManager->createMessageRegistry(
                eid, fwDeviceIDRecord, componentIndex, transferFailed, "",
                PLDM_TRANSFER_COMPLETE, transferResult);
            lg2::error(
                "Transfer of the component failed, EID={EID}, ComponentIndex="
                "{COMPONENT_INDEX}, TRANSFER_RESULT={TRANSFER_RESULT}",
                "EID", eid, "COMPONENT_INDEX", componentIndex,
                "TRANSFER_RESULT", transferResult);
            componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
            if (completeCommandsTimeoutTimer)
            {
                completeCommandsTimeoutTimer->stop();
                completeCommandsTimeoutTimer.reset();
            }
            if (cancelCompUpdateHandle == nullptr)
            {
                auto co = sendcancelUpdateComponentRequest();
                cancelCompUpdateHandle = co.handle;
            }
            else if (cancelCompUpdateHandle.done())
            {
                cancelCompUpdateHandle.destroy();
                auto co = sendcancelUpdateComponentRequest();
                cancelCompUpdateHandle = co.handle;
            }
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
        lg2::error(
            "Encoding TransferComplete response failed, EID={EID}, RC={RC}",
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
        lg2::error("Decoding VerifyComplete request failed, EID={EID}, RC={RC}",
                   "EID", eid, "RC", rc);
        rc = encode_verify_complete_resp(request->hdr.instance_id,
                                         PLDM_ERROR_INVALID_DATA, responseMsg,
                                         sizeof(completionCode));
        if (rc)
        {
            lg2::error(
                "Encoding VerifyComplete response failed, EID={EID}, RC={RC}",
                "EID", eid, "RC", rc);
        }
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
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
        lg2::error("Retry request for Verify complete, EID={EID}, "
                   "ComponentIndex={COMPONENTINDEX}",
                   "EID", eid, "COMPONENTINDEX", componentIndex);
        rc = encode_verify_complete_resp(request->hdr.instance_id,
                                         completionCode, responseMsg,
                                         sizeof(completionCode));
        if (rc)
        {
            lg2::error(
                "Encoding VerifyComplete response failed, EID={EID}, RC={RC}",
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
            lg2::info("Component verification complete, EID={EID}, "
                      "COMPONENT_VERSION={COMPONENT_VERSION}",
                      "EID", eid, "COMPONENT_VERSION", compVersion);
        }
        componentUpdaterState.nextState(componentUpdaterState.current);
    }
    else
    {
        auto verifyFailedStatusHandler = [this, verifyResult]() {
            // VerifyComplete Failed
            updateManager->createMessageRegistry(
                eid, fwDeviceIDRecord, componentIndex, verificationFailed, "",
                PLDM_VERIFY_COMPLETE, verifyResult);
            lg2::error(
                "Component verification failed, EID={EID}, ComponentIndex="
                "{COMPONENT_INDEX}, VERIFY_RESULT={VERIFY_RESULT}",
                "EID", eid, "COMPONENT_INDEX", componentIndex, "VERIFY_RESULT",
                verifyResult);
            componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
            if (completeCommandsTimeoutTimer)
            {
                completeCommandsTimeoutTimer->stop();
                completeCommandsTimeoutTimer.reset();
            }
            if (cancelCompUpdateHandle == nullptr)
            {
                auto co = sendcancelUpdateComponentRequest();
                cancelCompUpdateHandle = co.handle;
            }
            else if (cancelCompUpdateHandle.done())
            {
                cancelCompUpdateHandle.destroy();
                auto co = sendcancelUpdateComponentRequest();
                cancelCompUpdateHandle = co.handle;
            }
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
        lg2::error(
            "Encoding VerifyComplete response failed, EID={EID}, RC={RC}",
            "EID", eid, "RC", rc);
        return response;
    }
    return response;
}

void ComponentUpdater::applyCompleteFailedStatusHandler(uint8_t applyResult)
{
    updateManager->createMessageRegistry(eid, fwDeviceIDRecord, componentIndex,
                                         applyFailed, "", PLDM_APPLY_COMPLETE,
                                         applyResult);
    lg2::error("Component apply failed, EID={EID}, ComponentIndex="
               "{COMPONENT_INDEX}, APPLY_RESULT={APPLY_RESULT}",
               "EID", eid, "COMPONENT_INDEX", componentIndex, "APPLY_RESULT",
               applyResult);
    componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
    if (completeCommandsTimeoutTimer)
    {
        completeCommandsTimeoutTimer->stop();
        completeCommandsTimeoutTimer.reset();
    }
    if (cancelCompUpdateHandle == nullptr)
    {
        auto co = sendcancelUpdateComponentRequest();
        cancelCompUpdateHandle = co.handle;
    }
    else if (cancelCompUpdateHandle.done())
    {
        cancelCompUpdateHandle.destroy();
        auto co = sendcancelUpdateComponentRequest();
        cancelCompUpdateHandle = co.handle;
    }
}

void ComponentUpdater::applyCompleteSucceededStatusHandler(
    const std::string& compVersion, bitfield16_t compActivationModification)
{
    updateManager->createMessageRegistry(eid, fwDeviceIDRecord, componentIndex,
                                         updateSuccessful);
    if (updateManager->fwDebug)
    {
        lg2::info("Component apply complete, EID={EID}, "
                  "COMPONENT_VERSION={COMPONENT_VERSION}",
                  "EID", eid, "COMPONENT_VERSION", compVersion);
    }
    updateManager->createMessageRegistry(
        eid, fwDeviceIDRecord, componentIndex, awaitToActivate,
        updateManager->getActivationMethod(compActivationModification));
    pldmRequest = std::make_unique<sdeventplus::source::Defer>(
        updateManager->event,
        std::bind(&ComponentUpdater::updateComponentComplete, this, ComponentUpdateStatus::UpdateComplete));
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
        lg2::error("Decoding ApplyComplete request failed, EID={EID}, RC={RC}",
                   "EID", eid, "RC", rc);
        rc = encode_apply_complete_resp(request->hdr.instance_id,
                                        PLDM_ERROR_INVALID_DATA, responseMsg,
                                        sizeof(completionCode));
        if (rc)
        {
            lg2::error(
                "Encoding ApplyComplete response failed, EID={EID}, RC={RC}",
                "EID", eid, "RC", rc);
        }
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
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
        lg2::error("Retry request for apply complete, EID={EID}, "
                   "ComponentIndex={COMPONENTINDEX}",
                   "EID", eid, "COMPONENTINDEX", componentIndex);
        rc =
            encode_apply_complete_resp(request->hdr.instance_id, completionCode,
                                       responseMsg, sizeof(completionCode));
        if (rc)
        {
            lg2::error(
                "Encoding ApplyComplete response failed, EID={EID}, RC={RC}",
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
                applyCompleteFailedStatusHandler(applyResult);
            }
        };

        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event, [this, validateApplyStatusSuccess](EventBase&) {
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
        lg2::error("Encoding ApplyComplete response failed, EID={EID}, RC={RC}",
                   "EID", eid, "RC", rc);
        return response;
    }
    return response;
}

void ComponentUpdater::createRequestFwDataTimer()
{
    reqFwDataTimer = std::make_unique<phosphor::Timer>([this]() -> void {
        if (updateManager->fwDebug)
        {
            lg2::error("RequestUpdate timeout EID={EID}, "
                       "ComponentIndex={COMPONENTINDEX}",
                       "EID", eid, "COMPONENTINDEX", componentIndex);
        }
        updateManager->createMessageRegistry(
            eid, fwDeviceIDRecord, componentIndex, transferFailed, "",
            PLDM_REQUEST_FIRMWARE_DATA, COMMAND_TIMEOUT);
        componentUpdaterState.set(
            ComponentUpdaterSequence::CancelUpdateComponent);
        if (cancelCompUpdateHandle == nullptr)
        {
            auto co = sendcancelUpdateComponentRequest();
            cancelCompUpdateHandle = co.handle;
        }
        else if (cancelCompUpdateHandle.done())
        {
            cancelCompUpdateHandle.destroy();
            auto co = sendcancelUpdateComponentRequest();
            cancelCompUpdateHandle = co.handle;
        }
        return;
    });
}

void ComponentUpdater::createCompleteCommandsTimeoutTimer()
{
    completeCommandsTimeoutTimer =
        std::make_unique<phosphor::Timer>([this]() -> void {
            if (updateManager->fwDebug)
            {
                lg2::error("Complete Commands Timeout. EID={EID}, "
                           "ComponentIndex={COMPONENTINDEX}",
                           "EID", eid, "COMPONENTINDEX", componentIndex);
            }
            updateManager->createMessageRegistry(
                eid, fwDeviceIDRecord, componentIndex, transferFailed, "",
                PLDM_APPLY_COMPLETE, COMMAND_TIMEOUT);
            componentUpdaterState.set(
                ComponentUpdaterSequence::CancelUpdateComponent);
            if (cancelCompUpdateHandle == nullptr)
            {
                auto co = sendcancelUpdateComponentRequest();
                cancelCompUpdateHandle = co.handle;
            }
            else if (cancelCompUpdateHandle.done())
            {
                cancelCompUpdateHandle.destroy();
                auto co = sendcancelUpdateComponentRequest();
                cancelCompUpdateHandle = co.handle;
            }
            return;
        });
}

requester::Coroutine ComponentUpdater::sendcancelUpdateComponentRequest()
{
    pldmRequest.reset();
    auto instanceId = updateManager->requester.getInstanceId(eid);
    Request request(sizeof(pldm_msg_hdr));
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    const pldm_msg* response = NULL;
    size_t respMsgLen = 0;

    auto rc = encode_cancel_update_component_req(
        instanceId, requestMsg, PLDM_CANCEL_UPDATE_COMPONENT_REQ_BYTES);
    if (rc)
    {
        updateManager->requester.markFree(eid, instanceId);
        lg2::error("encode_cancel_update_component_req failed, EID={EID}, "
                   "ComponentIndex={COMPONENTINDEX}, RC={RC}",
                   "EID", eid, "COMPONENTINDEX", componentIndex, "RC", rc);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        co_return PLDM_ERROR;
    }

    printBuffer(
        pldm::utils::Tx, request,
        ("Send CancelUpdateComponentRequest for EID=" + std::to_string(eid)),
        updateManager->fwDebug);

    rc = co_await SendRecvPldmMsgOverMctp(updateManager->handler, eid, request,
                                          &response, &respMsgLen);
    if (rc)
    {
        lg2::error("Error while sending mctp request for ComponentUpdate."
                   " EID={EID}, ComponentIndex={COMPONENTINDEX}",
                   "EID", eid, "COMPONENTINDEX", componentIndex);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        co_return rc;
    }
    rc = processCancelUpdateComponentResponse(eid, response,
                                                       respMsgLen);
    if (rc)
    {
        lg2::error("Error while processing cancel update response."
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
        lg2::error("No response received for CancelUpdateComponent, EID={EID}",
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
        lg2::error("Decoding CancelUpdateComponent response failed, EID={EID}, "
                   "ComponentIndex={COMPONENTINDEX}, CC={CC}",
                   "EID", eid, "COMPONENTINDEX", componentIndex, "CC",
                   completionCode);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        return rc;
    }
    if (completionCode)
    {
        lg2::error(
            "CancelUpdateComponent response failed with error, EID={EID}, "
            "ComponentIndex={COMPONENTINDEX}, CC={CC}",
            "EID", eid, "COMPONENTINDEX", componentIndex, "CC", completionCode);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        return PLDM_ERROR;
    }
    return PLDM_SUCCESS;
}

void ComponentUpdater::updateComponentComplete(ComponentUpdateStatus status)
{
    if (updateCompletionCoHandle == nullptr)
    {
        auto co =
            deviceUpdater->updateComponentCompletion(componentIndex, status);
        updateCompletionCoHandle = co.handle;
    }
    else if (updateCompletionCoHandle.done())
    {
        updateCompletionCoHandle.destroy();
        auto co =
            deviceUpdater->updateComponentCompletion(componentIndex, status);
        updateCompletionCoHandle = co.handle;
    }
    return;
}

requester::Coroutine
    ComponentUpdater::GetStatus(std::function<void(uint8_t)> getStatusCallback)
{
    uint8_t currentFDState = 0;
    uint8_t progressPercent = 0x65;
    pldmRequest.reset();
    auto instanceId = updateManager->requester.getInstanceId(eid);
    Request request(sizeof(pldm_msg_hdr));
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    const pldm_msg* response = NULL;
    size_t respMsgLen = 0;

    auto rc = encode_get_status_req(instanceId, requestMsg,
                                    PLDM_GET_STATUS_REQ_BYTES);
    if (rc)
    {
        updateManager->requester.markFree(eid, instanceId);
        lg2::error("encode_get_status_req failed, EID={EID}, "
                   "ComponentIndex={COMPONENTINDEX}, RC={RC}",
                   "EID", eid, "COMPONENTINDEX", componentIndex, "RC", rc);
        co_return PLDM_ERROR;
    }

    printBuffer(pldm::utils::Tx, request,
                ("Send GetStatusRequest for EID=" + std::to_string(eid)),
                updateManager->fwDebug);

    rc = co_await SendRecvPldmMsgOverMctp(updateManager->handler, eid, request,
                                          &response, &respMsgLen);
    if (rc)
    {
        lg2::error("Error while sending mctp request for ComponentUpdate."
                   " EID={EID}, ComponentIndex={COMPONENTINDEX}",
                   "EID", eid, "COMPONENTINDEX", componentIndex);
        co_return rc;
    }
    rc = processGetStatusResponse(eid, response, respMsgLen,
                                           currentFDState, progressPercent);
    if (rc)
    {
        lg2::error("Error while processing get request response."
                   " EID={EID}, ComponentIndex={COMPONENTINDEX}",
                   "EID", eid, "COMPONENTINDEX", componentIndex);
    }
    getStatusCallback(currentFDState);
    co_return rc;
}

int ComponentUpdater::processGetStatusResponse(mctp_eid_t eid,
                                               const pldm_msg* response,
                                               size_t respMsgLen,
                                               uint8_t& currentFDState,
                                               uint8_t& progressPercent)
{
    if (response == nullptr || !respMsgLen)
    {
        lg2::error("No response received for GetStatus, EID={EID}", "EID", eid);
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
    auto rc = decode_get_status_resp(response, respMsgLen, &completionCode,
                                     &currentFDState, &previousState, &auxState,
                                     &auxStateStatus, &progressPercent,
                                     &reasonCode, &updateOptionFlagsEnabled);
    if (rc)
    {
        lg2::error("Decoding GetStatus response failed, EID={EID}, "
                   "ComponentIndex={COMPONENTINDEX}, CC={CC}",
                   "EID", eid, "COMPONENTINDEX", componentIndex, "CC",
                   completionCode);
        componentUpdaterState.set(ComponentUpdaterSequence::Invalid);
        return rc;
    }
    if (completionCode)
    {
        lg2::error("GetStatus response failed with error, EID={EID}, "
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

} // namespace fw_update

} // namespace pldm