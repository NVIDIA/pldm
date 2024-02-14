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
    pldmRequest = std::make_unique<sdeventplus::source::Defer>(
        updateManager->event,
        std::bind(&DeviceUpdater::deviceUpdaterHandler, this));
}

void DeviceUpdater::deviceUpdaterHandler()
{
    auto co = startDeviceUpdate();
    deviceUpdaterHandle = co.handle;
}

requester::Coroutine DeviceUpdater::startDeviceUpdate()
{
    const auto& applicableComponents =
        std::get<ApplicableComponents>(fwDeviceIDRecord);
    size_t numComponents = applicableComponents.size();
    auto rc = co_await sendRequestUpdate();
    if (rc)
    {
        lg2::error("Error while sending RequestUpdate.");
        auto rc = co_await sendCancelUpdateRequest();
        if (rc)
        {
            lg2::error("Error while sending CancelUpdate.");
            updateManager->updateDeviceCompletion(eid, false);
            co_return PLDM_ERROR;
        }
        co_return PLDM_ERROR;
    }
    for (size_t compIndex = 0; compIndex < numComponents; compIndex++)
    {
        rc = co_await sendPassCompTableRequest(compIndex);
        if (rc)
        {
            lg2::error("Error while sending PassComponentTable.");
            auto rc = co_await sendCancelUpdateRequest();
            if (rc)
            {
                lg2::error("Error while sending CancelUpdate.");
                updateManager->updateDeviceCompletion(eid, false);
                co_return PLDM_ERROR;
            }
            co_return PLDM_ERROR;
        }
    }
    std::unique_ptr<ComponentUpdater> compUpdater =
        std::make_unique<ComponentUpdater>(
            eid, package, fwDeviceIDRecord, compImageInfos, compInfo,
            compIdNameInfo, maxTransferSize, updateManager, this,
            componentIndex, updateManager->fwDebug);
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
        lg2::error("Error while initiating component updater for "
                   "ComponentIndex={COMPONENTINDEX}.",
                   "COMPONENTINDEX", componentIndex);
        co_return PLDM_ERROR;
    }
    co_return PLDM_SUCCESS;
}

requester::Coroutine DeviceUpdater::sendRequestUpdate()
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
        updateManager->requester.markFree(eid, instanceId);
        lg2::error("encode_request_update_req failed, EID={EID}, RC={RC}",
                   "EID", eid, "RC", rc);
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return rc;
    }

    printBuffer(pldm::utils::Tx, request,
                ("Send RequestUpdate for EID=" + std::to_string(eid)),
                updateManager->fwDebug);
    rc = co_await SendRecvPldmMsgOverMctp(updateManager->handler, eid, request,
                                          &response, &respMsgLen);
    if (rc)
    {
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        lg2::error("Error while sending mctp request for RequestUpdate");
        co_return rc;
    }
    rc = co_await processRequestUpdateResponse(eid, response, respMsgLen);
    if (rc)
    {
        lg2::error("Error while processing RequestUpdateResponse");
    }
    co_return rc;
}

requester::Coroutine DeviceUpdater::processRequestUpdateResponse(
    mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen)
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
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return PLDM_ERROR;
    }

    printBuffer(
        pldm::utils::Rx, response, respMsgLen,
        ("Received requestUpdate Response from EID=" + std::to_string(eid)),
        updateManager->fwDebug);

    uint8_t completionCode = 0;
    uint16_t fdMetaDataLen = 0;
    uint8_t fdWillSendPkgData = 0;

    auto rc = decode_request_update_resp(response, respMsgLen, &completionCode,
                                         &fdMetaDataLen, &fdWillSendPkgData);
    if (rc)
    {
        lg2::error("Decoding RequestUpdate response failed, EID={EID}, RC={RC}",
                   "EID", eid, "RC", rc);
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return PLDM_ERROR;
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
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return PLDM_ERROR;
    }

    deviceUpdaterState.nextState(deviceUpdaterState.current, componentIndex,
                                 numComponents);
    co_return PLDM_SUCCESS;
}

requester::Coroutine DeviceUpdater::sendPassCompTableRequest(size_t offset)
{
    pldmRequest.reset();

    auto instanceId = updateManager->requester.getInstanceId(eid);
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
        updateManager->requester.markFree(eid, instanceId);
        lg2::error(
            "Error: Unable to find the specified component in ComponentInfo");
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
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return rc;
    }

    printBuffer(pldm::utils::Tx, request,
                ("Send PassCompTable for EID=" + std::to_string(eid) +
                 " ,ComponentIndex=" + std::to_string(componentIndex)),
                updateManager->fwDebug);
    rc = co_await SendRecvPldmMsgOverMctp(updateManager->handler, eid, request,
                                          &response, &respMsgLen);
    if (rc)
    {
        lg2::error("Error while sending mctp request for PassCompTable.");
        co_return rc;
    }
    rc = co_await processPassCompTableResponse(eid, response, respMsgLen);
    if (rc)
    {
        lg2::error("Error while processing PassCompTable response.");
    }
    co_return rc;
}

requester::Coroutine DeviceUpdater::processPassCompTableResponse(
    mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen)
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
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return PLDM_ERROR;
    }

    printBuffer(
        pldm::utils::Rx, response, respMsgLen,
        ("Received Response for PassCompTable from EID=" + std::to_string(eid) +
         " ,ComponentIndex=" + std::to_string(componentIndex)),
        updateManager->fwDebug);

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
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return rc;
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
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return PLDM_ERROR;
    }
    if (compResponse)
    {
        lg2::info(
            "In PassComponentTable, componentResponse is non-zero. Component may be updateable EID={EID}, ComponentResponse={CR}, ComponentResponseCode= {CRC}",
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

requester::Coroutine DeviceUpdater::sendActivateFirmwareRequest()
{
    pldmRequest.reset();
    auto instanceId = updateManager->requester.getInstanceId(eid);
    Request request(sizeof(pldm_msg_hdr) +
                    sizeof(struct pldm_activate_firmware_req));
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    const pldm_msg* response = NULL;
    size_t respMsgLen = 0;

    auto rc = encode_activate_firmware_req(
        instanceId, PLDM_NOT_ACTIVATE_SELF_CONTAINED_COMPONENTS, requestMsg,
        sizeof(pldm_activate_firmware_req));
    if (rc)
    {
        updateManager->requester.markFree(eid, instanceId);
        lg2::error("encode_activate_firmware_req failed, EID={EID}, RC={RC}",
                   "EID", eid, "RC", rc);
        co_return rc;
    }

    printBuffer(pldm::utils::Tx, request,
                ("Send ActivateFirmware for EID=" + std::to_string(eid)),
                updateManager->fwDebug);
    rc = co_await SendRecvPldmMsgOverMctp(updateManager->handler, eid, request,
                                          &response, &respMsgLen);
    if (rc)
    {
        lg2::error(
            "Error while sending mctp request for ActivateFirmware. EID={EID}",
            "EID", eid);
        co_return rc;
    }
    rc = co_await processActivateFirmwareResponse(eid, response, respMsgLen);
    if (rc)
    {
        lg2::error("Error while processing ActivateFirmware. EID={EID}", "EID",
                   eid);
    }
    co_return rc;
}

requester::Coroutine DeviceUpdater::processActivateFirmwareResponse(
    mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen)
{
    if (response == nullptr || !respMsgLen)
    {
        // Handle error scenario
        lg2::error("No response received for ActivateFirmware, EID={EID}",
                   "EID", eid);
        updateManager->updateDeviceCompletion(eid, false);
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        const auto& applicableComponents =
            std::get<ApplicableComponents>(fwDeviceIDRecord);
        for (size_t compIndex = 0; compIndex < applicableComponents.size();
             compIndex++)
        {
            updateManager->createMessageRegistry(
                eid, fwDeviceIDRecord, compIndex, activateFailed, "",
                PLDM_ACTIVATE_FIRMWARE, COMMAND_TIMEOUT);
        }
        co_return PLDM_ERROR;
    }

    printBuffer(
        pldm::utils::Rx, response, respMsgLen,
        ("Received ActivateFirmware Response from EID=" + std::to_string(eid)),
        updateManager->fwDebug);

    uint8_t completionCode = 0;
    uint16_t estimatedTimeForActivation = 0;

    // On receiving ActivateFirmware response success/failure make the UA state
    // to Invalid to further not responds to any PLDM Type 5 requests from FD.
    deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);

    auto rc = decode_activate_firmware_resp(
        response, respMsgLen, &completionCode, &estimatedTimeForActivation);
    if (rc)
    {
        // Handle error scenario
        lg2::error(
            "Decoding ActivateFirmware response failed, EID={EID}, RC={RC}",
            "EID", eid, "RC", rc);
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return PLDM_ERROR;
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
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return PLDM_ERROR;
    }

    updateManager->updateDeviceCompletion(eid, true, successCompNames);
    deviceUpdaterState.nextState(deviceUpdaterState.current, componentIndex,
                                 numComponents);
    co_return PLDM_SUCCESS;
}

requester::Coroutine DeviceUpdater::updateComponentCompletion(
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
                componentIndex, updateManager->fwDebug);
        componentUpdaterMap.emplace(
            componentIndex, std::make_pair(std::move(compUpdater), false));
        componentUpdaterMap[componentIndex].first->startComponentUpdater();
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
                    lg2::error("Error while sending ActivateFirmware.");
                    co_return PLDM_ERROR;
                }
                co_return PLDM_SUCCESS;
            }
        }
        // None of the component update is success, cancel the update
        auto rc = co_await sendCancelUpdateRequest();
        if (rc)
        {
            lg2::error("Error while sending CancelUpdate.");
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

requester::Coroutine DeviceUpdater::sendCancelUpdateRequest()
{
    deviceUpdaterState.set(DeviceUpdaterSequence::CancelUpdate);
    auto instanceId = updateManager->requester.getInstanceId(eid);
    Request request(sizeof(pldm_msg_hdr));
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    const pldm_msg* response = NULL;
    size_t respMsgLen = 0;

    auto rc = encode_cancel_update_req(instanceId, requestMsg,
                                       PLDM_CANCEL_UPDATE_REQ_BYTES);
    if (rc)
    {
        updateManager->requester.markFree(eid, instanceId);
        lg2::error("encode_cancel_update_req failed, EID={EID}, RC={RC}", "EID",
                   eid, "RC", rc);
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return rc;
    }

    printBuffer(pldm::utils::Tx, request,
                ("Send CancelUpdate for EID=" + std::to_string(eid)),
                updateManager->fwDebug);
    rc = co_await SendRecvPldmMsgOverMctp(updateManager->handler, eid, request,
                                          &response, &respMsgLen);
    if (rc)
    {
        lg2::error(
            "Error while sending mctp request for CancelUpdate. EID={EID}",
            "EID", eid);
        co_return rc;
    }
    rc = co_await processCancelUpdateResponse(eid, response, respMsgLen);
    if (rc)
    {
        lg2::error("Error while processing CancelUpdate Response. EID={EID}",
                   "EID", eid);
    }
    co_return rc;
}

requester::Coroutine DeviceUpdater::processCancelUpdateResponse(
    mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen)
{
    if (response == nullptr || !respMsgLen)
    {
        // Handle error scenario
        lg2::error("No response received for CancelUpdate, EID={EID}", "EID",
                   eid);
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return PLDM_ERROR;
    }

    printBuffer(
        pldm::utils::Rx, response, respMsgLen,
        ("Received CancelUpdate Response from EID=" + std::to_string(eid)),
        updateManager->fwDebug);

    uint8_t completionCode = 0;
    bool8_t nonFunctioningComponentIndication;
    bitfield64_t nonFunctioningComponentBitmap{0};
    auto rc = decode_cancel_update_resp(response, respMsgLen, &completionCode,
                                        &nonFunctioningComponentIndication,
                                        &nonFunctioningComponentBitmap);
    if (rc)
    {
        lg2::error("Decoding CancelUpdate response failed, EID={EID}, CC={CC}",
                   "EID", eid, "CC", completionCode);
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return rc;
    }
    if (completionCode && completionCode != PLDM_FWUP_NOT_IN_UPDATE_MODE)
    {
        lg2::error(
            "CancelUpdate response failed with error, EID={EID}, CC={CC}",
            "EID", eid, "CC", completionCode);
        deviceUpdaterState.set(DeviceUpdaterSequence::Invalid);
        co_return PLDM_ERROR;
    }
    co_return PLDM_SUCCESS;
}

} // namespace fw_update

} // namespace pldm