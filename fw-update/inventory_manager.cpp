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

void InventoryManager::discoverFDs(const MctpInfos& mctpInfos)
{
    for (const auto& [eid, uuid, mediumType, networkId, bindingType] : mctpInfos)
    {
        mctpEidMap[eid] = std::make_tuple(uuid, mediumType, bindingType);

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
            continue;
        }

        rc = handler.registerRequest(
            eid, instanceId, PLDM_FWUP, PLDM_QUERY_DEVICE_IDENTIFIERS,
            std::move(requestMsg),
            std::move(std::bind_front(&InventoryManager::queryDeviceIdentifiers,
                                      this)));
        if (rc)
        {
            lg2::error(
                "Failed to send QueryDeviceIdentifiers request, EID={EID}, RC={RC} ",
                "EID", eid, "RC", rc);
        }
    }
}

void InventoryManager::queryDeviceIdentifiers(mctp_eid_t eid,
                                              const pldm_msg* response,
                                              size_t respMsgLen)
{
    if (response == nullptr || !respMsgLen)
    {
        lg2::error("No response received for QueryDeviceIdentifiers, EID={EID}",
                   "EID", eid);
        std::string messageError = "Discovery Timed Out";
        std::string resolution = "Reset the baseboard and retry the operation.";
        logDiscoveryFailedMessage(eid, messageError, resolution);
        mctpEidMap.erase(eid);
        return;
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
        return;
    }

    if (completionCode)
    {
        lg2::error(
            "QueryDeviceIdentifiers response failed with error completion code, EID={EID}, CC={CC}",
            "EID", eid, "CC", completionCode);
        std::string messageError = "Failed to discover";
        std::string resolution = "Reset the baseboard and retry the operation.";
        logDiscoveryFailedMessage(eid, messageError, resolution);
        return;
    }

    Descriptors descriptors{};
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
            return;
        }

        if (descriptorType != PLDM_FWUP_VENDOR_DEFINED)
        {
            std::vector<uint8_t> descData(
                descriptorData.ptr, descriptorData.ptr + descriptorData.length);
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
                return;
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
    if (eid != 0) // Revert this workaround once SPI issue is fixed
    {
        descriptorMap.emplace(eid, std::move(descriptors));
    }

    // Send GetFirmwareParameters request
    sendGetFirmwareParametersRequest(eid);
}

void InventoryManager::sendGetFirmwareParametersRequest(mctp_eid_t eid)
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
        return;
    }

    rc = handler.registerRequest(
        eid, instanceId, PLDM_FWUP, PLDM_GET_FIRMWARE_PARAMETERS,
        std::move(requestMsg),
        std::move(
            std::bind_front(&InventoryManager::getFirmwareParameters, this)));
    if (rc)
    {
        lg2::error(
            "Failed to send GetFirmwareParameters request, EID={EID}, RC={RC} ",
            "EID", eid, "RC", rc);
    }
}

void InventoryManager::getFirmwareParameters(mctp_eid_t eid,
                                             const pldm_msg* response,
                                             size_t respMsgLen)
{
    if (response == nullptr || !respMsgLen)
    {
        lg2::error("No response received for GetFirmwareParameters, EID={EID}",
                   "EID", eid);
        std::string messageError = "Discovery Timed Out";
        std::string resolution = "Reset the baseboard and retry the operation.";
        logDiscoveryFailedMessage(eid, messageError, resolution);
        descriptorMap.erase(eid);
        mctpEidMap.erase(eid);
        return;
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
        return;
    }

    if (fwParams.completion_code)
    {
        lg2::error(
            "GetFirmwareParameters response failed with error completion code, EID={EID}, CC={CC}",
            "EID", eid, "CC", fwParams.completion_code);
        std::string messageError = "Failed to discover";
        std::string resolution = "Reset the baseboard and retry the operation.";
        logDiscoveryFailedMessage(eid, messageError, resolution);
        return;
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
            return;
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
    // UUID.
    if (mctpEidMap.contains(eid))
    {
        const auto& [uuid, mediumType, bindingType] = mctpEidMap[eid];
        if (mctpInfoMap.contains(uuid))
        {
            auto search = mctpInfoMap.find(uuid);
            const auto& prevTop = search->second.top();
            auto prevTopEid = prevTop.eid;
            for (auto& mctpInfo : search->second)
            {
                if (mctpInfo.eid == eid)
                {
                    if (prevTopEid == eid)
                    {
                        lg2::info("Top EID in the queue matches with current "
                                  "EID. Skip erasing the map for EID={EID}",
                                  "EID", eid);
                    }
                    else
                    {
                        lg2::info("Top EID does not match with current EID."
                                  " Erasing descriptor map for EID={EID}",
                                  "EID", eid);
                        descriptorMap.erase(eid);
                        componentInfoMap.erase(eid);
                    }
                    return;
                }
            }
            search->second.push({eid, mediumType, bindingType});
            const auto& top = search->second.top();
            if (prevTopEid == top.eid)
            {
                descriptorMap.erase(eid);
                componentInfoMap.erase(eid);
            }
            else if (eid == top.eid)
            {
                descriptorMap.erase(prevTopEid);
                componentInfoMap.erase(prevTopEid);
            }
        }
        else
        {
            std::priority_queue<MctpEidInfo> mctpEidInfo;
            mctpEidInfo.push({eid, mediumType, bindingType});
            mctpInfoMap.emplace(uuid, std::move(mctpEidInfo));
            if (createInventoryCallBack)
            {
                createInventoryCallBack(eid, uuid);
            }
        }
    }
}

void InventoryManager::logDiscoveryFailedMessage(
    const mctp_eid_t& eid, const std::string& messageError,
    const std::string& resolution)
{
    if (mctpEidMap.contains(eid))
    {
        const auto& [uuid, mediumType, bindingType] = mctpEidMap[eid];
        if (deviceInventoryInfo.contains(uuid))
        {
            auto search = deviceInventoryInfo.find(uuid);
            const auto& deviceObjPath = std::get<DeviceObjPath>(
                std::get<CreateDeviceInfo>(search->second));
            std::string compName =
                std::filesystem::path(deviceObjPath).filename();
            createLogEntry(resourceErrorDetected, compName, messageError,
                           resolution);
        }
    }
}

} // namespace fw_update

} // namespace pldm