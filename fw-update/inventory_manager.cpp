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

#include "libpldm/firmware_update.h"

#include "common/utils.hpp"
#include "dbusutil.hpp"
#include "xyz/openbmc_project/Software/Version/server.hpp"

#include <phosphor-logging/lg2.hpp>

#include <functional>

namespace pldm
{

namespace fw_update
{

void InventoryManager::discoverFDs(const MctpInfos& mctpInfos,
                                   dbus::MctpInterfaces& mctpInterfaces)
{
    for (const auto& [eid, uuid, mediumType, networkId, bindingType] :
         mctpInfos)
    {
        mctpEidMap[eid] = std::make_tuple(uuid, mediumType, bindingType);
        auto co = startFirmwareDiscoveryFlow(eid, mctpInterfaces);

        if (inventoryCoRoutineHandlers.contains(eid))
        {
            inventoryCoRoutineHandlers[eid].destroy();
            inventoryCoRoutineHandlers[eid] = co.handle;
        }
        else
        {
            inventoryCoRoutineHandlers.emplace(eid, co.handle);
        }
    }
}

requester::Coroutine InventoryManager::getPLDMTypes(mctp_eid_t eid,
                                                    uint64_t& supportedTypes)
{
    auto instanceId = requester.getInstanceId(eid);
    Request request(sizeof(pldm_msg_hdr) + PLDM_GET_TYPES_REQ_BYTES);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    auto rc = encode_get_types_req(instanceId, requestMsg);
    if (rc)
    {
        lg2::error("encode_get_types_req failed, eid={EID} rc={RC}.", "EID",
                   eid, "RC", rc);
        co_return rc;
    }

    const pldm_msg* responseMsg = nullptr;
    size_t responseLen = 0;

    rc = co_await SendRecvPldmMsgOverMctp(handler, eid, request, &responseMsg,
                                          &responseLen);
    if (rc)
    {
        lg2::error("Failed to send GetPLDMTypes request, EID={EID}, RC={RC} ",
                   "EID", eid, "RC", rc);
        co_return rc;
    }

    uint8_t completionCode = PLDM_SUCCESS;
    bitfield8_t* types = reinterpret_cast<bitfield8_t*>(&supportedTypes);
    rc =
        decode_get_types_resp(responseMsg, responseLen, &completionCode, types);
    if (rc)
    {
        lg2::error("decode_get_types_resp failed, eid={EID} rc={RC}.", "EID",
                   eid, "RC", rc);
        co_return rc;
    }
    co_return completionCode;
}

requester::Coroutine InventoryManager::startFirmwareDiscoveryFlow(
    mctp_eid_t eid, dbus::MctpInterfaces mctpInterfaces)
{
    uint8_t rc = 0;
    uint64_t supportedTypes = 0;
    rc = co_await getPLDMTypes(eid, supportedTypes);
    if (rc)
    {
        lg2::error("getPLDMTypes failed, EID={EID} rc={RC}.", "EID", eid, "RC",
                   rc);
        co_return PLDM_ERROR;
    }

    auto isType5Supported = supportedTypes & (1 << PLDM_FWUP);
    if (!isType5Supported)
    {
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
            lg2::info(
                "Failed to attempt the execute of 'queryDeviceIdentifiers' function., EID={EID}, RC={RC} ",
                "EID", eid, "RC", rc);
        }
    }

    if (rc)
    {
        cleanUpResources(eid);
        lg2::error(
            "Failed to execute the 'queryDeviceIdentifiers' function., EID={EID}, RC={RC} ",
            "EID", eid, "RC", rc);
        if (!messageError.empty() && !resolution.empty())
        {
            logDiscoveryFailedMessage(eid, messageError, resolution,
                                      mctpInterfaces);
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
            lg2::error(
                "Failed to attempt the execute of 'getFirmwareParameters' function., EID={EID}, RC={RC} ",
                "EID", eid, "RC", rc);
        }
    }

    if (rc)
    {
        cleanUpResources(eid);
        lg2::error(
            "Failed to execute the 'getFirmwareParameters' function., EID={EID}, RC={RC} ",
            "EID", eid, "RC", rc);
        if (!messageError.empty() && !resolution.empty())
        {
            logDiscoveryFailedMessage(eid, messageError, resolution,
                                      mctpInterfaces);
        }
    }

    co_return rc;
}

requester::Coroutine InventoryManager::initiateGetActiveFirmwareVersion(
    mctp_eid_t eid, UpdateFWVersionCallBack updateFWVersionCallback)
{
    uint64_t supportedTypes = 0;
    auto rc = co_await getPLDMTypes(eid, supportedTypes);
    if (rc)
    {
        lg2::error("getPLDMTypes failed, EID={EID} rc={RC}.", "EID", eid, "RC",
                   rc);
        co_return PLDM_ERROR;
    }

    auto isType5Supported = supportedTypes & (1 << PLDM_FWUP);
    if (!isType5Supported)
    {
        co_return PLDM_SUCCESS;
    }

    if (!mctpEidMap.contains(eid))
    {
        co_return PLDM_SUCCESS;
    }

    dbus::MctpInterfaces mctpInterfaces;
    auto co =
        getActiveFirmwareVersion(eid, mctpInterfaces, updateFWVersionCallback);

    if (inventoryCoRoutineHandlers.contains(eid))
    {
        inventoryCoRoutineHandlers[eid].destroy();
        inventoryCoRoutineHandlers[eid] = co.handle;
    }
    else
    {
        inventoryCoRoutineHandlers.emplace(eid, co.handle);
    }
    co_return PLDM_SUCCESS;
}

requester::Coroutine InventoryManager::getActiveFirmwareVersion(
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
    lg2::error(
        "Failed to attempt the execute of 'getFirmwareParameters' function., EID={EID}, RC={RC} ",
        "EID", eid, "RC", rc);
    if (!messageError.empty() && !resolution.empty())
    {
        logDiscoveryFailedMessage(eid, messageError, resolution,
                                  mctpInterfaces);
    }

    co_return rc;
}

void InventoryManager::cleanUpResources(mctp_eid_t eid)
{
    mctpEidMap.erase(eid);
    descriptorMap.erase(eid);
}

requester::Coroutine InventoryManager::queryDeviceIdentifiers(
    mctp_eid_t eid, std::string& messageError, std::string& resolution)
{

    auto instanceId = requester.getInstanceId(eid);
    Request requestMsg(sizeof(pldm_msg_hdr) +
                       PLDM_QUERY_DEVICE_IDENTIFIERS_REQ_BYTES);
    auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());
    auto rc = encode_query_device_identifiers_req(
        instanceId, PLDM_QUERY_DEVICE_IDENTIFIERS_REQ_BYTES, request);
    if (rc)
    {
        requester.markFree(eid, instanceId);
        lg2::error(
            "encode_query_device_identifiers_req failed, EID={EID}, RC={RC}",
            "EID", eid, "RC", rc);
        co_return rc;
    }

    const pldm_msg* responseMsg = NULL;
    size_t responseLen = 0;

    rc = co_await SendRecvPldmMsgOverMctp(handler, eid, requestMsg,
                                          &responseMsg, &responseLen);

    if (rc)
    {
        lg2::error(
            "Failed to send QueryDeviceIdentifiers request, EID={EID}, RC={RC} ",
            "EID", eid, "RC", rc);
        co_return rc;
    }

    rc = co_await parseQueryDeviceIdentifiersResponse(
        eid, responseMsg, responseLen, messageError, resolution);
    if (rc)
    {
        lg2::error(
            "Failed to execute the 'parseQueryDeviceIdentifiersResponse' function., EID={EID}, RC={RC} ",
            "EID", eid, "RC", rc);

        co_return rc;
    }

    co_return rc;
}

requester::Coroutine InventoryManager::parseQueryDeviceIdentifiersResponse(
    mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen,
    std::string& messageError, std::string& resolution)
{
    if (response == nullptr || !respMsgLen)
    {
        lg2::error("No response received for QueryDeviceIdentifiers, EID={EID}",
                   "EID", eid);
        messageError = "Discovery Timed Out";
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
        lg2::error(
            "Decoding QueryDeviceIdentifiers response failed, EID={EID}, RC={RC}",
            "EID", eid, "RC", rc);
        messageError =
            "Failed to discover: decoding QueryDeviceIdentifiers response failed";
        resolution = "Reset the baseboard and retry the operation.";
        pldm::utils::printBuffer(pldm::utils::Rx, response, respMsgLen);
        co_return PLDM_ERROR;
    }

    if (completionCode)
    {
        lg2::error(
            "QueryDeviceIdentifiers response failed with error completion code, EID={EID}, CC={CC}",
            "EID", eid, "CC", completionCode);
        messageError = "Failed to discover";
        resolution = "Reset the baseboard and retry the operation.";
        pldm::utils::printBuffer(pldm::utils::Rx, response, respMsgLen);
        co_return PLDM_ERROR;
    }

    Descriptors descriptors{};
    std::ostringstream descriptorLog;
    while (descriptorCount-- && (deviceIdentifiersLen > 0))
    {
        uint16_t descriptorType = 0;
        variable_field descriptorData{};

        rc = decode_descriptor_type_length_value(
            descriptorPtr, deviceIdentifiersLen, &descriptorType,
            &descriptorData);
        if (rc)
        {
            lg2::error(
                "Decoding descriptor type, length and value failed, EID={EID}, RC={RC} ",
                "EID", eid, "RC", rc);
            pldm::utils::printBuffer(pldm::utils::Rx, response, respMsgLen);
            co_return PLDM_ERROR;
        }

        if (descriptorType != PLDM_FWUP_VENDOR_DEFINED)
        {
            std::vector<uint8_t> descData(
                descriptorData.ptr, descriptorData.ptr + descriptorData.length);
            std::ostringstream descValueStream;
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
                lg2::error(
                    "Decoding Vendor-defined descriptor value failed, EID={EID}, RC={RC} ",
                    "EID", eid, "RC", rc);
                pldm::utils::printBuffer(pldm::utils::Rx, response, respMsgLen);
                co_return PLDM_ERROR;
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
            std::ostringstream descValueStream;
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
    lg2::info("EID={EID} Descriptors=[{DESC}]", "EID", eid, "DESC",
              descriptorLog.str());
    descriptorMap.emplace(eid, std::move(descriptors));

    co_return PLDM_SUCCESS;
}

requester::Coroutine InventoryManager::getFirmwareParameters(
    mctp_eid_t eid, std::string& messageError, std::string& resolution,
    dbus::MctpInterfaces& mctpInterfaces, bool refreshFWVersionOnly)
{
    auto instanceId = requester.getInstanceId(eid);
    Request requestMsg(sizeof(pldm_msg_hdr) +
                       PLDM_GET_FIRMWARE_PARAMETERS_REQ_BYTES);
    auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());
    auto rc = encode_get_firmware_parameters_req(
        instanceId, PLDM_GET_FIRMWARE_PARAMETERS_REQ_BYTES, request);
    if (rc)
    {
        requester.markFree(eid, instanceId);
        lg2::error(
            "encode_get_firmware_parameters_req failed, EID={EID}, RC={RC}",
            "EID", eid, "RC", rc);
        co_return rc;
    }

    const pldm_msg* responseMsg = NULL;
    size_t responseLen = 0;

    rc = co_await SendRecvPldmMsgOverMctp(handler, eid, requestMsg,
                                          &responseMsg, &responseLen);

    if (rc)
    {
        lg2::error(
            "Failed to send GetFirmwareParameters request, EID={EID}, RC={RC} ",
            "EID", eid, "RC", rc);
        co_return rc;
    }

    rc = co_await parseGetFWParametersResponse(
        eid, responseMsg, responseLen, messageError, resolution, mctpInterfaces,
        refreshFWVersionOnly);

    if (rc)
    {
        lg2::error("parseGetFWParametersResponse failed, EID={EID}, RC={RC} ",
                   "EID", eid, "RC", rc);
    }

    co_return rc;
}

requester::Coroutine InventoryManager::parseGetFWParametersResponse(
    mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen,
    std::string& messageError, std::string& resolution,
    dbus::MctpInterfaces& mctpInterfaces, bool refreshFWVersionOnly)
{
    if (response == nullptr || !respMsgLen)
    {
        lg2::error("No response received for GetFirmwareParameters, EID={EID}",
                   "EID", eid);
        messageError = "Discovery Timed Out";
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
        lg2::error(
            "Decoding GetFirmwareParameters response failed, EID={EID}, RC={RC}",
            "EID", eid, "RC", rc);
        pldm::utils::printBuffer(pldm::utils::Rx, response, respMsgLen);
        messageError =
            "Failed to discover: decoding GetFirmwareParameters response failed";
        resolution = "Reset the baseboard and retry the operation.";
        co_return PLDM_ERROR;
    }

    if (fwParams.completion_code)
    {
        lg2::error(
            "GetFirmwareParameters response failed with error completion code, EID={EID}, CC={CC}",
            "EID", eid, "CC", fwParams.completion_code);
        messageError = "Failed to discover";
        resolution = "Reset the baseboard and retry the operation.";
        pldm::utils::printBuffer(pldm::utils::Rx, response, respMsgLen);
        co_return PLDM_ERROR;
    }

    auto compParamPtr = compParamTable.ptr;
    auto compParamTableLen = compParamTable.length;
    pldm_component_parameter_entry compEntry{};
    variable_field activeCompVerStr{};
    variable_field pendingCompVerStr{};

    ComponentInfo componentInfo{};
    while (fwParams.comp_count-- && (compParamTableLen > 0))
    {
        auto rc = decode_get_firmware_parameters_resp_comp_entry(
            compParamPtr, compParamTableLen, &compEntry, &activeCompVerStr,
            &pendingCompVerStr);
        if (rc)
        {
            lg2::error(
                "Decoding component parameter table entry failed, EID={EID}, RC={RC}",
                "EID", eid, "RC", rc);
            messageError =
                "Failed to discover: decoding component parameter table entry failed";
            resolution = "Reset the baseboard and retry the operation.";

            pldm::utils::printBuffer(pldm::utils::Rx, response, respMsgLen);
            co_return PLDM_ERROR;
        }

        auto compClassification = compEntry.comp_classification;
        auto compIdentifier = compEntry.comp_identifier;
        componentInfo.emplace(
            std::make_pair(compClassification, compIdentifier),
            std::make_tuple(compEntry.comp_classification_index,
                            utils::toString(activeCompVerStr)));
        compParamPtr += sizeof(pldm_component_parameter_entry) +
                        activeCompVerStr.length + pendingCompVerStr.length;
        compParamTableLen -= sizeof(pldm_component_parameter_entry) +
                             activeCompVerStr.length + pendingCompVerStr.length;
    }
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
                lg2::info(
                    "Fastest path to UUID={UUID} is already set to EID={EID}",
                    "UUID", uuid, "EID", eid);
                co_return PLDM_SUCCESS;
            }

            // Insert eid into priority queue, to identify the new fastest EID
            search->second.push({eid, mediumType, bindingType});

            const auto& newTop = search->second.top();
            auto newFastestEid = newTop.eid;
            // Check if eid is the fastest eid after comparison
            if (eid != newFastestEid)
            {
                lg2::info(
                    "Fastest path to UUID={UUID} is set to EID={EID}, removed DELETED_EID={DELETED_EID}",
                    "UUID", uuid, "EID", newFastestEid, "DELETED_EID", eid);
                descriptorMap.erase(eid);
                componentInfoMap.erase(eid);
            }
            else if (eid == newFastestEid)
            {
                lg2::info(
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

void InventoryManager::logDiscoveryFailedMessage(
    const mctp_eid_t& eid, const std::string& messageError,
    const std::string& resolution, dbus::MctpInterfaces mctpInterfaces)
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
                           resolution);
        }
    }
}

} // namespace fw_update

} // namespace pldm