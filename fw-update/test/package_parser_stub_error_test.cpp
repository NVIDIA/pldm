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

#include "fw-update/package_parser.hpp"

#include <array>
#include <cstring>

#include <gtest/gtest.h>

using namespace pldm::fw_update;

namespace
{

enum class StubMode
{
    success,
    deviceRecordError,
    descriptorError,
    vendorDecodeError,
    compImageError,
    headerDecodeError,
    headerInvalidUuid,
    headerUnsupportedVersion,
    payloadChecksumMismatch,
};

StubMode stubMode = StubMode::success;
uint16_t stubRecordLength = 8;
uint8_t stubDescriptorCount = 0;
size_t stubRecordDescriptorsLength = 4;

class PackageParserProbe : public PackageParser
{
  public:
    using PackageParser::componentImageInfos;

    PackageParserProbe(PackageHeaderSize pkgHeaderSize,
                       const PackageVersion& pkgVersion,
                       ComponentBitmapBitLength componentBitmapBitLength,
                       uint8_t formatVersion) :
        PackageParser(pkgHeaderSize, pkgVersion, componentBitmapBitLength,
                      formatVersion)
    {}

    size_t callParseFD(DeviceIDRecordCount deviceIdRecCount,
                       const std::vector<uint8_t>& pkgHdr, size_t offset,
                       uint8_t formatVersion)
    {
        return parseFDIdentificationArea(deviceIdRecCount, pkgHdr, offset,
                                         formatVersion);
    }

    size_t callParseComp(ComponentImageCount compImageCount,
                         const std::vector<uint8_t>& pkgHdr, size_t offset,
                         uint8_t formatVersion)
    {
        return parseCompImageInfoArea(compImageCount, pkgHdr, offset,
                                      formatVersion);
    }

    void callValidatePkgSize(uintmax_t pkgSize)
    {
        validatePkgTotalSize(pkgSize);
    }
};

} // namespace

extern "C"
{
int decode_firmware_device_id_record(
    const uint8_t*, size_t, uint16_t,
    struct pldm_firmware_device_id_record* rec,
    struct variable_field* applicableComponents,
    struct variable_field* compImageSetVersionStr,
    struct variable_field* recordDescriptors,
    struct variable_field* fwDevicePkgData, size_t)
{
    if (stubMode == StubMode::deviceRecordError)
    {
        return PLDM_ERROR_INVALID_DATA;
    }

    static const uint8_t version[] = {'v'};
    static const uint8_t descriptors[] = {0x00, 0x00, 0x00, 0x00};
    static const uint8_t compBitmap[] = {0x01};

    rec->record_length = stubRecordLength;
    rec->descriptor_count = stubDescriptorCount;
    rec->comp_image_set_version_string_length = sizeof(version);
    rec->fw_device_pkg_data_length = 0;
    applicableComponents->ptr = compBitmap;
    applicableComponents->length = sizeof(compBitmap);
    compImageSetVersionStr->ptr = version;
    compImageSetVersionStr->length = sizeof(version);
    recordDescriptors->ptr = descriptors;
    recordDescriptors->length = stubRecordDescriptorsLength;
    fwDevicePkgData->ptr = nullptr;
    fwDevicePkgData->length = 0;
    return PLDM_SUCCESS;
}

int decode_descriptor_type_length_value(const uint8_t*, size_t,
                                        uint16_t* descriptorType,
                                        struct variable_field* descriptorData)
{
    if (stubMode == StubMode::descriptorError)
    {
        return PLDM_ERROR_INVALID_DATA;
    }

    static const uint8_t descriptorBytes[] = {0x01, 0x02, 0x03, 0x04};
    if (stubMode == StubMode::vendorDecodeError)
    {
        *descriptorType = PLDM_FWUP_VENDOR_DEFINED;
        descriptorData->ptr = descriptorBytes;
        descriptorData->length = sizeof(descriptorBytes);
        return PLDM_SUCCESS;
    }

    *descriptorType = PLDM_FWUP_IANA_ENTERPRISE_ID;
    descriptorData->ptr = descriptorBytes;
    descriptorData->length = sizeof(descriptorBytes);
    return PLDM_SUCCESS;
}

int decode_vendor_defined_descriptor_value(const uint8_t*, size_t, uint8_t*,
                                           struct variable_field*,
                                           struct variable_field*)
{
    if (stubMode == StubMode::vendorDecodeError)
    {
        return PLDM_ERROR_INVALID_DATA;
    }
    return PLDM_SUCCESS;
}

int decode_pldm_comp_image_info(const uint8_t*, size_t,
                                struct pldm_component_image_information* info,
                                struct variable_field* compVersion)
{
    if (stubMode == StubMode::compImageError)
    {
        return PLDM_ERROR_INVALID_DATA;
    }

    static const uint8_t version[] = {'c'};
    info->comp_classification = 1;
    info->comp_identifier = 1;
    info->comp_comparison_stamp =
        PLDM_FWUP_INVALID_COMPONENT_COMPARISON_TIMESTAMP;
    info->comp_options.value = 0;
    info->requested_comp_activation_method.value = 0;
    info->comp_location_offset = 32;
    info->comp_size = 4;
    info->comp_version_string_type = PLDM_STR_TYPE_ASCII;
    info->comp_version_string_length = sizeof(version);
    compVersion->ptr = version;
    compVersion->length = sizeof(version);
    return PLDM_SUCCESS;
}

int decode_pldm_package_header_info(
    const uint8_t*, size_t, struct pldm_package_header_information* headerInfo,
    struct variable_field* packageVersionStr)
{
    if (stubMode == StubMode::headerDecodeError)
    {
        return PLDM_ERROR_INVALID_DATA;
    }

    static const uint8_t version[] = {'v'};
    static const std::array<uint8_t, PLDM_FWUP_UUID_LENGTH> uuidV10 =
        PLDM_PACKAGE_HEADER_IDENTIFIER_V1_0;

    std::memset(headerInfo, 0, sizeof(*headerInfo));
    if (stubMode == StubMode::headerInvalidUuid)
    {
        std::memset(headerInfo->uuid, 0xAA, PLDM_FWUP_UUID_LENGTH);
    }
    else
    {
        std::memcpy(headerInfo->uuid, uuidV10.data(), PLDM_FWUP_UUID_LENGTH);
    }
    if (stubMode == StubMode::headerUnsupportedVersion)
    {
        headerInfo->package_header_format_version = 0xFF;
    }
    else
    {
        headerInfo->package_header_format_version =
            PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR01H;
    }
    headerInfo->package_header_size = 64;
    headerInfo->component_bitmap_bit_length = 8;

    packageVersionStr->ptr = version;
    packageVersionStr->length = sizeof(version);
    return PLDM_SUCCESS;
}

uint32_t pldm_edac_crc32(const void*, size_t)
{
    if (stubMode == StubMode::payloadChecksumMismatch)
    {
        return 0;
    }
    return 1;
}

} // extern "C"

TEST(PackageParserStubErrorTest, parseFDDeviceRecordDecodeFailureThrows)
{
    stubMode = StubMode::deviceRecordError;
    PackageParserProbe parser(32, "v", 8,
                              PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR01H);
    std::vector<uint8_t> data{0x00};

    EXPECT_ANY_THROW(parser.callParseFD(
        1, data, 0, PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR01H));
}

TEST(PackageParserStubErrorTest,
     parseFDSkipsDescriptorLoopWhenDescriptorLengthIsZero)
{
    stubMode = StubMode::success;
    stubDescriptorCount = 1;
    stubRecordLength = 10;
    stubRecordDescriptorsLength = 0;
    PackageParserProbe parser(32, "v", 8,
                              PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR01H);
    std::vector<uint8_t> data(32, 0);

    auto offset = parser.callParseFD(1, data, 0,
                                     PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR01H);
    EXPECT_EQ(offset, stubRecordLength);

    stubDescriptorCount = 0;
    stubRecordLength = 8;
    stubRecordDescriptorsLength = 4;
}

TEST(PackageParserStubErrorTest, parseDescriptorDecodeFailureThrows)
{
    stubMode = StubMode::descriptorError;
    stubDescriptorCount = 1;
    stubRecordLength = 8;
    PackageParserProbe parser(64, "v", 8,
                              PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR01H);

    std::vector<uint8_t> data(64, 0);
    auto baseOffset =
        sizeof(pldm_package_header_information) + parser.pkgVersion.size();
    data[baseOffset] = 1;

    EXPECT_ANY_THROW(parser.parse(data.data(), data.size()));
    stubDescriptorCount = 0;
}

TEST(PackageParserStubErrorTest, parseVendorDescriptorDecodeFailureThrows)
{
    stubMode = StubMode::vendorDecodeError;
    stubDescriptorCount = 1;
    stubRecordLength = 8;
    PackageParserProbe parser(64, "v", 8,
                              PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR01H);

    std::vector<uint8_t> data(64, 0);
    auto baseOffset =
        sizeof(pldm_package_header_information) + parser.pkgVersion.size();
    data[baseOffset] = 1;

    EXPECT_ANY_THROW(parser.parse(data.data(), data.size()));
    stubDescriptorCount = 0;
}

TEST(PackageParserStubErrorTest, parseCompImageDecodeFailureThrows)
{
    stubMode = StubMode::compImageError;
    PackageParserProbe parser(32, "v", 8,
                              PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR01H);
    std::vector<uint8_t> data{0x00};

    EXPECT_ANY_THROW(parser.callParseComp(
        1, data, 0, PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR01H));
}

TEST(PackageParserStubErrorTest,
     parseCompImageLoopSkipsWhenHeaderRemainingSizeIsZero)
{
    stubMode = StubMode::success;
    PackageParserProbe parser(32, "v", 8,
                              PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR01H);
    std::vector<uint8_t> data{};

    auto offset = parser.callParseComp(
        1, data, 0, PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR01H);
    EXPECT_EQ(offset, 0);
}

TEST(PackageParserStubErrorTest, parseCompImageFr04AddsOpaqueDataLengthToOffset)
{
    stubMode = StubMode::success;
    PackageParserProbe parser(64, "v", 8,
                              PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR04H);
    std::vector<uint8_t> data(8, 0);

    auto offset = parser.callParseComp(
        1, data, 0, PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR04H);
    EXPECT_EQ(offset, sizeof(pldm_component_image_information) + 1 +
                          sizeof(CompOpaqueDataLength));
}

TEST(PackageParserStubErrorTest, parseDetectsDeviceIdRecordCountMismatch)
{
    stubMode = StubMode::success;
    stubDescriptorCount = 0;
    PackageParserProbe parser(64, "v", 8,
                              PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR01H);
    auto baseOffset =
        sizeof(pldm_package_header_information) + parser.pkgVersion.size();

    std::vector<uint8_t> data(64, 0);
    data[baseOffset] = 2;
    stubRecordLength = static_cast<uint16_t>(data.size() - (baseOffset + 1));

    EXPECT_ANY_THROW(parser.parse(data.data(), data.size()));
}

TEST(PackageParserStubErrorTest, parseRejectsDownstreamDeviceRecords)
{
    stubMode = StubMode::success;
    PackageParserProbe parser(64, "v", 8,
                              PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR03H);
    auto baseOffset =
        sizeof(pldm_package_header_information) + parser.pkgVersion.size();

    std::vector<uint8_t> data(64, 0);
    data[baseOffset] = 0;
    data[baseOffset + 1] = 1;

    EXPECT_ANY_THROW(parser.parse(data.data(), data.size()));
}

TEST(PackageParserStubErrorTest, parseRejectsMissingComponentImageCountField)
{
    stubMode = StubMode::success;
    constexpr std::string_view version{"v"};
    const auto baseOffset =
        sizeof(pldm_package_header_information) + version.size();
    PackageParserProbe parser(static_cast<PackageHeaderSize>(baseOffset + 3),
                              std::string(version), 8,
                              PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR01H);
    std::vector<uint8_t> data(parser.pkgHeaderSize, 0);
    data[baseOffset] = 0;

    EXPECT_ANY_THROW(parser.parse(data.data(), data.size()));
}

TEST(PackageParserStubErrorTest, parseRejectsMissingDeviceIdRecordCountField)
{
    stubMode = StubMode::success;
    constexpr std::string_view version{"v"};
    const auto baseOffset =
        sizeof(pldm_package_header_information) + version.size();
    PackageParserProbe parser(static_cast<PackageHeaderSize>(baseOffset + 1),
                              std::string(version), 8,
                              PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR01H);
    std::vector<uint8_t> data(parser.pkgHeaderSize, 0);

    EXPECT_ANY_THROW(parser.parse(data.data(), data.size()));
}

TEST(PackageParserStubErrorTest, parseDetectsComponentCountMismatch)
{
    stubMode = StubMode::success;
    PackageParserProbe parser(64, "v", 8,
                              PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR01H);
    parser.componentImageInfos.emplace_back(std::make_tuple(
        static_cast<uint16_t>(1), static_cast<uint16_t>(1),
        static_cast<uint32_t>(0), CompOptions{}, ReqCompActivationMethod{},
        static_cast<uint32_t>(32), static_cast<uint32_t>(4), std::string("x")));

    auto baseOffset =
        sizeof(pldm_package_header_information) + parser.pkgVersion.size();
    std::vector<uint8_t> data(64, 0);
    data[baseOffset] = 0;
    data[baseOffset + 1] = 0;
    data[baseOffset + 2] = 0;

    EXPECT_ANY_THROW(parser.parse(data.data(), data.size()));
}

TEST(PackageParserStubErrorTest, parseFr04PayloadChecksumMismatchThrows)
{
    stubMode = StubMode::payloadChecksumMismatch;
    constexpr std::string_view version{"v"};
    const auto baseOffset =
        sizeof(pldm_package_header_information) + version.size();
    const auto headerSize = static_cast<PackageHeaderSize>(baseOffset + 10);
    PackageParserProbe parser(headerSize, std::string(version), 8,
                              PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR04H);

    std::vector<uint8_t> data(headerSize, 0);
    data[baseOffset] = 0;
    data[baseOffset + 1] = 0;
    data[baseOffset + 2] = 0;
    data[baseOffset + 3] = 0;
    data[baseOffset + 6] = 1;

    EXPECT_ANY_THROW(parser.parse(data.data(), data.size()));
}

TEST(PackageParserStubErrorTest, calculatePackageSizeOffsetMismatchThrows)
{
    stubMode = StubMode::success;
    PackageParserProbe parser(32, "v", 8,
                              PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR01H);
    parser.componentImageInfos.emplace_back(
        std::make_tuple(static_cast<uint16_t>(1), static_cast<uint16_t>(1),
                        static_cast<uint32_t>(0), CompOptions{},
                        ReqCompActivationMethod{}, static_cast<uint32_t>(40),
                        static_cast<uint32_t>(4), std::string("comp")));

    EXPECT_ANY_THROW(parser.calculatePackageSize());
}

TEST(PackageParserStubErrorTest, parsePkgHeaderDecodeFailureReturnsNull)
{
    stubMode = StubMode::headerDecodeError;
    std::vector<uint8_t> data(sizeof(pldm_package_header_information), 0);
    EXPECT_EQ(parsePkgHeader(data.data(), data.size()), nullptr);
}

TEST(PackageParserStubErrorTest, parsePkgHeaderNullDataReturnsNull)
{
    stubMode = StubMode::success;
    EXPECT_EQ(parsePkgHeader(nullptr, sizeof(pldm_package_header_information)),
              nullptr);
}

TEST(PackageParserStubErrorTest, parsePkgHeaderUnsupportedVersionReturnsNull)
{
    stubMode = StubMode::headerUnsupportedVersion;
    std::vector<uint8_t> data(sizeof(pldm_package_header_information), 0);
    EXPECT_EQ(parsePkgHeader(data.data(), data.size()), nullptr);
}

TEST(PackageParserStubErrorTest, parsePkgHeaderInvalidUuidReturnsNull)
{
    stubMode = StubMode::headerInvalidUuid;
    std::vector<uint8_t> data(sizeof(pldm_package_header_information), 0);
    EXPECT_EQ(parsePkgHeader(data.data(), data.size()), nullptr);
}

TEST(PackageParserStubErrorTest,
     parseRejectsWhenPkgDataIsNullAndHeaderLargerThanPackage)
{
    stubMode = StubMode::success;
    PackageParserProbe parser(64, "v", 8,
                              PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR01H);
    EXPECT_ANY_THROW(parser.parse(nullptr, 8));
}
