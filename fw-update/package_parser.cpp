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

#include <cstring>
#include <memory>

PHOSPHOR_LOG2_USING;

namespace pldm
{

namespace fw_update
{

using InternalFailure =
    sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure;

Descriptors PackageParser::parseRecordDescriptors(
    uint8_t descriptorCount, variable_field& recordDescriptors)
{
    Descriptors descriptors{};
    while (descriptorCount-- && (recordDescriptors.length > 0))
    {
        uint16_t descriptorType = 0;
        variable_field descriptorData{};

        auto rc = decode_descriptor_type_length_value(
            recordDescriptors.ptr, recordDescriptors.length, &descriptorType,
            &descriptorData);
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
                descriptorData.ptr, descriptorData.length, &descTitleStrType,
                &descTitleStr, &vendorDefinedDescData);
            if (rc)
            {
                error(
                    "Failed to decode vendor-defined descriptor value of type '{TYPE}' and  length '{LENGTH}', response code '{RC}'",
                    "TYPE", descriptorType, "LENGTH", recordDescriptors.length,
                    "RC", rc);
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
    return descriptors;
}

size_t PackageParser::parseFDIdentificationArea(
    DeviceIDRecordCount deviceIdRecCount, std::span<const uint8_t> pkgHdr,
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

        Descriptors descriptors = parseRecordDescriptors(
            deviceIdRecHeader.descriptor_count, recordDescriptors);

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

namespace
{

/** @brief Bounds-checked little-endian field readers over a byte range,
 *         used by parseDownstreamIdentificationArea() below. libpldm has no
 *         flat decode entry point for DSP0267 Table 5 (only the newer
 *         iterator-based struct pldm_package API, which this class's
 *         flat-decode style predates), so this area is read directly. */
uint8_t readU8(const uint8_t* base, size_t recLen, size_t& pos)
{
    if (pos + sizeof(uint8_t) > recLen)
    {
        throw sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure();
    }
    return base[pos++];
}

uint16_t readU16(const uint8_t* base, size_t recLen, size_t& pos)
{
    if (pos + sizeof(uint16_t) > recLen)
    {
        throw sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure();
    }
    uint16_t value = le16toh(base[pos] | (base[pos + 1] << 8));
    pos += sizeof(uint16_t);
    return value;
}

uint32_t readU32(const uint8_t* base, size_t recLen, size_t& pos)
{
    if (pos + sizeof(uint32_t) > recLen)
    {
        throw sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure();
    }
    uint32_t value = le32toh(static_cast<uint32_t>(base[pos]) |
                             (static_cast<uint32_t>(base[pos + 1]) << 8) |
                             (static_cast<uint32_t>(base[pos + 2]) << 16) |
                             (static_cast<uint32_t>(base[pos + 3]) << 24));
    pos += sizeof(uint32_t);
    return value;
}

/** @brief Advance pos past a byte range of the given length, throwing if it
 *         would run past recLen. Returns a variable_field over that range. */
variable_field readBytes(const uint8_t* base, size_t recLen, size_t& pos,
                         size_t len)
{
    if (pos + len > recLen)
    {
        throw sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure();
    }
    variable_field field{base + pos, len};
    pos += len;
    return field;
}

} // namespace

size_t PackageParser::parseDownstreamIdentificationArea(
    DownstreamDeviceIDRecordCount downstreamDeviceIdRecCount,
    std::span<const uint8_t> pkgHdr, size_t offset, uint8_t formatVersion)
{
    size_t pkgHdrRemainingSize = pkgHdr.size() - offset;
    const size_t applicableComponentsSize = (componentBitmapBitLength + 7) / 8;

    while (downstreamDeviceIdRecCount-- && (pkgHdrRemainingSize > 0))
    {
        const uint8_t* recBase = pkgHdr.data() + offset;
        size_t pos = 0;

        // RecordLength is authoritative for how far to advance `offset`
        // afterwards; every other field below is read relative to `pos`
        // within [recBase, recBase + recordLength).
        uint16_t recordLength = readU16(recBase, pkgHdrRemainingSize, pos);
        if (recordLength > pkgHdrRemainingSize)
        {
            error(
                "Downstream device ID record length '{LENGTH}' exceeds remaining package header size '{REMAINING}'",
                "LENGTH", recordLength, "REMAINING", pkgHdrRemainingSize);
            throw InternalFailure();
        }

        uint8_t descriptorCount = readU8(recBase, recordLength, pos);
        DownstreamDeviceUpdateOptionFlags updateOptionFlags =
            readU32(recBase, recordLength, pos);
        // SelfContainedActivationMinVersionStringType is read but not
        // retained: matching/job-building only needs the descriptors and
        // applicable-components bitmap below.
        readU8(recBase, recordLength, pos);
        uint8_t selfContainedActivationMinVersionStringLength =
            readU8(recBase, recordLength, pos);
        uint16_t downstreamDevicePackageDataLength =
            readU16(recBase, recordLength, pos);

        uint32_t referenceManifestLength = 0;
        bool hasReferenceManifestLength = false;
        if (formatVersion >= PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR04H)
        {
            referenceManifestLength = readU32(recBase, recordLength, pos);
            hasReferenceManifestLength = true;
        }

        variable_field applicableComponentsField =
            readBytes(recBase, recordLength, pos, applicableComponentsSize);

        // SelfContainedActivationMinVersionString is written unconditionally
        // by this project's package creator tool (zero length when option
        // flag bit 0 is unset), so it is always read using the length field
        // above.
        readBytes(recBase, recordLength, pos,
                  selfContainedActivationMinVersionStringLength);

        // The comparison stamp only exists when option flag bit 0 is set.
        if (updateOptionFlags[0])
        {
            readU32(recBase, recordLength, pos);
        }

        variable_field recordDescriptorsField =
            readBytes(recBase, recordLength, pos,
                      recordLength > pos ? recordLength - pos : 0);
        Descriptors descriptors =
            parseRecordDescriptors(descriptorCount, recordDescriptorsField);

        DownstreamApplicableComponents componentsList;
        for (size_t varBitfieldIdx = 0;
             varBitfieldIdx < applicableComponentsField.length;
             varBitfieldIdx++)
        {
            std::bitset<8> entry{
                *(applicableComponentsField.ptr + varBitfieldIdx)};
            for (size_t idx = 0; idx < entry.size(); idx++)
            {
                if (entry[idx])
                {
                    componentsList.emplace_back(
                        idx + (varBitfieldIdx * entry.size()));
                }
            }
        }

        // recordDescriptorsField was handed every byte left in the record
        // (package data + reference manifest data live past the
        // descriptors), so recover where the descriptors actually ended by
        // re-deriving pos from how much parseRecordDescriptors consumed.
        pos = static_cast<size_t>(recordDescriptorsField.ptr - recBase);

        DownstreamPackageData downstreamPackageData;
        if (downstreamDevicePackageDataLength > 0)
        {
            auto pkgDataField = readBytes(recBase, recordLength, pos,
                                          downstreamDevicePackageDataLength);
            downstreamPackageData.assign(
                pkgDataField.ptr, pkgDataField.ptr + pkgDataField.length);
        }

        if (hasReferenceManifestLength && referenceManifestLength > 0)
        {
            readBytes(recBase, recordLength, pos, referenceManifestLength);
        }

        if (pos != recordLength)
        {
            error(
                "Downstream device ID record consumed '{CONSUMED}' bytes but declared RecordLength '{LENGTH}'",
                "CONSUMED", pos, "LENGTH", recordLength);
            throw InternalFailure();
        }

        downstreamDeviceIDRecords.emplace_back(std::make_tuple(
            updateOptionFlags, std::move(componentsList),
            std::move(descriptors), std::move(downstreamPackageData)));

        offset += recordLength;
        pkgHdrRemainingSize -= recordLength;
    }

    return offset;
}

size_t PackageParser::parseCompImageInfoArea(
    ComponentImageCount compImageCount, std::span<const uint8_t> pkgHdr,
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
        // Guard the size_t subtraction: an oversized (locally recomputed)
        // sizeOfCompImageInfo would underflow pkgHdrRemainingSize and walk
        // offset past pkgHdr into an OOB decode.
        if (sizeOfCompImageInfo > pkgHdrRemainingSize)
        {
            error(
                "Component image information size '{SIZE}' exceeds remaining package header size '{REMAINING}'",
                "SIZE", sizeOfCompImageInfo, "REMAINING", pkgHdrRemainingSize);
            throw InternalFailure();
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

void PackageParser::parse(std::span<const uint8_t> pkgHdr, uintmax_t pkgSize)
{
    if (pkgHeaderSize >= pkgHdr.size())
    {
        error("Invalid package header size '{PKG_HDR_SIZE}'", "PKG_HDR_SIZE",
              pkgHeaderSize);
        throw InternalFailure();
    }

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
        if (offset + sizeof(DownstreamDeviceIDRecordCount) > pkgHeaderSize)
        {
            error("Failed to parse package header of size '{PKG_HDR_SIZE}'",
                  "PKG_HDR_SIZE", pkgHeaderSize);
            throw InternalFailure();
        }
        DownstreamDeviceIDRecordCount downstreamDeviceIdRecCount =
            pkgHdr[offset];
        offset += sizeof(DownstreamDeviceIDRecordCount);

        if (downstreamDeviceIdRecCount != 0)
        {
            offset = parseDownstreamIdentificationArea(
                downstreamDeviceIdRecCount, pkgHdr, offset, formatVersion);
            if (downstreamDeviceIdRecCount != downstreamDeviceIDRecords.size())
            {
                error(
                    "Expected '{EXPECTED}' DownstreamDeviceIDRecordCount entries but parsed '{PARSED}'",
                    "EXPECTED", downstreamDeviceIdRecCount, "PARSED",
                    downstreamDeviceIDRecords.size());
                throw InternalFailure();
            }
        }
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

    std::erase_if(fwDeviceIDRecords, [&](auto& fdr) {
        auto& applicable = std::get<ApplicableComponents>(fdr);
        const auto removed = std::erase_if(applicable, [&](const auto& idx) {
            if (idx >= componentImageInfos.size())
            {
                error(
                    "Dropping out-of-range applicable-component index '{IDX}' (componentImageInfos size '{SIZE}')",
                    "IDX", idx, "SIZE", componentImageInfos.size());
                return true;
            }
            return false;
        });
        // A record stripped of every component would still match descriptors
        // and shadow a later valid record for the same device.
        if (removed > 0 && applicable.empty())
        {
            error(
                "Dropping FirmwareDeviceIDRecord with no valid applicable components after sanitization");
            return true;
        }
        return false;
    });

    std::erase_if(downstreamDeviceIDRecords, [&](auto& ddr) {
        auto& applicable = std::get<DownstreamApplicableComponents>(ddr);
        const auto removed = std::erase_if(applicable, [&](const auto& idx) {
            if (idx >= componentImageInfos.size())
            {
                error(
                    "Dropping out-of-range applicable-component index '{IDX}' (componentImageInfos size '{SIZE}')",
                    "IDX", idx, "SIZE", componentImageInfos.size());
                return true;
            }
            return false;
        });
        if (removed > 0 && applicable.empty())
        {
            error(
                "Dropping DownstreamDeviceIDRecord with no valid applicable components after sanitization");
            return true;
        }
        return false;
    });

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
        auto calcChecksum = pldm_edac_crc32(pkgHdr.data() + pkgHeaderSize,
                                            calcPkgSize - pkgHeaderSize);
        if (calcChecksum != checksum)
        {
            error(
                "Payload Checksum Verification failed. Calculated checksum '{CALCULATED_CHECKSUM}' and expected checksum '{PACKAGE_PAYLOAD_CHECKSUM}'",
                "CALCULATED_CHECKSUM", calcChecksum, "PACKAGE_PAYLOAD_CHECKSUM",
                checksum);
            throw PackagePayloadChecksumMismatch(checksum, calcChecksum);
        }
    }

#ifndef SKIP_PACKAGE_SIZE_CHECK

    validatePkgTotalSize(pkgSize);

#endif
}

std::unique_ptr<PackageParser> parsePkgHeader(
    const uint8_t* pkgData, size_t pkgSize, PackageHeaderDiagnostic* diagnostic)
{
    if (diagnostic != nullptr)
    {
        *diagnostic = {};
    }

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
        if (diagnostic != nullptr)
        {
            diagnostic->error = PackageHeaderError::unavailable;
        }
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
        // libpldm validates the header CRC while decoding. Recover the more
        // specific diagnostic before treating a decode failure as a bad header.
        const auto revision = pkgData[offsetof(pldm_package_header_information,
                                               package_header_format_version)];
        const auto supportedVersion = supportedPackageVersions.find(revision);
        if (supportedVersion != supportedPackageVersions.end() &&
            std::equal(pkgData, pkgData + PLDM_FWUP_UUID_LENGTH,
                       supportedVersion->second.begin()))
        {
            PackageHeaderSize headerSize{};
            std::memcpy(&headerSize,
                        pkgData + offsetof(pldm_package_header_information,
                                           package_header_size),
                        sizeof(headerSize));
            headerSize = le16toh(headerSize);
            const size_t checksumFieldsSize =
                sizeof(PackageHeaderChecksum) +
                (revision >= PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR04H
                     ? sizeof(PackagePayloadChecksum)
                     : 0);
            if (headerSize <= pkgSize &&
                headerSize >= sizeof(pldm_package_header_information) +
                                  checksumFieldsSize)
            {
                const size_t checksumOffset = headerSize - checksumFieldsSize;
                uint32_t storedChecksum{};
                std::memcpy(&storedChecksum, pkgData + checksumOffset,
                            sizeof(storedChecksum));
                storedChecksum = le32toh(storedChecksum);
                const uint32_t calculatedChecksum =
                    pldm_edac_crc32(pkgData, checksumOffset);
                if (storedChecksum != calculatedChecksum)
                {
                    error(
                        "Package header checksum mismatch. Calculated checksum '{CALCULATED_CHECKSUM}' and expected checksum '{PACKAGE_HEADER_CHECKSUM}'",
                        "CALCULATED_CHECKSUM", calculatedChecksum,
                        "PACKAGE_HEADER_CHECKSUM", storedChecksum);
                    if (diagnostic != nullptr)
                    {
                        diagnostic->error =
                            PackageHeaderError::checksumMismatch;
                        diagnostic->expectedChecksum = storedChecksum;
                        diagnostic->calculatedChecksum = calculatedChecksum;
                    }
                    return nullptr;
                }
            }
        }
        if (diagnostic != nullptr)
        {
            diagnostic->error = PackageHeaderError::invalid;
        }
        return nullptr;
    }

    auto it =
        supportedPackageVersions.find(pkgHeader.package_header_format_version);
    if (it != supportedPackageVersions.end())
    {
        // Validate UUID matches the expected value for this format version
        const auto& expectedUUID = it->second;
        if (!std::equal(pkgHeader.uuid, pkgHeader.uuid + PLDM_FWUP_UUID_LENGTH,
                        expectedUUID.begin(), expectedUUID.end()))
        {
            error("PLDM package UUID does not match format version");
            if (diagnostic != nullptr)
            {
                diagnostic->error = PackageHeaderError::invalid;
            }
            return nullptr;
        }
        PackageHeaderSize pkgHdrSize = pkgHeader.package_header_size;
        const size_t checksumFieldsSize =
            sizeof(PackageHeaderChecksum) +
            (pkgHeader.package_header_format_version >=
                     PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR04H
                 ? sizeof(PackagePayloadChecksum)
                 : 0);
        const size_t minimumHeaderSize =
            sizeof(pldm_package_header_information) + pkgVersion.length +
            sizeof(DeviceIDRecordCount) + sizeof(ComponentImageCount) +
            (pkgHeader.package_header_format_version >=
                     PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR03H
                 ? sizeof(DownstreamDeviceIDRecordCount)
                 : 0) +
            checksumFieldsSize;
        if (pkgHdrSize > pkgSize || pkgHdrSize < minimumHeaderSize)
        {
            error("Invalid PLDM package header size {SIZE}", "SIZE",
                  pkgHdrSize);
            if (diagnostic != nullptr)
            {
                diagnostic->error = PackageHeaderError::invalid;
            }
            return nullptr;
        }

        ComponentBitmapBitLength componentBitmapBitLength =
            pkgHeader.component_bitmap_bit_length;
        return std::make_unique<PackageParser>(
            pkgHdrSize, utils::toString(pkgVersion), componentBitmapBitLength,
            pkgHeader.package_header_format_version);
    }
    if (diagnostic != nullptr)
    {
        diagnostic->error = PackageHeaderError::invalid;
    }
    return nullptr;
}

} // namespace fw_update

} // namespace pldm
