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
#include "inventory_manager.hpp"

#include "common/utils.hpp"
#include "dbusutil.hpp"
#include "fw_update_utility.hpp"
#include "requester/handler.hpp"
#include "xyz/openbmc_project/Software/Version/server.hpp"

#include <phosphor-logging/lg2.hpp>

#include <chrono>
#include <functional>

PHOSPHOR_LOG2_USING;

namespace pldm
{
namespace fw_update
{

void InventoryManager::discoverFDs(const MctpInfos& mctpInfos,
                                   dbus::MctpInterfaces mctpInterfaces)
{
    queuedMctpInfos.emplace(mctpInfos, mctpInterfaces);
    if (discoverFDsTaskHandle.has_value())
    {
        auto& [scope, rcOpt] = *discoverFDsTaskHandle;
        if (!rcOpt.has_value())
        {
            info("Discover FDs already in progress");
            return;
        }
        stdexec::sync_wait(scope.on_empty());
        discoverFDsTaskHandle.reset();
    }

    auto& [scope, rcOpt] = discoverFDsTaskHandle.emplace();
    stdexec::start_detached(
        discoverFDsTask() | stdexec::then([&](int rc) { rcOpt.emplace(rc); }),
        exec::default_task_context<void>(exec::inline_scheduler{}));
}

exec::task<int> InventoryManager::discoverFDsTask()
{
    while (!queuedMctpInfos.empty())
    {
        const auto& [mctpInfos, mctpInterfaces] = queuedMctpInfos.front();
        for (const auto& [eid, uuid, mediumType, networkId, _, bindingType] :
             mctpInfos)
        {
            mctpEidMap[eid] = std::make_tuple(uuid, mediumType, bindingType);
            co_await startFirmwareDiscoveryFlow(eid, mctpInterfaces);
        }
        queuedMctpInfos.pop();
    }

    co_return PLDM_SUCCESS;
}

exec::task<int> InventoryManager::getPLDMTypes(mctp_eid_t eid,
                                               uint64_t& supportedTypes)
{
    auto instanceId = instanceIdDb.next(eid);
    Request request(sizeof(pldm_msg_hdr) + PLDM_GET_TYPES_REQ_BYTES);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    auto rc = encode_get_types_req(instanceId, requestMsg);
    if (rc)
    {
        error("encode_get_types_req failed, eid={EID} rc={RC}.", "EID", eid,
              "RC", rc);
        co_return rc;
    }

    const pldm_msg* responseMsg = nullptr;
    size_t responseLen = 0;

    rc = co_await sendRecvPldmMsgOverMctp(handler, eid, request, &responseMsg,
                                          &responseLen);
    if (rc)
    {
        error("Failed to send GetPLDMTypes request, EID={EID}, RC={RC} ", "EID",
              eid, "RC", rc);
        if (rc == PLDM_REQUESTER_MCTP_TRANSPORT_ERROR)
        {
            handleTransportError(handler, eid, "GetPLDMTypes", PLDM_BASE);
        }
        co_return rc;
    }

    uint8_t completionCode = PLDM_SUCCESS;
    bitfield8_t* types = reinterpret_cast<bitfield8_t*>(&supportedTypes);
    rc =
        decode_get_types_resp(responseMsg, responseLen, &completionCode, types);
    if (rc)
    {
        error("decode_get_types_resp failed, eid={EID} rc={RC}.", "EID", eid,
              "RC", rc);
        co_return rc;
    }
    co_return completionCode;
}

exec::task<int> InventoryManager::startFirmwareDiscoveryFlow(
    mctp_eid_t eid, dbus::MctpInterfaces mctpInterfaces)
{
    uint8_t rc = 0;
    uint64_t supportedTypes = 0;
    rc = co_await getPLDMTypes(eid, supportedTypes);
    if (rc)
    {
        error("getPLDMTypes failed, EID={EID} rc={RC}.", "EID", eid, "RC", rc);
        co_return PLDM_ERROR;
    }

    auto isType5Supported = supportedTypes & (1 << PLDM_FWUP);
    if (!isType5Supported)
    {
        info("Eid {EID} does not support T5", "EID", eid);
        co_return PLDM_SUCCESS;
    }

    uint8_t queryDeviceIdentifiersAttempts = numAttempts;
    uint8_t getFirmwareParametersAttempts = numAttempts;

    std::string messageError{};
    std::string resolution{};

    while (queryDeviceIdentifiersAttempts--)
    {
        rc = co_await queryDeviceIdentifiers(eid, messageError, resolution);

        if (rc == PLDM_SUCCESS)
        {
            break;
        }
        else
        {
            info(
                "Failed to attempt the execute of 'queryDeviceIdentifiers' function., EID={EID}, RC={RC} ",
                "EID", eid, "RC", rc);
        }
    }

    if (rc)
    {
        cleanUpResources(eid);
        error(
            "Failed to execute the 'queryDeviceIdentifiers' function., EID={EID}, RC={RC} ",
            "EID", eid, "RC", rc);
        if (!messageError.empty() && !resolution.empty())
        {
            if (rc == PLDM_ERROR_INVALID_DATA)
            {
                logDiscoveryFailedMessage(eid, messageError, resolution,
                                          mctpInterfaces);
            }
            else
            {
                if (!logDeviceStatusErrors(eid))
                {
                    logDiscoveryFailedMessage(eid, messageError, resolution,
                                              mctpInterfaces);
                }
            }
        }
        co_return rc;
    }

    while (getFirmwareParametersAttempts--)
    {
        rc = co_await getFirmwareParameters(eid, messageError, resolution,
                                            mctpInterfaces);

        if (rc == PLDM_SUCCESS)
        {
            break;
        }
        else
        {
            error("Failed to attempt the execute of 'getFirmwareParameters' "
                  "function., EID={EID}, RC={RC} ",
                  "EID", eid, "RC", rc);
        }
    }

    if (rc)
    {
        cleanUpResources(eid);
        error("Failed to execute the 'getFirmwareParameters' function., "
              "EID={EID}, RC={RC} ",
              "EID", eid, "RC", rc);
        if (!messageError.empty() && !resolution.empty())
        {
            if (rc == PLDM_ERROR_INVALID_DATA)
            {
                logDiscoveryFailedMessage(eid, messageError, resolution,
                                          mctpInterfaces);
            }
            else
            {
                if (!logDeviceStatusErrors(eid))
                {
                    logDiscoveryFailedMessage(eid, messageError, resolution,
                                              mctpInterfaces);
                }
            }
        }
    }

    co_return rc;
}

exec::task<int> InventoryManager::initiateGetActiveFirmwareVersion(
    mctp_eid_t eid, UpdateFWVersionCallBack updateFWVersionCallback)
{
    uint64_t supportedTypes = 0;
    auto rc = co_await getPLDMTypes(eid, supportedTypes);
    if (rc)
    {
        error("getPLDMTypes failed, EID={EID} rc={RC}.", "EID", eid, "RC", rc);
        co_return PLDM_ERROR;
    }

    auto isType5Supported = supportedTypes & (1 << PLDM_FWUP);
    if (!isType5Supported)
    {
        lg2::info("EID={EID} does not support PLDM T5.", "EID", eid);
        co_return PLDM_SUCCESS;
    }

    if (!mctpEidMap.contains(eid))
    {
        co_return PLDM_SUCCESS;
    }

    dbus::MctpInterfaces mctpInterfaces;
    auto co =
        getActiveFirmwareVersion(eid, mctpInterfaces, updateFWVersionCallback);

    co_return PLDM_SUCCESS;
}

exec::task<int> InventoryManager::getActiveFirmwareVersion(
    mctp_eid_t eid, dbus::MctpInterfaces& mctpInterfaces,
    UpdateFWVersionCallBack updateFWVersionCallback)
{
    std::string messageError{};
    std::string resolution{};

    auto rc = co_await getFirmwareParameters(eid, messageError, resolution,
                                             mctpInterfaces, true);

    if (rc == PLDM_SUCCESS)
    {
        if (updateFWVersionCallback)
        {
            updateFWVersionCallback(eid);
        }
        co_return rc;
    }

    cleanUpResources(eid);
    error(
        "Failed to attempt the execute of 'getFirmwareParameters' function., EID={EID}, RC={RC} ",
        "EID", eid, "RC", rc);
    if (!messageError.empty() && !resolution.empty())
    {
        if (rc == PLDM_ERROR_INVALID_DATA)
        {
            logDiscoveryFailedMessage(eid, messageError, resolution,
                                      mctpInterfaces);
        }
        else
        {
            if (!logDeviceStatusErrors(eid))
            {
                logDiscoveryFailedMessage(eid, messageError, resolution,
                                          mctpInterfaces);
            }
        }
    }

    co_return rc;
}

void InventoryManager::cleanUpResources(mctp_eid_t eid)
{
    mctpEidMap.erase(eid);
    descriptorMap.erase(eid);
}

exec::task<int> InventoryManager::queryDeviceIdentifiers(
    mctp_eid_t eid, std::string& messageError, std::string& resolution)
{
    auto instanceId = instanceIdDb.next(eid);
    Request requestMsg(
        sizeof(pldm_msg_hdr) + PLDM_QUERY_DEVICE_IDENTIFIERS_REQ_BYTES);
    auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());
    auto rc = encode_query_device_identifiers_req(
        instanceId, PLDM_QUERY_DEVICE_IDENTIFIERS_REQ_BYTES, request);
    if (rc)
    {
        instanceIdDb.free(eid, instanceId);
        error("encode_query_device_identifiers_req failed, EID={EID}, RC={RC}",
              "EID", eid, "RC", rc);
        co_return rc;
    }

    const pldm_msg* responseMsg = NULL;
    size_t responseLen = 0;

    rc = co_await sendRecvPldmMsgOverMctp(handler, eid, requestMsg,
                                          &responseMsg, &responseLen);

    if (rc)
    {
        error(
            "Failed to send QueryDeviceIdentifiers request, EID={EID}, RC={RC} ",
            "EID", eid, "RC", rc);
        if (rc == PLDM_ERROR_NOT_READY)
        {
            messageError =
                "The device did not respond to the device identifiers request, and the communication timed out.";
            resolution = "Reset the baseboard and retry the operation.";
        }
        else if (rc == PLDM_REQUESTER_MCTP_TRANSPORT_ERROR)
        {
            handleTransportError(handler, eid, "QueryDeviceIdentifiers");
        }
        co_return rc;
    }

    rc = co_await parseQueryDeviceIdentifiersResponse(
        eid, responseMsg, responseLen, messageError, resolution);
    if (rc)
    {
        error(
            "Failed to execute the 'parseQueryDeviceIdentifiersResponse' function., EID={EID}, RC={RC} ",
            "EID", eid, "RC", rc);

        co_return rc;
    }

    co_return rc;
}

exec::task<int> InventoryManager::parseQueryDeviceIdentifiersResponse(
    mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen,
    std::string& messageError, std::string& resolution)
{
    if (response == nullptr || !respMsgLen)
    {
        error(
            "No response received for query device identifiers for endpoint ID {EID}",
            "EID", eid);
        messageError =
            "The device did not respond to the device identifiers request, and the communication timed out.";
        resolution = "Reset the baseboard and retry the operation.";
        co_return PLDM_ERROR;
    }

    uint8_t completionCode = PLDM_SUCCESS;
    uint32_t deviceIdentifiersLen = 0;
    uint8_t descriptorCount = 0;
    uint8_t* descriptorPtr = nullptr;

    auto rc = decode_query_device_identifiers_resp(
        response, respMsgLen, &completionCode, &deviceIdentifiersLen,
        &descriptorCount, &descriptorPtr);
    if (rc)
    {
        error(
            "Failed to decode query device identifiers response for endpoint ID {EID} and descriptor count {DESCRIPTOR_COUNT}, response code {RC}",
            "EID", eid, "DESCRIPTOR_COUNT", descriptorCount, "RC", rc);
        messageError =
            "Received an invalid or corrupted response for the device identifiers request.";
        resolution = "Reset the baseboard and retry the operation.";
        pldm::utils::printBuffer(pldm::utils::Rx, response, respMsgLen);
        co_return PLDM_ERROR_INVALID_DATA;
    }

    if (completionCode)
    {
        error(
            "Failed to query device identifiers response for endpoint ID {EID}, completion code {CC}",
            "EID", eid, "CC", completionCode);
        messageError =
            "Received an invalid or corrupted response for the device identifiers request.";
        resolution = "Reset the baseboard and retry the operation.";
        pldm::utils::printBuffer(pldm::utils::Rx, response, respMsgLen);
        co_return PLDM_ERROR_INVALID_DATA;
    }

    Descriptors descriptors{};
    std::ostringstream descriptorLog{};
    while (descriptorCount-- && (deviceIdentifiersLen > 0))
    {
        uint16_t descriptorType = 0;
        variable_field descriptorData{};

        rc = decode_descriptor_type_length_value(
            descriptorPtr, deviceIdentifiersLen, &descriptorType,
            &descriptorData);
        if (rc)
        {
            error(
                "Failed to decode descriptor type {TYPE}, length {LENGTH} and value for endpoint ID {EID}, response code {RC}",
                "TYPE", descriptorType, "LENGTH", deviceIdentifiersLen, "EID",
                eid, "RC", rc);
            messageError =
                "Received an invalid or corrupted response for the device identifiers request.";
            resolution = "Reset the baseboard and retry the operation.";
            pldm::utils::printBuffer(pldm::utils::Rx, response, respMsgLen);
            co_return PLDM_ERROR_INVALID_DATA;
        }

        if (descriptorType != PLDM_FWUP_VENDOR_DEFINED)
        {
            std::vector<uint8_t> descData(
                descriptorData.ptr, descriptorData.ptr + descriptorData.length);
            std::ostringstream descValueStream{};
            for (const auto& byte : descData)
            {
                descValueStream << std::hex << std::setw(2) << std::setfill('0')
                                << static_cast<int>(byte);
            }
            descriptorLog << "{Type: " << descriptorType
                          << ", Value: " << descValueStream.str() << "}, ";

            descriptors.emplace(descriptorType, std::move(descData));
        }
        else
        {
            uint8_t descriptorTitleStrType = 0;
            variable_field descriptorTitleStr{};
            variable_field vendorDefinedDescriptorData{};

            rc = decode_vendor_defined_descriptor_value(
                descriptorData.ptr, descriptorData.length,
                &descriptorTitleStrType, &descriptorTitleStr,
                &vendorDefinedDescriptorData);
            if (rc)
            {
                error(
                    "Failed to decode vendor-defined descriptor value for endpoint ID {EID}, response code {RC}",
                    "EID", eid, "RC", rc);
                messageError =
                    "Received an invalid or corrupted response for the device identifiers request.";
                resolution = "Reset the baseboard and retry the operation.";
                pldm::utils::printBuffer(pldm::utils::Rx, response, respMsgLen);
                co_return PLDM_ERROR_INVALID_DATA;
            }

            auto vendorDefinedDescriptorTitleStr =
                utils::toString(descriptorTitleStr);
            std::vector<uint8_t> vendorDescData(
                vendorDefinedDescriptorData.ptr,
                vendorDefinedDescriptorData.ptr +
                    vendorDefinedDescriptorData.length);
            descriptors.emplace(descriptorType,
                                std::make_tuple(vendorDefinedDescriptorTitleStr,
                                                vendorDescData));
            std::ostringstream descValueStream{};
            for (const auto& byte : vendorDescData)
            {
                descValueStream << std::hex << std::setw(2) << std::setfill('0')
                                << static_cast<int>(byte);
            }
            descriptorLog << "{Type: " << descriptorType << ", Value: {"
                          << vendorDefinedDescriptorTitleStr << ": "
                          << descValueStream.str() << "}}, ";
        }
        auto nextDescriptorOffset =
            sizeof(pldm_descriptor_tlv().descriptor_type) +
            sizeof(pldm_descriptor_tlv().descriptor_length) +
            descriptorData.length;
        descriptorPtr += nextDescriptorOffset;
        deviceIdentifiersLen -= nextDescriptorOffset;
    }

    if (descriptorMap.contains(eid))
    {
        descriptorMap.erase(eid);
    }
    info("EID={EID} Descriptors=[{DESC}]", "EID", eid, "DESC",
         descriptorLog.str());
    descriptorMap.emplace(eid, std::move(descriptors));

    co_return PLDM_SUCCESS;
}

exec::task<int> InventoryManager::getFirmwareParameters(
    mctp_eid_t eid, std::string& messageError, std::string& resolution,
    dbus::MctpInterfaces& mctpInterfaces, bool refreshFWVersionOnly)
{
    auto instanceId = instanceIdDb.next(eid);
    Request requestMsg(
        sizeof(pldm_msg_hdr) + PLDM_GET_FIRMWARE_PARAMETERS_REQ_BYTES);
    auto request = new (requestMsg.data()) pldm_msg;
    auto rc = encode_get_firmware_parameters_req(
        instanceId, PLDM_GET_FIRMWARE_PARAMETERS_REQ_BYTES, request);
    if (rc)
    {
        instanceIdDb.free(eid, instanceId);
        error(
            "Failed to encode get firmware parameters req for endpoint ID {EID}, response code {RC}",
            "EID", eid, "RC", rc);
        co_return rc;
    }

    const pldm_msg* responseMsg = NULL;
    size_t responseLen = 0;

    rc = co_await sendRecvPldmMsgOverMctp(handler, eid, requestMsg,
                                          &responseMsg, &responseLen);

    if (rc)
    {
        error(
            "Failed to send get firmware parameters request for endpoint ID {EID}, response code {RC}",
            "EID", eid, "RC", rc);
        if (rc == PLDM_ERROR_NOT_READY)
        {
            messageError =
                "The device did not respond to the firmware information request, and the communication timed out.";
            resolution = "Reset the baseboard and retry the operation.";
        }
        else if (rc == PLDM_REQUESTER_MCTP_TRANSPORT_ERROR)
        {
            handleTransportError(handler, eid, "GetFirmwareParameters");
        }
        co_return rc;
    }

    rc = co_await parseGetFWParametersResponse(
        eid, responseMsg, responseLen, messageError, resolution, mctpInterfaces,
        refreshFWVersionOnly);

    if (rc)
    {
        error("parseGetFWParametersResponse failed, EID={EID}, RC={RC} ", "EID",
              eid, "RC", rc);
    }

    co_return rc;
}

exec::task<int> InventoryManager::parseGetFWParametersResponse(
    mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen,
    std::string& messageError, std::string& resolution,
    dbus::MctpInterfaces& mctpInterfaces, bool refreshFWVersionOnly)
{
    if (response == nullptr || !respMsgLen)
    {
        error(
            "No response received for get firmware parameters for endpoint ID {EID}",
            "EID", eid);
        messageError =
            "The device did not respond to the firmware information request, and the communication timed out.";
        resolution = "Reset the baseboard and retry the operation.";
        co_return PLDM_ERROR;
    }

    pldm_get_firmware_parameters_resp fwParams{};
    variable_field activeCompImageSetVerStr{};
    variable_field pendingCompImageSetVerStr{};
    variable_field compParamTable{};

    auto rc = decode_get_firmware_parameters_resp(
        response, respMsgLen, &fwParams, &activeCompImageSetVerStr,
        &pendingCompImageSetVerStr, &compParamTable);
    if (rc)
    {
        error(
            "Failed to decode get firmware parameters response for endpoint ID {EID}, response code {RC}",
            "EID", eid, "RC", rc);
        messageError =
            "Received an invalid or corrupted response for the firmware information request.";
        resolution = "Reset the baseboard and retry the operation.";
        pldm::utils::printBuffer(pldm::utils::Rx, response, respMsgLen);
        co_return PLDM_ERROR_INVALID_DATA;
    }

    if (fwParams.completion_code)
    {
        auto fw_param_cc = fwParams.completion_code;
        error(
            "Failed to get firmware parameters response for endpoint ID {EID}, completion code {CC}",
            "EID", eid, "CC", fw_param_cc);
        messageError =
            "Received an invalid or corrupted response for the firmware information request.";
        resolution = "Reset the baseboard and retry the operation.";
        pldm::utils::printBuffer(pldm::utils::Rx, response, respMsgLen);
        co_return PLDM_ERROR_INVALID_DATA;
    }

    auto compParamPtr = compParamTable.ptr;
    auto compParamTableLen = compParamTable.length;
    pldm_component_parameter_entry compEntry{};
    variable_field activeCompVerStr{};
    variable_field pendingCompVerStr{};

    ComponentInfo componentInfo{};
    std::ostringstream paramsLog{};

    while (fwParams.comp_count-- && (compParamTableLen > 0))
    {
        auto rc = decode_get_firmware_parameters_resp_comp_entry(
            compParamPtr, compParamTableLen, &compEntry, &activeCompVerStr,
            &pendingCompVerStr);
        if (rc)
        {
            error(
                "Failed to decode component parameter table entry for endpoint ID {EID}, response code {RC}",
                "EID", eid, "RC", rc);
            messageError =
                "Device responded with invalid or corrupted response for GetFirmwareParameters request";
            resolution = "Reset the baseboard and retry the operation.";
            pldm::utils::printBuffer(pldm::utils::Rx, response, respMsgLen);
            co_return PLDM_ERROR_INVALID_DATA;
        }

        auto compClassification = compEntry.comp_classification;
        auto compIdentifier = compEntry.comp_identifier;
        auto compActivationMethods = compEntry.comp_activation_methods.value;
        componentInfo.emplace(
            std::make_pair(compClassification, compIdentifier),
            std::make_tuple(compEntry.comp_classification_index,
                            utils::toString(activeCompVerStr),
                            compActivationMethods));

        paramsLog
            << "{Classification: " << static_cast<int>(compClassification)
            << ", ID: " << compIdentifier << ", Index: "
            << static_cast<int>(compEntry.comp_classification_index)
            << ", Active Version: " << utils::toString(activeCompVerStr)
            << ", ActiveCompStamp: " << compEntry.active_comp_comparison_stamp
            << ", Pending Version: " << utils::toString(pendingCompVerStr)
            << ", PendingCompStamp: " << compEntry.pending_comp_comparison_stamp
            << ", ActivationMethods: 0x" << std::hex << compActivationMethods
            << std::dec << "}, ";

        compParamPtr += sizeof(pldm_component_parameter_entry) +
                        activeCompVerStr.length + pendingCompVerStr.length;
        compParamTableLen -= sizeof(pldm_component_parameter_entry) +
                             activeCompVerStr.length + pendingCompVerStr.length;
    }
    paramsLog << "]";
    lg2::info("EID={EID} FW Parameters: {PARAMS}", "EID", eid, "PARAMS",
              paramsLog.str());

    if (componentInfoMap.contains(eid))
    {
        componentInfoMap.erase(eid);
    }
    componentInfoMap.emplace(eid, std::move(componentInfo));

    // If there are multiple endpoints associated with the same device, then
    // based on a policy one MCTP endpoint is picked for firmware update, the
    // remaining endpoints are cleared from DescriptorMap and ComponentInfoMap
    // The default policy is to pick the MCTP endpoint where the outgoing
    // physical medium is the fastest. Skip firmware/device inventory for the
    // next endpoints after discovering the first endpoint associated with the
    // UUID. The logic to calculate fastest EID to the PLDM FD is not
    // needed when FW versions are refreshed.
    if (mctpEidMap.contains(eid) && !refreshFWVersionOnly)
    {
        const auto& [uuid, mediumType, bindingType] = mctpEidMap[eid];
        // This condition is met, if an additional eid is discovered for a
        // device(same UUID) that is already discovered.
        if (mctpInfoMap.contains(uuid))
        {
            auto search = mctpInfoMap.find(uuid);

            const auto& curTop = search->second.top();
            auto curFastestEid = curTop.eid;
            // Check if eid is already the fastest, this can happen on a
            // rediscovery of the MCTP endpoint
            if (curFastestEid == eid)
            {
                info("Fastest path to UUID={UUID} is already set to EID={EID}",
                     "UUID", uuid, "EID", eid);
                // WAR: For when an already discovered EID is processed again
                // from an InterfacesAdded signal
                if (updateInventoryCallBack)
                {
                    updateInventoryCallBack(eid, uuid, mctpInterfaces);
                }
                co_return PLDM_SUCCESS;
            }

            // Insert eid into priority queue, to identify the new fastest EID
            search->second.push({eid, mediumType, bindingType});

            const auto& newTop = search->second.top();
            auto newFastestEid = newTop.eid;
            // Check if eid is the fastest eid after comparison
            if (eid != newFastestEid)
            {
                info(
                    "Fastest path to UUID={UUID} is set to EID={EID}, removed DELETED_EID={DELETED_EID}",
                    "UUID", uuid, "EID", newFastestEid, "DELETED_EID", eid);
                descriptorMap.erase(eid);
                componentInfoMap.erase(eid);
            }
            else if (eid == newFastestEid)
            {
                info(
                    "Fastest path to UUID={UUID} is set to EID={EID}, DELETED_EID={DELETED_EID}",
                    "UUID", uuid, "EID", newFastestEid, "DELETED_EID",
                    curFastestEid);
                descriptorMap.erase(curFastestEid);
                componentInfoMap.erase(curFastestEid);
            }

            // Trim priority queue to have only the fastest eid, remove the
            // second entry.
            const auto& currTop = search->second.top();
            auto topEID = currTop.eid;
            auto topMediumType = currTop.medium;
            auto topBindingType = currTop.binding;
            search->second.pop();
            search->second.pop();
            search->second.push({topEID, topMediumType, topBindingType});
        }
        else
        {
            std::priority_queue<MctpEidInfo> mctpEidInfo;
            mctpEidInfo.push({eid, mediumType, bindingType});
            mctpInfoMap.emplace(uuid, std::move(mctpEidInfo));
            if (createInventoryCallBack)
            {
                createInventoryCallBack(eid, uuid, mctpInterfaces);
            }
        }
    }

    co_return PLDM_SUCCESS;
}

sdbusplus::async::task<int> InventoryManager::queryDownstreamDevices(
    mctp_eid_t eid)
{
    Request requestMsg(sizeof(pldm_msg_hdr));
    auto instanceId = instanceIdDb.next(eid);
    auto request = new (requestMsg.data()) pldm_msg;
    auto rc = encode_query_downstream_devices_req(instanceId, request);

    if (rc)
    {
        instanceIdDb.free(eid, instanceId);
        error(
            "Failed to encoude QueryDownstreamDevices request for endpoint ID {EID}, response code {RC}",
            "EID", eid, "RC", rc);
        co_return rc;
    }

    const pldm_msg* responseMsg = NULL;
    size_t responseLen = 0;

    rc = co_await sendRecvPldmMsgOverMctp(handler, eid, requestMsg,
                                          &responseMsg, &responseLen);
    if (rc)
    {
        error(
            "Failed to send QueryDownstreamDevices request for endpoint ID {EID}, response code {RC}",
            "EID", eid, "RC", rc);
        if (rc == PLDM_REQUESTER_MCTP_TRANSPORT_ERROR)
        {
            handleTransportError(handler, eid, "QueryDownstreamDevices");
        }
        co_return rc;
    }

    rc = co_await parseQueryDownstreamDevicesResponse(eid, responseMsg,
                                                      responseLen);

    if (rc)
    {
        error("parseQueryDownstreamDeviceResponse failed, EID={EID}, RC={RC} ",
              "EID", eid, "RC", rc);
    }

    co_return rc;
}

sdbusplus::async::task<int>
    InventoryManager::parseQueryDownstreamDevicesResponse(
        mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen)
{
    if (!response || !respMsgLen)
    {
        error(
            "No response received for QueryDownstreamDevices for endpoint ID {EID}",
            "EID", eid);
        co_return PLDM_ERROR;
    }

    pldm_query_downstream_devices_resp downstreamDevicesResp{};
    auto rc = decode_query_downstream_devices_resp(response, respMsgLen,
                                                   &downstreamDevicesResp);
    if (rc)
    {
        error(
            "Decoding QueryDownstreamDevices response failed for endpoint ID {EID} with response code {RC}",
            "EID", eid, "RC", rc);
        co_return PLDM_ERROR;
    }

    switch (downstreamDevicesResp.completion_code)
    {
        case PLDM_SUCCESS:
            break;
        case PLDM_ERROR_UNSUPPORTED_PLDM_CMD:
            /* QueryDownstreamDevices is optional, consider the device does not
             * support Downstream Devices.
             */
            info("Endpoint ID {EID} does not support QueryDownstreamDevices",
                 "EID", eid);
            co_return PLDM_ERROR;
        default:
            error(
                "QueryDownstreamDevices response failed with error completion code for endpoint ID {EID} with completion code {CC}",
                "EID", eid, "CC", downstreamDevicesResp.completion_code);
            co_return PLDM_ERROR;
    }

    error("DownstreamDevicesResp.downstream_device_update_supported: {X}", "X",
          downstreamDevicesResp.downstream_device_update_supported);
    error("PLDM_FWUP_DOWNSTREAM_DEVICE_UPDATE_SUPPORTED: {X}", "X",
          PLDM_FWUP_DOWNSTREAM_DEVICE_UPDATE_SUPPORTED);
    error("PLDM_FWUP_DOWNSTREAM_DEVICE_UPDATE_NOT_SUPPORTED: {X}", "X",
          PLDM_FWUP_DOWNSTREAM_DEVICE_UPDATE_NOT_SUPPORTED);
    switch (downstreamDevicesResp.downstream_device_update_supported)
    {
        case PLDM_FWUP_DOWNSTREAM_DEVICE_UPDATE_SUPPORTED:
        {
            /** DataTransferHandle will be skipped when TransferOperationFlag is
             *  `GetFirstPart`. Use 0x0 as default by following example in
             *  Figure 9 in DSP0267 1.1.0
             */
            auto rc = co_await queryDownstreamIdentifiers(eid, 0x0,
                                                          PLDM_GET_FIRSTPART);
            if (rc)
            {
                error(
                    "Failed to send QueryDownstreamIdentifiers request for endpoint ID {EID}",
                    "EID", eid);
            }
            break;
        }
        case PLDM_FWUP_DOWNSTREAM_DEVICE_UPDATE_NOT_SUPPORTED:
            /* The FDP does not support firmware updates but may report
             * inventory information on downstream devices.
             * In this scenario, sends only GetDownstreamFirmwareParameters
             * to the FDP.
             * The definition can be found at Table 15 of DSP0267_1.1.0
             */
            break;
        default:
            error(
                "Unknown response of DownstreamDeviceUpdateSupported from endpoint ID {EID} with value {VALUE}",
                "EID", eid, "VALUE",
                downstreamDevicesResp.downstream_device_update_supported);
            co_return PLDM_ERROR;
    }
    co_return PLDM_SUCCESS;
}

sdbusplus::async::task<int> InventoryManager::queryDownstreamIdentifiers(
    mctp_eid_t eid, uint32_t dataTransferHandle,
    enum transfer_op_flag transferOperationFlag)
{
    auto instanceId = instanceIdDb.next(eid);
    Request requestMsg(
        sizeof(pldm_msg_hdr) + PLDM_QUERY_DOWNSTREAM_IDENTIFIERS_REQ_BYTES);
    auto request = new (requestMsg.data()) pldm_msg;
    pldm_query_downstream_identifiers_req requestParameters{
        dataTransferHandle, static_cast<uint8_t>(transferOperationFlag)};

    auto rc = encode_query_downstream_identifiers_req(
        instanceId, &requestParameters, request,
        PLDM_QUERY_DOWNSTREAM_IDENTIFIERS_REQ_BYTES);
    if (rc)
    {
        instanceIdDb.free(eid, instanceId);
        error(
            "Failed to encode query downstream identifiers request for endpoint ID {EID} with response code {RC}",
            "EID", eid, "RC", rc);
        co_return rc;
    }

    const pldm_msg* responseMsg = NULL;
    size_t responseLen = 0;

    rc = co_await sendRecvPldmMsgOverMctp(handler, eid, requestMsg,
                                          &responseMsg, &responseLen);
    if (rc)
    {
        error(
            "Failed to send QueryDownstreamIdentifiers request for endpoint ID {EID} with response code {RC}",
            "EID", eid, "RC", rc);
        if (rc == PLDM_REQUESTER_MCTP_TRANSPORT_ERROR)
        {
            handleTransportError(handler, eid, "QueryDownstreamIdentifiers");
        }
        co_return rc;
    }

    rc = co_await parseQueryDownstreamIdentifiersResponse(
        eid, responseMsg, responseLen);

    if (rc)
    {
        error(
            "parseQueryDownstreamIdentifiersResponse failed, EID={EID}, RC={RC} ",
            "EID", eid, "RC", rc);
    }
    co_return rc;
}

sdbusplus::async::task<int>
    InventoryManager::parseQueryDownstreamIdentifiersResponse(
        mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen)
{
    if (!response || !respMsgLen)
    {
        error(
            "No response received for QueryDownstreamIdentifiers for endpoint ID {EID}",
            "EID", eid);
        descriptorMap.erase(eid);
        co_return PLDM_ERROR;
    }

    pldm_query_downstream_identifiers_resp downstreamIds{};
    pldm_downstream_device_iter devs{};

    auto rc = decode_query_downstream_identifiers_resp(response, respMsgLen,
                                                       &downstreamIds, &devs);
    if (rc)
    {
        error(
            "Decoding QueryDownstreamIdentifiers response failed for endpoint ID {EID} with response code {RC}",
            "EID", eid, "RC", rc);
        co_return PLDM_ERROR;
    }

    if (downstreamIds.completion_code)
    {
        error(
            "QueryDownstreamIdentifiers response failed with error completion code for endpoint ID {EID} with completion code {CC}",
            "EID", eid, "CC", unsigned(downstreamIds.completion_code));
        co_return PLDM_ERROR;
    }

    DownstreamDeviceInfo initialDownstreamDevices{};
    DownstreamDeviceInfo* downstreamDevices;
    if (!downstreamDescriptorMap.contains(eid) ||
        downstreamIds.transfer_flag == PLDM_START ||
        downstreamIds.transfer_flag == PLDM_START_AND_END)
    {
        downstreamDevices = &initialDownstreamDevices;
    }
    else
    {
        downstreamDevices = &downstreamDescriptorMap.at(eid);
    }

    pldm_downstream_device dev;
    foreach_pldm_downstream_device(devs, dev, rc)
    {
        pldm_descriptor desc;
        Descriptors descriptors{};
        foreach_pldm_downstream_device_descriptor(devs, dev, desc, rc)
        {
            const auto descriptorData =
                new (const_cast<void*>(desc.descriptor_data))
                    uint8_t[desc.descriptor_length];
            if (desc.descriptor_type != PLDM_FWUP_VENDOR_DEFINED)
            {
                std::vector<uint8_t> descData(
                    descriptorData, descriptorData + desc.descriptor_length);
                descriptors.emplace(desc.descriptor_type, std::move(descData));
            }
            else
            {
                uint8_t descriptorTitleStrType = 0;
                variable_field descriptorTitleStr{};
                variable_field vendorDefinedDescriptorData{};

                rc = decode_vendor_defined_descriptor_value(
                    descriptorData, desc.descriptor_length,
                    &descriptorTitleStrType, &descriptorTitleStr,
                    &vendorDefinedDescriptorData);

                if (rc)
                {
                    error(
                        "Decoding Vendor-defined descriptor value failed for endpoint ID {EID} with response code {RC}",
                        "EID", eid, "RC", rc);
                    co_return PLDM_ERROR;
                }

                auto vendorDefinedDescriptorTitleStr =
                    utils::toString(descriptorTitleStr);
                std::vector<uint8_t> vendorDescData(
                    vendorDefinedDescriptorData.ptr,
                    vendorDefinedDescriptorData.ptr +
                        vendorDefinedDescriptorData.length);
                descriptors.emplace(
                    desc.descriptor_type,
                    std::make_tuple(vendorDefinedDescriptorTitleStr,
                                    vendorDescData));
            }
        }
        if (rc)
        {
            error(
                "Failed to decode downstream device descriptor for endpoint ID {EID} with response code {RC}",
                "EID", eid, "RC", rc);
            co_return PLDM_ERROR;
        }
        downstreamDevices->emplace(dev.downstream_device_index, descriptors);
    }
    if (rc)
    {
        error(
            "Failed to decode downstream devices from iterator for endpoint ID {EID} with response code {RC}",
            "EID", eid, "RC", rc);
        co_return PLDM_ERROR;
    }

    switch (downstreamIds.transfer_flag)
    {
        case PLDM_START:
            downstreamDescriptorMap.insert_or_assign(
                eid, std::move(initialDownstreamDevices));
            [[fallthrough]];
        case PLDM_MIDDLE:
        {
            auto rc = co_await queryDownstreamIdentifiers(
                eid, downstreamIds.next_data_transfer_handle,
                PLDM_GET_NEXTPART);
            if (rc)
            {
                error(
                    "Failed to send QueryDownstreamIdentifiers request for endpoint ID {EID}",
                    "EID", eid);
            }
            break;
        }
        case PLDM_START_AND_END:
            downstreamDescriptorMap.insert_or_assign(
                eid, std::move(initialDownstreamDevices));
            /** DataTransferHandle will be skipped when TransferOperationFlag is
             *  `GetFirstPart`. Use 0x0 as default by following example in
             *  Figure 9 in DSP0267 1.1.0
             */
            [[fallthrough]];
        case PLDM_END:
        {
            auto rc = co_await getDownstreamFirmwareParameters(
                eid, 0x0, PLDM_GET_FIRSTPART);
            if (rc)
            {
                error(
                    "Failed to send GetDownstreamFirmwareParameters request for endpoint ID {EID}",
                    "EID", eid);
            }

            break;
        }
    }
    co_return PLDM_SUCCESS;
}

sdbusplus::async::task<int> InventoryManager::getDownstreamFirmwareParameters(
    mctp_eid_t eid, uint32_t dataTransferHandle,
    enum transfer_op_flag transferOperationFlag)
{
    Request requestMsg(sizeof(pldm_msg_hdr) +
                       PLDM_GET_DOWNSTREAM_FIRMWARE_PARAMETERS_REQ_BYTES);
    auto instanceId = instanceIdDb.next(eid);
    auto request = new (requestMsg.data()) pldm_msg;
    pldm_get_downstream_firmware_parameters_req requestParameters{
        dataTransferHandle, static_cast<uint8_t>(transferOperationFlag)};
    auto rc = encode_get_downstream_firmware_parameters_req(
        instanceId, &requestParameters, request,
        PLDM_GET_DOWNSTREAM_FIRMWARE_PARAMETERS_REQ_BYTES);
    if (rc)
    {
        instanceIdDb.free(eid, instanceId);
        error(
            "Failed to encode query downstream firmware parameters request for endpoint ID {EID} with response code {RC}",
            "EID", eid, "RC", rc);
        co_return rc;
    }

    const pldm_msg* responseMsg = NULL;
    size_t responseLen = 0;

    rc = co_await sendRecvPldmMsgOverMctp(handler, eid, requestMsg,
                                          &responseMsg, &responseLen);
    if (rc)
    {
        error(
            "Failed to send GetDownstreamFirmwareParameters request for endpoint ID {EID}, response code {RC}",
            "EID", eid, "RC", rc);
        if (rc == PLDM_REQUESTER_MCTP_TRANSPORT_ERROR)
        {
            handleTransportError(handler, eid,
                                 "QueryDownstreamFirmwareParameters");
        }
        co_return rc;
    }

    rc = co_await parseGetDownstreamFirmwareParametersResponse(
        eid, responseMsg, responseLen);

    if (rc)
    {
        error(
            "parseGetDownstreamFirmwareParametersResponse failed, EID={EID}, RC={RC} ",
            "EID", eid, "RC", rc);
    }
    co_return rc;
}

sdbusplus::async::task<int>
    InventoryManager::parseGetDownstreamFirmwareParametersResponse(
        mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen)
{
    if (!response || !respMsgLen)
    {
        error(
            "No response received for QueryDownstreamFirmwareParameters for endpoint ID {EID}",
            "EID", eid);
        descriptorMap.erase(eid);
        co_return PLDM_ERROR;
    }

    pldm_get_downstream_firmware_parameters_resp resp{};
    pldm_downstream_device_parameters_iter params{};
    pldm_downstream_device_parameters_entry entry{};

    auto rc = decode_get_downstream_firmware_parameters_resp(
        response, respMsgLen, &resp, &params);

    if (rc)
    {
        error(
            "Decoding QueryDownstreamFirmwareParameters response failed for endpoint ID {EID} with response code {RC}",
            "EID", eid, "RC", rc);
        co_return PLDM_ERROR;
    }

    if (resp.completion_code)
    {
        error(
            "QueryDownstreamFirmwareParameters response failed with error completion code for endpoint ID {EID} with completion code {CC}",
            "EID", eid, "CC", resp.completion_code);
        co_return PLDM_ERROR;
    }

    foreach_pldm_downstream_device_parameters_entry(params, entry, rc)
    {
        // Reserved for upcoming use
        [[maybe_unused]] variable_field activeCompVerStr{
            reinterpret_cast<const uint8_t*>(entry.active_comp_ver_str),
            entry.active_comp_ver_str_len};
    }
    if (rc)
    {
        error(
            "Failed to decode downstream device parameters from iterator for endpoint ID {EID} with response code {RC}",
            "EID", eid, "RC", rc);
        co_return PLDM_ERROR;
    }

    switch (resp.transfer_flag)
    {
        case PLDM_START:
        case PLDM_MIDDLE:
        {
            auto rc = co_await getDownstreamFirmwareParameters(
                eid, resp.next_data_transfer_handle, PLDM_GET_NEXTPART);
            if (rc)
            {
                error(
                    "Failed to send GetDownstreamFirmwareParameters request for endpoint ID {EID}",
                    "EID", eid);
            }
            break;
        }
    }
    co_return PLDM_SUCCESS;
}

bool InventoryManager::logDeviceStatusErrors(const mctp_eid_t eid,
                                             bool overrideSeverity,
                                             const std::string& logNamespace)
{
    auto errorInfos = queryDeviceStatusError(eid);
    if (errorInfos.empty())
    {
        return false;
    }

    for (const auto& errorInfo : errorInfos)
    {
        createLogEntry(errorInfo.messageId, errorInfo.arg0, errorInfo.arg1, "",
                       logNamespace, overrideSeverity);
    }
    return true;
}

void InventoryManager::logDiscoveryFailedMessage(
    const mctp_eid_t eid, const std::string& messageError,
    const std::string& resolution, dbus::MctpInterfaces mctpInterfaces,
    const std::string& logNamespace, bool forceInformational)
{
    if (mctpEidMap.contains(eid))
    {
        const auto& [uuid, mediumType, bindingType] = mctpEidMap[eid];
        DeviceInfo deviceInfo;
        if (deviceInventoryInfo.matchInventoryEntry(mctpInterfaces[uuid],
                                                    deviceInfo))
        {
            const auto& deviceObjPath =
                std::get<DeviceObjPath>(std::get<CreateDeviceInfo>(deviceInfo));
            std::string compName =
                std::filesystem::path(deviceObjPath).filename();
            createLogEntry(resourceErrorDetected, compName, messageError,
                           resolution, logNamespace, forceInformational);
        }
    }
}

exec::task<int> InventoryManager::refreshFirmwareInventory(
    const std::vector<mctp_eid_t>& eids, dbus::MctpInterfaces& mctpInterfaces,
    const ComponentTargetList& compTargetList)
{
    info("Refreshing firmware inventory for {COUNT} endpoints", "COUNT",
         eids.size());

    int overallRc = PLDM_SUCCESS;
    for (const auto& eid : eids)
    {
        std::string messageError{};
        // discoveryResolution captures the resolution message from discovery
        // functions (queryDeviceIdentifiers/getFirmwareParameters). It is not
        // used because FW Update operations require a specific resolution
        // message defined in `resolution` below.
        std::string discoveryResolution{};
        std::string resolution{
            "Retry firmware update operation, if problem persists, follow FW upgrade recovery flow."};

        auto isTarget = compTargetList.contains(eid);

        auto rc = co_await queryDeviceIdentifiers(eid, messageError,
                                                  discoveryResolution);
        if (rc != PLDM_SUCCESS)
        {
            if (rc == PLDM_ERROR_INVALID_DATA or
                !logDeviceStatusErrors(eid, !isTarget, "FWUpdate"))
            {
                if (isTarget)
                {
                    error(
                        "Failed to refresh descriptors for target endpoint ID {EID}, RC={RC}",
                        "EID", eid, "RC", rc);
                    logDiscoveryFailedMessage(eid, messageError, resolution,
                                              mctpInterfaces, "FWUpdate",
                                              false);
                }
                else
                {
                    warning(
                        "Failed to refresh descriptors for endpoint ID {EID}, RC={RC}",
                        "EID", eid, "RC", rc);
                    logDiscoveryFailedMessage(eid, messageError, resolution,
                                              mctpInterfaces, "FWUpdate", true);
                }
            }
            descriptorMap.erase(eid);
            componentInfoMap.erase(eid);
            overallRc = PLDM_ERROR;
            continue;
        }

        rc = co_await getFirmwareParameters(
            eid, messageError, discoveryResolution, mctpInterfaces, true);
        if (rc != PLDM_SUCCESS)
        {
            if (rc == PLDM_ERROR_INVALID_DATA or
                !logDeviceStatusErrors(eid, !isTarget, "FWUpdate"))
            {
                logDiscoveryFailedMessage(eid, messageError, resolution,
                                          mctpInterfaces, "FWUpdate",
                                          !isTarget);
                if (isTarget)
                {
                    error(
                        "Failed to refresh firmware parameters for target endpoint ID {EID}, RC={RC}",
                        "EID", eid, "RC", rc);
                }
                else
                {
                    warning(
                        "Failed to refresh firmware parameters for endpoint ID {EID}, RC={RC}",
                        "EID", eid, "RC", rc);
                }
            }
            descriptorMap.erase(eid);
            componentInfoMap.erase(eid);
            overallRc = PLDM_ERROR;
            continue;
        }

        if (updateInventoryCallBack && mctpEidMap.contains(eid))
        {
            const auto& [uuid, mediumType, bindingType] = mctpEidMap[eid];
            info("Updating inventory for endpoint ID {EID} with UUID {UUID}",
                 "EID", eid, "UUID", uuid);
            updateInventoryCallBack(eid, uuid, mctpInterfaces);
        }

        info("Successfully refreshed firmware inventory for endpoint ID {EID}",
             "EID", eid);
    }

    co_return overallRc;
}

} // namespace fw_update

} // namespace pldm
