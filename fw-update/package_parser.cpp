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
#include "package_parser.hpp"

#include "common/utils.hpp"
#include "package_signature.hpp"

#include <libpldm/edac.h>
#include <libpldm/firmware_update.h>

#include <phosphor-logging/lg2.hpp>
#include <xyz/openbmc_project/Common/error.hpp>
#include <xyz/openbmc_project/Software/Update/server.hpp>

#include <memory>

PHOSPHOR_LOG2_USING;

namespace pldm
{

namespace fw_update
{

using InternalFailure =
    sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure;

size_t PackageParser::parseFDIdentificationArea(
    DeviceIDRecordCount deviceIdRecCount, const std::vector<uint8_t>& pkgHdr,
    size_t offset, uint8_t formatVersion)
{
    size_t pkgHdrRemainingSize = pkgHdr.size() - offset;

    while (deviceIdRecCount-- && (pkgHdrRemainingSize > 0))
    {
        pldm_firmware_device_id_record deviceIdRecHeader{};
        variable_field applicableComponents{};
        variable_field compImageSetVersionStr{};
        variable_field recordDescriptors{};
        variable_field fwDevicePkgData{};

        auto rc = decode_firmware_device_id_record(
            pkgHdr.data() + offset, pkgHdrRemainingSize,
            componentBitmapBitLength, &deviceIdRecHeader, &applicableComponents,
            &compImageSetVersionStr, &recordDescriptors, &fwDevicePkgData,
            formatVersion);
        if (rc)
        {
            error(
                "Failed to decode firmware device ID record, response code '{RC}'",
                "RC", rc);
            throw InternalFailure();
        }

        Descriptors descriptors{};
        while (deviceIdRecHeader.descriptor_count-- &&
               (recordDescriptors.length > 0))
        {
            uint16_t descriptorType = 0;
            variable_field descriptorData{};

            rc = decode_descriptor_type_length_value(
                recordDescriptors.ptr, recordDescriptors.length,
                &descriptorType, &descriptorData);
            if (rc)
            {
                error(
                    "Failed to decode descriptor type value of type '{TYPE}' and  length '{LENGTH}', response code '{RC}'",
                    "TYPE", descriptorType, "LENGTH", recordDescriptors.length,
                    "RC", rc);
                throw InternalFailure();
            }

            if (descriptorType != PLDM_FWUP_VENDOR_DEFINED)
            {
                descriptors.emplace(
                    descriptorType,
                    DescriptorData{descriptorData.ptr,
                                   descriptorData.ptr + descriptorData.length});
            }
            else
            {
                uint8_t descTitleStrType = 0;
                variable_field descTitleStr{};
                variable_field vendorDefinedDescData{};

                rc = decode_vendor_defined_descriptor_value(
                    descriptorData.ptr, descriptorData.length,
                    &descTitleStrType, &descTitleStr, &vendorDefinedDescData);
                if (rc)
                {
                    error(
                        "Failed to decode vendor-defined descriptor value of type '{TYPE}' and  length '{LENGTH}', response code '{RC}'",
                        "TYPE", descriptorType, "LENGTH",
                        recordDescriptors.length, "RC", rc);
                    throw InternalFailure();
                }

                descriptors.emplace(
                    descriptorType,
                    std::make_tuple(utils::toString(descTitleStr),
                                    VendorDefinedDescriptorData{
                                        vendorDefinedDescData.ptr,
                                        vendorDefinedDescData.ptr +
                                            vendorDefinedDescData.length}));
            }

            auto nextDescriptorOffset =
                sizeof(pldm_descriptor_tlv().descriptor_type) +
                sizeof(pldm_descriptor_tlv().descriptor_length) +
                descriptorData.length;
            recordDescriptors.ptr += nextDescriptorOffset;
            recordDescriptors.length -= nextDescriptorOffset;
        }

        DeviceUpdateOptionFlags deviceUpdateOptionFlags =
            deviceIdRecHeader.device_update_option_flags.value;

        ApplicableComponents componentsList;

        for (size_t varBitfieldIdx = 0;
             varBitfieldIdx < applicableComponents.length; varBitfieldIdx++)
        {
            std::bitset<8> entry{*(applicableComponents.ptr + varBitfieldIdx)};
            for (size_t idx = 0; idx < entry.size(); idx++)
            {
                if (entry[idx])
                {
                    componentsList.emplace_back(
                        idx + (varBitfieldIdx * entry.size()));
                }
            }
        }

        fwDeviceIDRecords.emplace_back(std::make_tuple(
            deviceUpdateOptionFlags, componentsList,
            utils::toString(compImageSetVersionStr), std::move(descriptors),
            FirmwareDevicePackageData{
                fwDevicePkgData.ptr,
                fwDevicePkgData.ptr + fwDevicePkgData.length}));
        offset += deviceIdRecHeader.record_length;
        pkgHdrRemainingSize -= deviceIdRecHeader.record_length;
    }

    return offset;
}

size_t PackageParser::parseCompImageInfoArea(
    ComponentImageCount compImageCount, const std::vector<uint8_t>& pkgHdr,
    size_t offset, uint8_t formatVersion)
{
    size_t pkgHdrRemainingSize = pkgHdr.size() - offset;

    while (compImageCount-- && (pkgHdrRemainingSize > 0))
    {
        pldm_component_image_information compImageInfo{};
        variable_field compVersion{};

        auto rc = decode_pldm_comp_image_info(
            pkgHdr.data() + offset, pkgHdrRemainingSize, &compImageInfo,
            &compVersion);
        if (rc)
        {
            error(
                "Failed to decode component image information, response code '{RC}'",
                "RC", rc);
            throw InternalFailure();
        }

        CompClassification compClassification =
            compImageInfo.comp_classification;
        CompIdentifier compIdentifier = compImageInfo.comp_identifier;
        CompComparisonStamp compComparisonTime =
            compImageInfo.comp_comparison_stamp;
        CompOptions compOptions = compImageInfo.comp_options.value;
        ReqCompActivationMethod reqCompActivationMethod =
            compImageInfo.requested_comp_activation_method.value;
        CompLocationOffset compLocationOffset =
            compImageInfo.comp_location_offset;
        CompSize compSize = compImageInfo.comp_size;

        componentImageInfos.emplace_back(std::make_tuple(
            compClassification, compIdentifier, compComparisonTime, compOptions,
            reqCompActivationMethod, compLocationOffset, compSize,
            utils::toString(compVersion)));

        auto sizeOfCompImageInfo = sizeof(pldm_component_image_information) +
                                   compImageInfo.comp_version_string_length;
        if (formatVersion >= PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR04H)
        {
            // Skipping component opaque data length field
            sizeOfCompImageInfo += sizeof(CompOpaqueDataLength);
        }
        offset += sizeOfCompImageInfo;
        pkgHdrRemainingSize -= sizeOfCompImageInfo;
    }

    return offset;
}

uintmax_t PackageParser::calculatePackageSize()
{
    uintmax_t calcPkgSize = pkgHeaderSize;
    for (const auto& componentImageInfo : componentImageInfos)
    {
        CompLocationOffset compLocOffset = std::get<static_cast<size_t>(
            ComponentImageInfoPos::CompLocationOffsetPos)>(componentImageInfo);
        CompSize compSize =
            std::get<static_cast<size_t>(ComponentImageInfoPos::CompSizePos)>(
                componentImageInfo);

        if (compLocOffset != calcPkgSize)
        {
            auto cmpVersion = std::get<static_cast<size_t>(
                ComponentImageInfoPos::CompVersionPos)>(componentImageInfo);
            error(
                "Failed to validate the component location offset '{OFFSET}' for version '{COMPONENT_VERSION}' and package size '{SIZE}'",
                "OFFSET", compLocOffset, "COMPONENT_VERSION", cmpVersion,
                "SIZE", calcPkgSize);
            throw InternalFailure();
        }

        calcPkgSize += compSize;
    }

    return calcPkgSize;
}

void PackageParser::validatePkgTotalSize(uintmax_t pkgSize)
{
    uintmax_t calcPkgSize = calculatePackageSize();

    // The package can be signed or not signed.
    // For not signed package, the real size of package must be equal with
    // calculated size. For signed package, the real size is 1K higher than the
    // calculated size. The FW Update Package Signature is padding to 1 KB (1024
    // bytes)

    if ((calcPkgSize != pkgSize) &&
        (calcPkgSize + pldmFwupSignaturePackageSize != pkgSize))
    {
        error(
            "Failed to match package size '{PKG_SIZE}' to calculated package size '{CALCULATED_PACKAGE_SIZE}'.",
            "PKG_SIZE", pkgSize, "CALCULATED_PACKAGE_SIZE", calcPkgSize);
        throw InternalFailure();
    }
}

void PackageParser::parse(const uint8_t* pkgData, uintmax_t pkgSize)
{
    if (pkgData == nullptr || pkgHeaderSize > pkgSize)
    {
#ifndef SKIP_PACKAGE_SIZE_CHECK
        error(
            "Invalid package data or header size '{PKG_HDR_SIZE}' exceeds package size '{PKG_SIZE}'",
            "PKG_HDR_SIZE", pkgHeaderSize, "PKG_SIZE", pkgSize);
        throw InternalFailure();
#endif
    }

    // Create a vector view of only the header portion - no copying of entire
    // package
    std::vector<uint8_t> pkgHdr(pkgData, pkgData + pkgHeaderSize);

    size_t offset = sizeof(pldm_package_header_information) + pkgVersion.size();
    if (offset + sizeof(DeviceIDRecordCount) >= pkgHeaderSize)
    {
        error("Failed to parse package header of size '{PKG_HDR_SIZE}'",
              "PKG_HDR_SIZE", pkgHeaderSize);
        throw InternalFailure();
    }

    auto deviceIdRecCount = static_cast<DeviceIDRecordCount>(pkgHdr[offset]);
    offset += sizeof(DeviceIDRecordCount);

    offset = parseFDIdentificationArea(deviceIdRecCount, pkgHdr, offset,
                                       formatVersion);
    if (deviceIdRecCount != fwDeviceIDRecords.size())
    {
        error("Failed to find DeviceIDRecordCount {DREC_CNT} entries",
              "DREC_CNT", deviceIdRecCount);
        throw InternalFailure();
    }

    if (formatVersion >= PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR03H)
    {
        DownstreamDeviceIDRecordCount downstreamDeviceIdRecCount =
            pkgHdr[offset];

        // Downstream devices are not supported on OEM NVIDIA platforms
        if (downstreamDeviceIdRecCount != 0)
        {
            error(
                "Unexpected DownstreamDeviceIDRecordCount '{COUNT}' (expected 0)",
                "COUNT", downstreamDeviceIdRecCount);
            throw InternalFailure();
        }

        offset += sizeof(DownstreamDeviceIDRecordCount);
    }

    if (offset + sizeof(ComponentImageCount) >= pkgHeaderSize)
    {
        error("Failed to parsing package header of size '{PKG_HDR_SIZE}'",
              "PKG_HDR_SIZE", pkgHeaderSize);
        throw InternalFailure();
    }

    auto compImageCount = static_cast<ComponentImageCount>(
        le16toh(pkgHdr[offset] | (pkgHdr[offset + 1] << 8)));
    offset += sizeof(ComponentImageCount);

    offset =
        parseCompImageInfoArea(compImageCount, pkgHdr, offset, formatVersion);
    if (compImageCount != componentImageInfos.size())
    {
        error("Failed to find ComponentImageCount '{COMP_IMG_CNT}' entries",
              "COMP_IMG_CNT", compImageCount);
        throw InternalFailure();
    }

    size_t expectedChecksumSize = sizeof(PackageHeaderChecksum);
    if (formatVersion >= PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR04H)
    {
        expectedChecksumSize += sizeof(PackagePayloadChecksum);
    }

    if (offset + expectedChecksumSize != pkgHeaderSize)
    {
        error("Failed to parse package header of size '{PKG_HDR_SIZE}'",
              "PKG_HDR_SIZE", pkgHeaderSize);
        throw InternalFailure();
    }

    offset += sizeof(PackageHeaderChecksum);

    if (formatVersion >= PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR04H)
    {
        auto checksum = static_cast<PackagePayloadChecksum>(
            le32toh(pkgHdr[offset] | (pkgHdr[offset + 1] << 8) |
                    (pkgHdr[offset + 2] << 16) | (pkgHdr[offset + 3] << 24)));
        auto calcPkgSize = calculatePackageSize();
        if (calcPkgSize > pkgSize)
        {
            error(
                "Calculated package size '{CALC_SIZE}' exceeds actual package size '{PKG_SIZE}'",
                "CALC_SIZE", calcPkgSize, "PKG_SIZE", pkgSize);
            throw InternalFailure();
        }
        auto calcChecksum = pldm_edac_crc32(pkgData + pkgHeaderSize,
                                            calcPkgSize - pkgHeaderSize);
        if (calcChecksum != checksum)
        {
            error(
                "Payload Checksum Verification failed. Calculated checksum '{CALCULATED_CHECKSUM}' and expected checksum '{PACKAGE_PAYLOAD_CHECKSUM}'",
                "CALCULATED_CHECKSUM", calcChecksum, "PACKAGE_PAYLOAD_CHECKSUM",
                checksum);
            throw sdbusplus::error::xyz::openbmc_project::software::update::
                InvalidSignature();
        }
    }

#ifndef SKIP_PACKAGE_SIZE_CHECK

    validatePkgTotalSize(pkgSize);

#endif
}

std::unique_ptr<PackageParser> parsePkgHeader(const uint8_t* pkgData,
                                              size_t pkgSize)
{
    static const std::map<uint8_t, std::array<uint8_t, PLDM_FWUP_UUID_LENGTH>>
        supportedPackageVersions = {
            {PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR01H,
             PLDM_PACKAGE_HEADER_IDENTIFIER_V1_0},
            {PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR02H,
             PLDM_PACKAGE_HEADER_IDENTIFIER_V1_1},
            {PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR03H,
             PLDM_PACKAGE_HEADER_IDENTIFIER_V1_2},
            {PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR04H,
             PLDM_PACKAGE_HEADER_IDENTIFIER_V1_3},
        };

    if (pkgData == nullptr || pkgSize < sizeof(pldm_package_header_information))
    {
        error("Invalid package data or size {SIZE}", "SIZE", pkgSize);
        return nullptr;
    }

    pldm_package_header_information pkgHeader{};
    variable_field pkgVersion{};
    auto rc = decode_pldm_package_header_info(pkgData, pkgSize, &pkgHeader,
                                              &pkgVersion);
    if (rc)
    {
        error(
            "Failed to decode PLDM package header information, response code '{RC}'",
            "RC", rc);
        return nullptr;
    }

    auto it =
        supportedPackageVersions.find(pkgHeader.package_header_format_version);
    if (it != supportedPackageVersions.end())
    {
        const auto& expectedUUID = it->second;
        bool validPackage =
            std::equal(pkgHeader.uuid, pkgHeader.uuid + PLDM_FWUP_UUID_LENGTH,
                       expectedUUID.begin(), expectedUUID.end());

        if (validPackage)
        {
            PackageHeaderSize pkgHdrSize = pkgHeader.package_header_size;
            ComponentBitmapBitLength componentBitmapBitLength =
                pkgHeader.component_bitmap_bit_length;

            return std::make_unique<PackageParser>(
                pkgHdrSize, utils::toString(pkgVersion),
                componentBitmapBitLength,
                pkgHeader.package_header_format_version);
        }
    }
    return nullptr;
}

} // namespace fw_update

} // namespace pldm
