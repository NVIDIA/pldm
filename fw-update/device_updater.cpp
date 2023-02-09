#include "device_updater.hpp"

#include "libpldm/firmware_update.h"

#include "activation.hpp"
#include "update_manager.hpp"

#include <phosphor-logging/lg2.hpp>

#include <functional>

namespace pldm
{

namespace fw_update
{

void DeviceUpdater::startFwUpdateFlow()
{
    auto instanceId = updateManager->requester.getInstanceId(eid);
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

    Request request(sizeof(pldm_msg_hdr) +
                    sizeof(struct pldm_request_update_req) +
                    compImgSetVerStrInfo.length);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    auto rc = encode_request_update_req(
        instanceId, maxTransferSize, applicableComponents.size(),
        PLDM_FWUP_MIN_OUTSTANDING_REQ, fwDevicePkgData.size(),
        PLDM_STR_TYPE_ASCII, compImgSetVerStrInfo.length, &compImgSetVerStrInfo,
        requestMsg,
        sizeof(struct pldm_request_update_req) + compImgSetVerStrInfo.length);
    if (rc)
    {
        updateManager->requester.markFree(eid, instanceId);
        lg2::error("encode_request_update_req failed, EID={EID}, RC={RC}",
                   "EID", eid, "RC", rc);
        uaState.set(UASequence::Invalid);
        // Handle error scenario
    }

    printBuffer(pldm::utils::Tx, request,
                ("Send RequestUpdate for EID=" + std::to_string(eid)));

    rc = updateManager->handler.registerRequest(
        eid, instanceId, PLDM_FWUP, PLDM_REQUEST_UPDATE, std::move(request),
        std::move(std::bind_front(&DeviceUpdater::requestUpdate, this)));
    if (rc)
    {
        lg2::error("Failed to send RequestUpdate request, EID={EID}, RC={RC}",
                   "EID", eid, "RC", rc);
        uaState.set(UASequence::Invalid);
        // Handle error scenario
    }
}

void DeviceUpdater::requestUpdate(mctp_eid_t eid, const pldm_msg* response,
                                  size_t respMsgLen)
{
    if (response == nullptr || !respMsgLen)
    {
        const auto& applicableComponents =
            std::get<ApplicableComponents>(fwDeviceIDRecord);
        for (size_t compIndex = 0; compIndex < applicableComponents.size();
             compIndex++)
        {
            auto [messageStatus, oemMessageId, oemMessageError, oemResolution] =
                getOemMessage(PLDM_REQUEST_UPDATE, COMMAND_TIMEOUT);
            if (messageStatus)
            {
                updateManager->createMessageRegistryResourceErrors(
                    eid, fwDeviceIDRecord, compIndex, oemMessageId,
                    oemMessageError, oemResolution);
            }
        }
        lg2::error("No response received for RequestUpdate, EID={EID}", "EID",
                   eid);
        updateManager->updateDeviceCompletion(eid, false);
        uaState.set(UASequence::Invalid);
        return;
    }

    printBuffer(
        pldm::utils::Rx, response, respMsgLen,
        ("Received requestUpdate Response from EID=" + std::to_string(eid)));

    uint8_t completionCode = 0;
    uint16_t fdMetaDataLen = 0;
    uint8_t fdWillSendPkgData = 0;

    auto rc = decode_request_update_resp(response, respMsgLen, &completionCode,
                                         &fdMetaDataLen, &fdWillSendPkgData);
    if (rc)
    {
        lg2::error("Decoding RequestUpdate response failed, EID={EID}, RC={RC}",
                   "EID", eid, "RC", rc);
        uaState.set(UASequence::Invalid);
        return;
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
        lg2::error("RequestUpdate response failed with error completion code."
                   " EID={EID}, CompletionCode={COMPLETIONCODE}",
                   "EID", eid, "COMPLETIONCODE", completionCode);
        updateManager->updateDeviceCompletion(eid, false);
        uaState.set(UASequence::Invalid);
        return;
    }

    uaState.nextState(uaState.current, componentIndex, numComponents);
    // Optional fields DeviceMetaData and GetPackageData not handled
    pldmRequest = std::make_unique<sdeventplus::source::Defer>(
        updateManager->event,
        std::bind(&DeviceUpdater::sendPassCompTableRequest, this,
                  componentIndex));
}

void DeviceUpdater::sendPassCompTableRequest(size_t offset)
{
    pldmRequest.reset();

    auto instanceId = updateManager->requester.getInstanceId(eid);
    // TransferFlag
    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
    uint8_t transferFlag = 0;
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
        // Handle error scenario
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

    Request request(sizeof(pldm_msg_hdr) +
                    sizeof(struct pldm_pass_component_table_req) +
                    compVerStrInfo.length);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    auto rc = encode_pass_component_table_req(
        instanceId, transferFlag, compClassification, compIdentifier,
        compClassificationIndex, compComparisonStamp, PLDM_STR_TYPE_ASCII,
        compVerStrInfo.length, &compVerStrInfo, requestMsg,
        sizeof(pldm_pass_component_table_req) + compVerStrInfo.length);
    if (rc)
    {
        updateManager->requester.markFree(eid, instanceId);
        lg2::error("encode_pass_component_table_req failed, EID={EID}, RC={RC}",
                   "EID", eid, "RC", rc);
        uaState.set(UASequence::Invalid);
        // Handle error scenario
    }

    printBuffer(pldm::utils::Tx, request,
                ("Send PassCompTable for EID=" + std::to_string(eid) +
                 " ,ComponentIndex=" + std::to_string(componentIndex)));

    rc = updateManager->handler.registerRequest(
        eid, instanceId, PLDM_FWUP, PLDM_PASS_COMPONENT_TABLE,
        std::move(request),
        std::move(std::bind_front(&DeviceUpdater::passCompTable, this)));
    if (rc)
    {
        lg2::error(
            "Failed to send PassComponentTable request, EID={EID}, RC={RC} ",
            "EID", eid, "RC", rc);
        uaState.set(UASequence::Invalid);
        // Handle error scenario
    }
}

void DeviceUpdater::passCompTable(mctp_eid_t eid, const pldm_msg* response,
                                  size_t respMsgLen)
{
    if (response == nullptr || !respMsgLen)
    {
        auto [messageStatus, oemMessageId, oemMessageError, oemResolution] =
            getOemMessage(PLDM_PASS_COMPONENT_TABLE, COMMAND_TIMEOUT);
        if (messageStatus)
        {
            updateManager->createMessageRegistryResourceErrors(
                eid, fwDeviceIDRecord, componentIndex, oemMessageId,
                oemMessageError, oemResolution);
        }
        lg2::error("No response received for PassComponentTable, EID={EID}",
                   "EID", eid);
        updateManager->updateDeviceCompletion(eid, false);
        uaState.set(UASequence::Invalid);
        return;
    }

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
        // Handle error scenario
        lg2::error(
            "Decoding PassComponentTable response failed, EID={EID}, RC={RC}",
            "EID", eid, "RC", rc);
        uaState.set(UASequence::Invalid);
        return;
    }
    if (completionCode)
    {
        // Handle error scenario
        lg2::error(
            "PassComponentTable response failed with error completion code,"
            " EID={EID}, CC={CC}",
            "EID", eid, "CC", completionCode);
        auto [messageStatus, oemMessageId, oemMessageError, oemResolution] =
            getOemMessage(PLDM_PASS_COMPONENT_TABLE,
                          PLDM_FWUP_INVALID_STATE_FOR_COMMAND);
        if (messageStatus)
        {
            updateManager->createMessageRegistryResourceErrors(
                eid, fwDeviceIDRecord, componentIndex, oemMessageId,
                oemMessageError, oemResolution);
        }
        updateManager->updateDeviceCompletion(eid, false);
        uaState.set(UASequence::Invalid);
        return;
    }
    // Handle ComponentResponseCode

    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
    if (componentIndex == applicableComponents.size() - 1)
    {
        uaState.nextState(uaState.current, numComponents, numComponents);
        componentIndex = 0;
        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event,
            std::bind(&DeviceUpdater::sendUpdateComponentRequest, this,
                      componentIndex));
    }
    else
    {
        componentIndex++;
        uaState.nextState(uaState.current, componentIndex, numComponents);
        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event,
            std::bind(&DeviceUpdater::sendPassCompTableRequest, this,
                      componentIndex));
    }
}

void DeviceUpdater::sendUpdateComponentRequest(size_t offset)
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
    else
    {
        // Handle error scenario
    }

    // UpdateOptionFlags
    bitfield32_t updateOptionFlags;
    // TODO: Revert to reading the UpdateOptionFlags from package header instead
    // of hardcoding.
    // updateOptionFlags.bits.bit0 = std::get<3>(comp)[0];
    updateOptionFlags.bits.bit0 = 1;
    // ComponentVersion
    const auto& compVersion = std::get<7>(comp);
    variable_field compVerStrInfo{};
    compVerStrInfo.ptr = reinterpret_cast<const uint8_t*>(compVersion.data());
    compVerStrInfo.length = static_cast<uint8_t>(compVersion.size());

    Request request(sizeof(pldm_msg_hdr) +
                    sizeof(struct pldm_update_component_req) +
                    compVerStrInfo.length);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

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
        uaState.set(UASequence::Invalid);
        // Handle error scenario
    }

    printBuffer(pldm::utils::Tx, request,
                ("Send UpdateComponent for EID=" + std::to_string(eid) +
                 " ,ComponentIndex=" + std::to_string(componentIndex)));

    rc = updateManager->handler.registerRequest(
        eid, instanceId, PLDM_FWUP, PLDM_UPDATE_COMPONENT, std::move(request),
        std::move(std::bind_front(&DeviceUpdater::updateComponent, this)));
    if (rc)
    {
        lg2::error(
            "Failed to send UpdateComponent request, EID={EID}, RC={RC} ",
            "EID", eid, "RC", rc);
        uaState.set(UASequence::Invalid);
        // Handle error scenario
    }
}

void DeviceUpdater::updateComponent(mctp_eid_t eid, const pldm_msg* response,
                                    size_t respMsgLen)
{
    if (response == nullptr || !respMsgLen)
    {
        updateManager->createMessageRegistry(
            eid, fwDeviceIDRecord, componentIndex, transferFailed, "",
            PLDM_UPDATE_COMPONENT, COMMAND_TIMEOUT);
        lg2::error("No response received for updateComponent, EID={EID}", "EID",
                   eid);
        updateManager->updateDeviceCompletion(eid, false);
        uaState.set(UASequence::Invalid);
        return;
    }

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
        lg2::error(
            "Decoding UpdateComponent response failed, EID={EID}, RC={RC}",
            "EID", eid, "RC", rc);
        uaState.set(UASequence::Invalid);
        return;
    }
    if (completionCode)
    {
        lg2::error("UpdateComponent response failed with error completion code,"
                   " EID={EID}, CC={CC}",
                   "EID", eid, "CC", completionCode);
        updateManager->createMessageRegistry(
            eid, fwDeviceIDRecord, componentIndex, transferFailed, "",
            PLDM_UPDATE_COMPONENT, PLDM_FWUP_INVALID_STATE_FOR_COMMAND);
        updateManager->updateDeviceCompletion(eid, false);
        uaState.set(UASequence::Invalid);
        return;
    }

    uaState.nextState(uaState.current, componentIndex, numComponents);

    updateManager->createMessageRegistry(eid, fwDeviceIDRecord, componentIndex,
                                         transferringToComponent);
}

Response DeviceUpdater::requestFwData(const pldm_msg* request,
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

    if (!uaState.expectedState(UASequence::RequestFirmwareData))
    {
        return sendCommandNotExpectedResponse(request, payloadLength);
    }

    if (length < PLDM_FWUP_BASELINE_TRANSFER_SIZE || length > maxTransferSize)
    {
        lg2::error(
            "RequestFirmwareData reported PLDM_FWUP_INVALID_TRANSFER_LENGTH, EID={EID}, offset={OFFSET}, length={LENGTH}",
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
        lg2::error(
            "RequestFirmwareData reported PLDM_FWUP_DATA_OUT_OF_RANGE, EID={EID}, offset={OFFSET}, length={LENGTH}",
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
    if (offset == 0 && !reqFwDataTimer)
    {
        // create timer for first request
        createRequestFwDataTimer();
    }
    reqFwDataTimer->start(std::chrono::seconds(updateTimeoutSeconds), false);
    return response;
}

Response DeviceUpdater::transferComplete(const pldm_msg* request,
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
        return response;
    }

    if (!uaState.expectedState(UASequence::TransferComplete))
    {
        return sendCommandNotExpectedResponse(request, payloadLength);
    }

    reqFwDataTimer->stop();
    reqFwDataTimer.reset();

    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
    const auto& comp = compImageInfos[applicableComponents[componentIndex]];
    const auto& compVersion = std::get<7>(comp);

    if (transferResult == PLDM_FWUP_TRANSFER_SUCCESS)
    {
        if (updateManager->fwDebug)
        {
            lg2::info(
                "Component Transfer complete, EID={EID}, COMPONENT_VERSION={COMPONENT_VERSION}",
                "EID", eid, "COMPONENT_VERSION", compVersion);
        }
        uaState.nextState(uaState.current, componentIndex, numComponents);
    }
    else
    {
        // TransferComplete Failed
        updateManager->createMessageRegistry(
            eid, fwDeviceIDRecord, componentIndex, transferFailed, "",
            PLDM_TRANSFER_COMPLETE, transferResult);
        lg2::error(
            "Transfer of the component failed, EID={EID}, COMPONENT_VERSION={COMPONENT_VERSION}, TRANSFER_RESULT={TRANSFER_RESULT}",
            "EID", eid, "COMPONENT_VERSION", compVersion, "TRANSFER_RESULT",
            transferResult);
        updateManager->updateDeviceCompletion(eid, false);
        uaState.set(UASequence::Invalid);
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

Response DeviceUpdater::verifyComplete(const pldm_msg* request,
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
        return response;
    }

    if (!uaState.expectedState(UASequence::VerifyComplete))
    {
        return sendCommandNotExpectedResponse(request, payloadLength);
    }

    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
    const auto& comp = compImageInfos[applicableComponents[componentIndex]];
    const auto& compVersion = std::get<7>(comp);

    if (verifyResult == PLDM_FWUP_VERIFY_SUCCESS)
    {
        if (updateManager->fwDebug)
        {
            lg2::info(
                "Component verification complete, EID={EID}, COMPONENT_VERSION={COMPONENT_VERSION}",
                "EID", eid, "COMPONENT_VERSION", compVersion);
        }
        uaState.nextState(uaState.current, componentIndex, numComponents);
    }
    else
    {
        // VerifyComplete Failed
        updateManager->createMessageRegistry(
            eid, fwDeviceIDRecord, componentIndex, verificationFailed, "",
            PLDM_VERIFY_COMPLETE, verifyResult);
        lg2::error(
            "Component verification failed, EID={EID}, COMPONENT_VERSION={COMPONENT_VERSION}, VERIFY_RESULT={VERIFY_RESULT}",
            "EID", eid, "COMPONENT_VERSION", compVersion, "VERIFY_RESULT",
            verifyResult);
        updateManager->updateDeviceCompletion(eid, false);
        uaState.set(UASequence::Invalid);
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

Response DeviceUpdater::applyComplete(const pldm_msg* request,
                                      size_t payloadLength)
{
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
        return response;
    }

    if (!uaState.expectedState(UASequence::ApplyComplete))
    {
        return sendCommandNotExpectedResponse(request, payloadLength);
    }

    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
    const auto& comp = compImageInfos[applicableComponents[componentIndex]];
    const auto& compVersion = std::get<7>(comp);

    if (applyResult == PLDM_FWUP_APPLY_SUCCESS ||
        applyResult == PLDM_FWUP_APPLY_SUCCESS_WITH_ACTIVATION_METHOD)
    {
        updateManager->createMessageRegistry(eid, fwDeviceIDRecord,
                                             componentIndex, updateSuccessful);
        if (updateManager->fwDebug)
        {
            lg2::info(
                "Component apply complete, EID={EID}, COMPONENT_VERSION={COMPONENT_VERSION}",
                "EID", eid, "COMPONENT_VERSION", compVersion);
        }
        updateManager->createMessageRegistry(
            eid, fwDeviceIDRecord, componentIndex, awaitToActivate,
            updateManager->getActivationMethod(compActivationModification));
        successCompNames.emplace_back(updateManager->getComponentName(
            eid, fwDeviceIDRecord, componentIndex));
    }
    else
    {
        // ApplyComplete Failed
        updateManager->createMessageRegistry(eid, fwDeviceIDRecord,
                                             componentIndex, applyFailed);
        lg2::error(
            "Component apply failed, EID={EID}, COMPONENT_VERSION={COMPONENT_VERSION}, APPLY_RESULT={APPLY_RESULT}",
            "EID", eid, "COMPONENT_VERSION", compVersion, "APPLY_RESULT",
            applyResult);
        updateManager->updateDeviceCompletion(eid, false);
        uaState.set(UASequence::Invalid);
    }

    rc = encode_apply_complete_resp(request->hdr.instance_id, completionCode,
                                    responseMsg, sizeof(completionCode));
    if (rc)
    {
        lg2::error("Encoding ApplyComplete response failed, EID={EID}, RC={RC}",
                   "EID", eid, "RC", rc);
        return response;
    }

    if (componentIndex == applicableComponents.size() - 1)
    {
        uaState.nextState(uaState.current, numComponents, numComponents);
        componentIndex = 0;
        componentUpdateStatus.clear();
        componentUpdateStatus[componentIndex] = true;
        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event,
            std::bind(&DeviceUpdater::sendActivateFirmwareRequest, this));
    }
    else
    {
        updateManager->updateActivationProgress(); // for previous component
        componentIndex++;
        uaState.nextState(uaState.current, componentIndex, numComponents);
        componentUpdateStatus[componentIndex] = true;
        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event,
            std::bind(&DeviceUpdater::sendUpdateComponentRequest, this,
                      componentIndex));
    }

    return response;
}

void DeviceUpdater::sendActivateFirmwareRequest()
{
    pldmRequest.reset();
    auto instanceId = updateManager->requester.getInstanceId(eid);
    Request request(sizeof(pldm_msg_hdr) +
                    sizeof(struct pldm_activate_firmware_req));
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    auto rc = encode_activate_firmware_req(
        instanceId, PLDM_NOT_ACTIVATE_SELF_CONTAINED_COMPONENTS, requestMsg,
        sizeof(pldm_activate_firmware_req));
    if (rc)
    {
        updateManager->requester.markFree(eid, instanceId);
        lg2::error("encode_activate_firmware_req failed, EID={EID}, RC={RC}",
                   "EID", eid, "RC", rc);
    }

    printBuffer(pldm::utils::Tx, request,
                ("Send ActivateFirmware for EID=" + std::to_string(eid)));

    rc = updateManager->handler.registerRequest(
        eid, instanceId, PLDM_FWUP, PLDM_ACTIVATE_FIRMWARE, std::move(request),
        std::move(std::bind_front(&DeviceUpdater::activateFirmware, this)));
    if (rc)
    {
        lg2::error(
            "Failed to send ActivateFirmware request, EID={EID}, RC={RC} ",
            "EID", eid, "RC", rc);
    }
}

void DeviceUpdater::activateFirmware(mctp_eid_t eid, const pldm_msg* response,
                                     size_t respMsgLen)
{
    if (response == nullptr || !respMsgLen)
    {
        // Handle error scenario
        lg2::error("No response received for ActivateFirmware, EID={EID}",
                   "EID", eid);
        updateManager->updateDeviceCompletion(eid, false);
        uaState.set(UASequence::Invalid);
        const auto& applicableComponents =
            std::get<ApplicableComponents>(fwDeviceIDRecord);
        for (size_t compIndex = 0; compIndex < applicableComponents.size();
             compIndex++)
        {
            updateManager->createMessageRegistry(
                eid, fwDeviceIDRecord, compIndex, activateFailed, "",
                PLDM_ACTIVATE_FIRMWARE, COMMAND_TIMEOUT);
        }
        return;
    }

    printBuffer(
        pldm::utils::Rx, response, respMsgLen,
        ("Received ActivateFirmware Response from EID=" + std::to_string(eid)));

    uint8_t completionCode = 0;
    uint16_t estimatedTimeForActivation = 0;

    // On receiving ActivateFirmware response success/failure make the UA state
    // to Invalid to further not responds to any PLDM Type 5 requests from FD.
    uaState.set(UASequence::Invalid);

    auto rc = decode_activate_firmware_resp(
        response, respMsgLen, &completionCode, &estimatedTimeForActivation);
    if (rc)
    {
        // Handle error scenario
        lg2::error(
            "Decoding ActivateFirmware response failed, EID={EID}, RC={RC}",
            "EID", eid, "RC", rc);
        return;
    }
    if (completionCode)
    {
        const auto& applicableComponents =
            std::get<ApplicableComponents>(fwDeviceIDRecord);
        for (size_t compIndex = 0; compIndex < applicableComponents.size();
             compIndex++)
        {
            updateManager->createMessageRegistry(eid, fwDeviceIDRecord,
                                                 compIndex, activateFailed);
        }
        lg2::error(
            "ActivateFirmware response failed with error completion code, EID={EID}, CC={CC}",
            "EID", eid, "CC", completionCode);
        updateManager->updateDeviceCompletion(eid, false);
        return;
    }

    updateManager->updateDeviceCompletion(eid, true, successCompNames);
}

void DeviceUpdater::printBuffer(bool isTx, const std::vector<uint8_t>& buffer,
                                const std::string& message)
{
    if (updateManager->fwDebug)
    {
        lg2::info("{INFO_MESSAGE}", "INFO_MESSAGE", message);
        pldm::utils::printBuffer(isTx, buffer);
    }
}

void DeviceUpdater::printBuffer(bool isTx, const pldm_msg* buffer,
                                size_t bufferLen, const std::string& message)
{
    if (updateManager->fwDebug)
    {
        lg2::info("{INFO_MESSAGE}", "INFO_MESSAGE", message);
        auto ptr = reinterpret_cast<const uint8_t*>(buffer);
        auto outBuffer =
            std::vector<uint8_t>(ptr, ptr + (sizeof(pldm_msg_hdr) + bufferLen));
        pldm::utils::printBuffer(isTx, outBuffer);
    }
}

void DeviceUpdater::createRequestFwDataTimer()
{
    reqFwDataTimer = std::make_unique<phosphor::Timer>([this]() {
        if (updateManager->fwDebug)
        {
            lg2::error(
                "RequestUpdate timeout EID={EID}, ComponentIndex={COMPONENTINDEX}",
                "EID", eid, "COMPONENTINDEX", componentIndex);
        }
        updateManager->createMessageRegistry(
            eid, fwDeviceIDRecord, componentIndex, transferFailed, "",
            PLDM_REQUEST_FIRMWARE_DATA, COMMAND_TIMEOUT);
        componentUpdateStatus[componentIndex] = false;
        uaState.set(UASequence::CancelUpdateComponent);
        sendcancelUpdateComponentRequest();
        updateManager->updateDeviceCompletion(eid, false);
        return;
    });
}

void DeviceUpdater::sendcancelUpdateComponentRequest()
{
    pldmRequest.reset();
    auto instanceId = updateManager->requester.getInstanceId(eid);
    Request request(sizeof(pldm_msg_hdr));
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    auto rc = encode_cancel_update_component_req(
        instanceId, requestMsg, PLDM_CANCEL_UPDATE_COMPONENT_REQ_BYTES);
    if (rc)
    {
        updateManager->requester.markFree(eid, instanceId);
        lg2::error(
            "encode_cancel_update_component_req failed, EID={EID}, ComponentIndex={COMPONENTINDEX}, RC={RC}",
            "EID", eid, "COMPONENTINDEX", componentIndex, "RC", rc);
        uaState.set(UASequence::Invalid);
    }

    printBuffer(
        pldm::utils::Tx, request,
        ("Send CancelUpdateComponentRequest for EID=" + std::to_string(eid)));

    rc = updateManager->handler.registerRequest(
        eid, instanceId, PLDM_FWUP, PLDM_CANCEL_UPDATE_COMPONENT,
        std::move(request),
        std::move(
            std::bind_front(&DeviceUpdater::cancelUpdateComponent, this)));
    if (rc)
    {
        lg2::error(
            "Failed to send cancelUpdateComponent request, EID={EID}, ComponentIndex={COMPONENTINDEX}, RC={RC} ",
            "EID", eid, "COMPONENTINDEX", componentIndex, "RC", rc);
        uaState.set(UASequence::Invalid);
    }
}

void DeviceUpdater::cancelUpdateComponent(mctp_eid_t eid,
                                          const pldm_msg* response,
                                          size_t respMsgLen)
{
    if (response == nullptr || !respMsgLen)
    {
        // Handle error scenario
        lg2::error("No response received for CancelUpdateComponent, EID={EID}",
                   "EID", eid);
        uaState.set(UASequence::Invalid);
        return;
    }

    printBuffer(pldm::utils::Rx, response, respMsgLen,
                ("Received CancelUpdateComponent Response from EID=" +
                 std::to_string(eid)));

    uint8_t completionCode = 0;
    auto rc = decode_cancel_update_component_resp(response, respMsgLen,
                                                  &completionCode);
    if (rc)
    {
        lg2::error(
            "Decoding CancelUpdateComponent response failed, EID={EID}, ComponentIndex={COMPONENTINDEX}, CC={CC}",
            "EID", eid, "COMPONENTINDEX", componentIndex, "CC", completionCode);
        uaState.set(UASequence::Invalid);
        return;
    }
    if (completionCode)
    {
        lg2::error(
            "CancelUpdateComponent response failed with error, EID={EID}, ComponentIndex={COMPONENTINDEX}, CC={CC}",
            "EID", eid, "COMPONENTINDEX", componentIndex, "CC", completionCode);
        uaState.set(UASequence::Invalid);
        return;
    }
    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
    // this scenario occurs when last component update is cancelled
    if (componentIndex == applicableComponents.size() - 1)
    {
        size_t cancelledUpdates = 0;
        for (auto& compStatus : componentUpdateStatus)
        {
            if (!compStatus.second)
            {
                cancelledUpdates += 1;
            }
        }
        // send activation request if atleast one device is succeeded
        if (cancelledUpdates < applicableComponents.size() - 1)
        {
            componentIndex = 0;
            componentUpdateStatus.clear();
            pldmRequest = std::make_unique<sdeventplus::source::Defer>(
                updateManager->event,
                std::bind(&DeviceUpdater::sendActivateFirmwareRequest, this));
            uaState.set(UASequence::ActivateFirmware);
        }
        else
        {
            uaState.set(UASequence::Invalid);
        }
    }
    else
    {
        componentIndex++;
        componentUpdateStatus[componentIndex] = true;
        pldmRequest = std::make_unique<sdeventplus::source::Defer>(
            updateManager->event,
            std::bind(&DeviceUpdater::sendUpdateComponentRequest, this,
                      componentIndex));
        uaState.set(UASequence::UpdateComponent);
    }
    return;
}

Response DeviceUpdater::sendCommandNotExpectedResponse(const pldm_msg* request,
                                                       size_t /*requestLen*/)
{
    Response response(sizeof(pldm_msg), 0);
    auto ptr = reinterpret_cast<pldm_msg*>(response.data());
    auto rc = encode_cc_only_resp(request->hdr.instance_id, request->hdr.type,
                                  request->hdr.command,
                                  PLDM_FWUP_COMMAND_NOT_EXPECTED, ptr);
    assert(rc == PLDM_SUCCESS);
    return response;
}

} // namespace fw_update

} // namespace pldm