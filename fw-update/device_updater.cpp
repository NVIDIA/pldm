#include "device_updater.hpp"

#include "libpldm/firmware_update.h"

#include "activation.hpp"
#include "update_manager.hpp"

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
        std::cerr << "encode_request_update_req failed, EID=" << unsigned(eid)
                  << ", RC=" << rc << "\n";
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
        std::cerr << "Failed to send RequestUpdate request, EID="
                  << unsigned(eid) << ", RC=" << rc << "\n ";
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
            updateManager->createMessageRegistry(eid, fwDeviceIDRecord,
                                                 compIndex,
                                                 updateManager->transferFailed);
        }
        std::cerr << "No response received for RequestUpdate, EID="
                  << unsigned(eid) << "\n";
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
        std::cerr << "Decoding RequestUpdate response failed, EID="
                  << unsigned(eid) << ", RC=" << rc << "\n";
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
            updateManager->createMessageRegistry(eid, fwDeviceIDRecord,
                                                 compIndex,
                                                 updateManager->transferFailed);
#ifdef OEM_NVIDIA
            if (completionCode ==
                static_cast<uint8_t>(
                    oemErrorCodes::RequestFwData::backgroundCopyInProgress))
            {
                std::cerr << "Background copy in progress for EID="
                          << unsigned(eid) << "\n";
                std::string resolution =
                    "Wait for background copy operation to complete. Once the"
                    " operation is complete retry the firmware update operation.";
                std::string messageError = "Background copy in progress";
                updateManager->createMessageRegistryResourceErrors(
                    eid, fwDeviceIDRecord, compIndex,
                    updateManager->resourceErrorDetected, messageError,
                    resolution);
            }
#endif
        }
        std::cerr << "RequestUpdate response failed with error "
                     "completion code, EID="
                  << unsigned(eid) << ", CC=" << unsigned(completionCode)
                  << "\n";
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
        std::cerr << "encode_pass_component_table_req failed, EID="
                  << unsigned(eid) << ", RC=" << rc << "\n";
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
        std::cerr << "Failed to send PassComponentTable request, EID="
                  << unsigned(eid) << ", RC=" << rc << "\n ";
        uaState.set(UASequence::Invalid);
        // Handle error scenario
    }
}

void DeviceUpdater::passCompTable(mctp_eid_t eid, const pldm_msg* response,
                                  size_t respMsgLen)
{
    if (response == nullptr || !respMsgLen)
    {
        updateManager->createMessageRegistry(eid, fwDeviceIDRecord,
                                             componentIndex,
                                             updateManager->transferFailed);
        std::cerr << "No response received for PassComponentTable, EID="
                  << unsigned(eid) << "\n";
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
        std::cerr << "Decoding PassComponentTable response failed, EID="
                  << unsigned(eid) << ", RC=" << rc << "\n";
        uaState.set(UASequence::Invalid);
        return;
    }
    if (completionCode)
    {
        // Handle error scenario
        std::cerr << "PassComponentTable response failed with error "
                     "completion code, EID="
                  << unsigned(eid) << ", CC=" << unsigned(completionCode)
                  << "\n";
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
        std::cerr << "encode_update_component_req failed, EID=" << unsigned(eid)
                  << ", RC=" << rc << "\n";
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
        std::cerr << "Failed to send UpdateComponent request, EID="
                  << unsigned(eid) << ", RC=" << rc << "\n ";
        uaState.set(UASequence::Invalid);
        // Handle error scenario
    }
}

void DeviceUpdater::updateComponent(mctp_eid_t eid, const pldm_msg* response,
                                    size_t respMsgLen)
{
    if (response == nullptr || !respMsgLen)
    {
        updateManager->createMessageRegistry(eid, fwDeviceIDRecord,
                                             componentIndex,
                                             updateManager->transferFailed);
        std::cerr << "No response received for updateComponent, EID="
                  << unsigned(eid) << "\n";
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
        std::cerr << "Decoding UpdateComponent response failed, EID="
                  << unsigned(eid) << ", RC=" << rc << "\n";
        uaState.set(UASequence::Invalid);
        return;
    }
    if (completionCode)
    {
        std::cerr << "UpdateComponent response failed with error "
                     "completion code, EID="
                  << unsigned(eid) << ", CC=" << unsigned(completionCode)
                  << "\n";
        uaState.set(UASequence::Invalid);
        return;
    }

    uaState.nextState(uaState.current, componentIndex, numComponents);

    updateManager->createMessageRegistry(
        eid, fwDeviceIDRecord, componentIndex,
        updateManager->transferringToComponent);
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
        std::cerr << "Decoding RequestFirmwareData request failed, EID="
                  << unsigned(eid) << ", RC=" << rc << "\n";
        rc = encode_request_firmware_data_resp(
            request->hdr.instance_id, PLDM_ERROR_INVALID_DATA, responseMsg,
            sizeof(completionCode));
        if (rc)
        {
            std::cerr << "Encoding RequestFirmwareData response failed, EID="
                      << unsigned(eid) << ", RC=" << rc << "\n";
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
        std::cerr << "EID = " << unsigned(eid)
                  << ", ComponentIndex = " << unsigned(componentIndex)
                  << ", offset = " << unsigned(offset)
                  << ", length = " << unsigned(length) << "\n";
    }

    if (!uaState.expectedState(UASequence::RequestFirmwareData))
    {
        return sendCommandNotExpectedResponse(request, payloadLength);
    }

    if (length < PLDM_FWUP_BASELINE_TRANSFER_SIZE || length > maxTransferSize)
    {
        std::cerr
            << "RequestFirmwareData reported PLDM_FWUP_INVALID_TRANSFER_LENGTH, EID="
            << unsigned(eid) << ", offset=" << offset << ", length" << length
            << "\n";
        rc = encode_request_firmware_data_resp(
            request->hdr.instance_id, PLDM_FWUP_INVALID_TRANSFER_LENGTH,
            responseMsg, sizeof(completionCode));
        if (rc)
        {
            std::cerr << "Encoding RequestFirmwareData response failed, EID="
                      << unsigned(eid) << ", RC=" << rc << "\n";
        }
        return response;
    }

    if (offset + length > compSize + PLDM_FWUP_BASELINE_TRANSFER_SIZE)
    {
        std::cerr
            << "RequestFirmwareData reported PLDM_FWUP_DATA_OUT_OF_RANGE, EID="
            << unsigned(eid) << ", offset=" << offset << ", length" << length
            << "\n";
        rc = encode_request_firmware_data_resp(
            request->hdr.instance_id, PLDM_FWUP_DATA_OUT_OF_RANGE, responseMsg,
            sizeof(completionCode));
        if (rc)
        {
            std::cerr << "Encoding RequestFirmwareData response failed, EID="
                      << unsigned(eid) << ", RC=" << rc << "\n";
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
        std::cerr << "Encoding RequestFirmwareData response failed, EID="
                  << unsigned(eid) << ", RC=" << rc << "\n";
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
        std::cerr << "Decoding TransferComplete request failed, EID="
                  << unsigned(eid) << ", RC=" << rc << "\n";
        rc = encode_transfer_complete_resp(request->hdr.instance_id,
                                           PLDM_ERROR_INVALID_DATA, responseMsg,
                                           sizeof(completionCode));
        if (rc)
        {
            std::cerr << "Encoding TransferComplete response failed, EID="
                      << unsigned(eid) << ", RC=" << rc << "\n";
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
            std::cout << "Component Transfer complete, EID=" << unsigned(eid)
                      << ", COMPONENT_VERSION=" << compVersion << "\n";
        }
        uaState.nextState(uaState.current, componentIndex, numComponents);
    }
    else
    {
        // TransferComplete Failed
        updateManager->createMessageRegistry(eid, fwDeviceIDRecord,
                                             componentIndex,
                                             updateManager->transferFailed);
        std::cerr << "Transfer of the component failed, EID=" << unsigned(eid)
                  << ", COMPONENT_VERSION=" << compVersion
                  << ", TRANSFER_RESULT=" << unsigned(transferResult) << "\n";
#ifdef OEM_NVIDIA
        if (transferResult ==
            static_cast<uint8_t>(
                oemErrorCodes::TransferComplete::reqGrantError))
        {
            std::cerr << "Req/Grant Error for EID=" << unsigned(eid) << "\n";
            std::string resolution =
                "Make sure device AP flash is not accessed by other"
                " application and retry the firmware update operation.";
            std::string messageError = "Req Grant Error";
            updateManager->createMessageRegistryResourceErrors(
                eid, fwDeviceIDRecord, componentIndex,
                updateManager->resourceErrorDetected, messageError, resolution);
        }
        else if (transferResult ==
                 static_cast<uint8_t>(
                     oemErrorCodes::TransferComplete::writeProtectEnabled))
        {
            std::cerr << "Write protect Error for EID=" << unsigned(eid)
                      << "\n";
            std::string resolution =
                "Disable write protect on the device and retry the"
                " firmware update operation.";
            std::string messageError = "Write Protect Enabled";
            updateManager->createMessageRegistryResourceErrors(
                eid, fwDeviceIDRecord, componentIndex,
                updateManager->resourceErrorDetected, messageError, resolution);
        }
#endif
        updateManager->updateDeviceCompletion(eid, false);
        uaState.set(UASequence::Invalid);
    }

    rc = encode_transfer_complete_resp(request->hdr.instance_id, completionCode,
                                       responseMsg, sizeof(completionCode));
    if (rc)
    {
        std::cerr << "Encoding TransferComplete response failed, EID="
                  << unsigned(eid) << ", RC=" << rc << "\n";
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
        std::cerr << "Decoding VerifyComplete request failed, EID="
                  << unsigned(eid) << ", RC=" << rc << "\n";
        rc = encode_verify_complete_resp(request->hdr.instance_id,
                                         PLDM_ERROR_INVALID_DATA, responseMsg,
                                         sizeof(completionCode));
        if (rc)
        {
            std::cerr << "Encoding VerifyComplete response failed, EID="
                      << unsigned(eid) << ", RC=" << rc << "\n";
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
            std::cout << "Component verification complete, EID="
                      << unsigned(eid) << ", COMPONENT_VERSION=" << compVersion
                      << "\n";
        }
        uaState.nextState(uaState.current, componentIndex, numComponents);
    }
    else
    {
        // VerifyComplete Failed
        updateManager->createMessageRegistry(eid, fwDeviceIDRecord,
                                             componentIndex,
                                             updateManager->verificationFailed);
        std::cerr << "Component verification failed, EID=" << unsigned(eid)
                  << ", COMPONENT_VERSION=" << compVersion
                  << ", VERIFY_RESULT=" << unsigned(verifyResult) << "\n";
        updateManager->updateDeviceCompletion(eid, false);
        uaState.set(UASequence::Invalid);
    }

    rc = encode_verify_complete_resp(request->hdr.instance_id, completionCode,
                                     responseMsg, sizeof(completionCode));
    if (rc)
    {
        std::cerr << "Encoding VerifyComplete response failed, EID="
                  << unsigned(eid) << ", RC=" << rc << "\n";
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
        std::cerr << "Decoding ApplyComplete request failed, EID="
                  << unsigned(eid) << ", RC=" << rc << "\n";
        rc = encode_apply_complete_resp(request->hdr.instance_id,
                                        PLDM_ERROR_INVALID_DATA, responseMsg,
                                        sizeof(completionCode));
        if (rc)
        {
            std::cerr << "Encoding ApplyComplete response failed, EID="
                      << unsigned(eid) << ", RC=" << rc << "\n";
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
                                             componentIndex,
                                             updateManager->updateSuccessful);
        if (updateManager->fwDebug)
        {
            std::cout << "Component apply complete, EID=" << unsigned(eid)
                      << ", COMPONENT_VERSION=" << compVersion << "\n";
        }
        updateManager->createMessageRegistry(
            eid, fwDeviceIDRecord, componentIndex,
            updateManager->awaitToActivate,
            updateManager->getActivationMethod(compActivationModification));
    }
    else
    {
        // ApplyComplete Failed
        updateManager->createMessageRegistry(
            eid, fwDeviceIDRecord, componentIndex, updateManager->applyFailed);
        std::cerr << "Component apply failed, EID=" << unsigned(eid)
                  << ", COMPONENT_VERSION=" << compVersion
                  << ", APPLY_RESULT=" << unsigned(applyResult) << "\n";
        updateManager->updateDeviceCompletion(eid, false);
        uaState.set(UASequence::Invalid);
    }

    rc = encode_apply_complete_resp(request->hdr.instance_id, completionCode,
                                    responseMsg, sizeof(completionCode));
    if (rc)
    {
        std::cerr << "Encoding ApplyComplete response failed, EID="
                  << unsigned(eid) << ", RC=" << rc << "\n";
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
        std::cerr << "encode_activate_firmware_req failed, EID="
                  << unsigned(eid) << ", RC=" << rc << "\n";
    }

    printBuffer(pldm::utils::Tx, request,
                ("Send ActivateFirmware for EID=" + std::to_string(eid)));

    rc = updateManager->handler.registerRequest(
        eid, instanceId, PLDM_FWUP, PLDM_ACTIVATE_FIRMWARE, std::move(request),
        std::move(std::bind_front(&DeviceUpdater::activateFirmware, this)));
    if (rc)
    {
        std::cerr << "Failed to send ActivateFirmware request, EID="
                  << unsigned(eid) << ", RC=" << rc << "\n ";
    }
}

void DeviceUpdater::activateFirmware(mctp_eid_t eid, const pldm_msg* response,
                                     size_t respMsgLen)
{
    if (response == nullptr || !respMsgLen)
    {
        // Handle error scenario
        std::cerr << "No response received for ActivateFirmware, EID="
                  << unsigned(eid) << "\n";
        updateManager->updateDeviceCompletion(eid, false);
        uaState.set(UASequence::Invalid);
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
        std::cerr << "Decoding ActivateFirmware response failed, EID="
                  << unsigned(eid) << ", RC=" << rc << "\n";
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
                                                 compIndex,
                                                 updateManager->activateFailed);
        }
        std::cerr << "ActivateFirmware response failed with error "
                     "completion code, EID="
                  << unsigned(eid) << ", CC=" << unsigned(completionCode)
                  << "\n";
        updateManager->updateDeviceCompletion(eid, false);
        return;
    }

    updateManager->updateDeviceCompletion(eid, true);
}

void DeviceUpdater::printBuffer(bool isTx, const std::vector<uint8_t>& buffer,
                                const std::string& message)
{
    if (updateManager->fwDebug)
    {
        std::cout << message << "\n";
        pldm::utils::printBuffer(isTx, buffer);
    }
}

void DeviceUpdater::printBuffer(bool isTx, const pldm_msg* buffer,
                                size_t bufferLen, const std::string& message)
{
    if (updateManager->fwDebug)
    {
        std::cout << message << "\n";
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
            std::cerr << "RequestUpdate timeout EID=" << unsigned(eid)
                      << ", ComponentIndex= " << componentIndex << "\n";
        }
        updateManager->createMessageRegistry(eid, fwDeviceIDRecord,
                                             componentIndex,
                                             updateManager->transferFailed);
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
        std::cerr << "encode_cancel_update_component_req failed, EID="
                  << unsigned(eid) << ", ComponentIndex=" << componentIndex
                  << ", RC=" << rc << "\n";
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
        std::cerr << "Failed to send cancelUpdateComponent request, EID="
                  << unsigned(eid) << ", ComponentIndex=" << componentIndex
                  << "RC=" << rc << "\n ";
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
        std::cerr << "No response received for CancelUpdateComponent, EID="
                  << unsigned(eid) << "\n";
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
        std::cerr << "Decoding CancelUpdateComponent response failed, EID="
                  << unsigned(eid) << ", ComponentIndex=" << componentIndex
                  << ", CC=" << unsigned(completionCode) << "\n";
        uaState.set(UASequence::Invalid);
        return;
    }
    if (completionCode)
    {
        std::cerr << "CancelUpdateComponent response failed with error, "
                     "EID="
                  << unsigned(eid) << ", ComponentIndex=" << componentIndex
                  << ", CC=" << unsigned(completionCode) << "\n";
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