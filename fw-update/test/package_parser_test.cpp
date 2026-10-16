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

#include <libpldm/edac.h>

#include <xyz/openbmc_project/Common/error.hpp>
#include <xyz/openbmc_project/Software/Update/error.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <typeinfo>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldm::fw_update;

void imageGenerate(std::vector<uint8_t>& image, size_t size)
{
    for (size_t i = 1; i <= size; ++i)
    {
        image.push_back(static_cast<uint8_t>(i));
    }
}

void imageInsert(std::vector<uint8_t>& fwPkgHdr, std::vector<uint8_t>& image)
{
    // Insert the image at the end of the package header
    fwPkgHdr.insert(fwPkgHdr.end(), image.begin(), image.end());
}

TEST(PackageParser, ValidPkgSingleDescriptorSingleComponent)
{
    std::vector<uint8_t> fwPkgHdr{
        0xF0, 0x18, 0x87, 0x8C, 0xCB, 0x7D, 0x49, 0x43, 0x98, 0x00, 0xA0, 0x2F,
        0x05, 0x9A, 0xCA, 0x02, 0x01, 0x8B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x19, 0x0C, 0xE5, 0x07, 0x00, 0x08, 0x00, 0x01, 0x0E,
        0x56, 0x65, 0x72, 0x73, 0x69, 0x6F, 0x6E, 0x53, 0x74, 0x72, 0x69, 0x6E,
        0x67, 0x31, 0x01, 0x2E, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0E,
        0x00, 0x00, 0x01, 0x56, 0x65, 0x72, 0x73, 0x69, 0x6F, 0x6E, 0x53, 0x74,
        0x72, 0x69, 0x6E, 0x67, 0x32, 0x02, 0x00, 0x10, 0x00, 0x16, 0x20, 0x23,
        0xC9, 0x3E, 0xC5, 0x41, 0x15, 0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
        0x75, 0x01, 0x00, 0x0A, 0x00, 0x64, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
        0x00, 0x00, 0x00, 0x8B, 0x00, 0x00, 0x00, 0x1B, 0x00, 0x00, 0x00, 0x01,
        0x0E, 0x56, 0x65, 0x72, 0x73, 0x69, 0x6F, 0x6E, 0x53, 0x74, 0x72, 0x69,
        0x6E, 0x67, 0x33, 0x4F, 0x96, 0xAE, 0x56};

    constexpr uintmax_t pkgSize = 166;
    constexpr uintmax_t pkgHeaderSize = 139;
    constexpr uintmax_t pkgImageSize = pkgSize - pkgHeaderSize;
    std::vector<uint8_t> compImage;
    imageGenerate(compImage, pkgImageSize);
    imageInsert(fwPkgHdr, compImage);
    constexpr std::string_view pkgVersion{"VersionString1"};
    auto parser = parsePkgHeader(fwPkgHdr.data(), fwPkgHdr.size());
    auto obj = parser.get();
    EXPECT_EQ(typeid(*obj).name(), typeid(PackageParser).name());
    EXPECT_EQ(parser->pkgHeaderSize, pkgHeaderSize);
    EXPECT_EQ(parser->pkgVersion, pkgVersion);

    parser->parse(fwPkgHdr, pkgSize);
    auto outfwDeviceIDRecords = parser->getFwDeviceIDRecords();
    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {0},
         "VersionString2",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}}},
         {}},
    };
    EXPECT_EQ(outfwDeviceIDRecords, fwDeviceIDRecords);

    auto outCompImageInfos = parser->getComponentImageInfos();
    ComponentImageInfos compImageInfos{
        {10, 100, 0xFFFFFFFF, 0, 0, 139, 27, "VersionString3"}};
    EXPECT_EQ(outCompImageInfos, compImageInfos);
}

TEST(PackageParser, ValidPkgMultipleDescriptorsMultipleComponents)
{
    std::vector<uint8_t> fwPkgHdr{
        0xF0, 0x18, 0x87, 0x8C, 0xCB, 0x7D, 0x49, 0x43, 0x98, 0x00, 0xA0, 0x2F,
        0x05, 0x9A, 0xCA, 0x02, 0x01, 0x46, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x19, 0x0C, 0xE5, 0x07, 0x00, 0x08, 0x00, 0x01, 0x0E,
        0x56, 0x65, 0x72, 0x73, 0x69, 0x6F, 0x6E, 0x53, 0x74, 0x72, 0x69, 0x6E,
        0x67, 0x31, 0x03, 0x45, 0x00, 0x03, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0E,
        0x00, 0x00, 0x03, 0x56, 0x65, 0x72, 0x73, 0x69, 0x6F, 0x6E, 0x53, 0x74,
        0x72, 0x69, 0x6E, 0x67, 0x32, 0x02, 0x00, 0x10, 0x00, 0x12, 0x44, 0xD2,
        0x64, 0x8D, 0x7D, 0x47, 0x18, 0xA0, 0x30, 0xFC, 0x8A, 0x56, 0x58, 0x7D,
        0x5B, 0x01, 0x00, 0x04, 0x00, 0x47, 0x16, 0x00, 0x00, 0xFF, 0xFF, 0x0B,
        0x00, 0x01, 0x07, 0x4F, 0x70, 0x65, 0x6E, 0x42, 0x4D, 0x43, 0x12, 0x34,
        0x2E, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x0E, 0x00, 0x00, 0x07,
        0x56, 0x65, 0x72, 0x73, 0x69, 0x6F, 0x6E, 0x53, 0x74, 0x72, 0x69, 0x6E,
        0x67, 0x33, 0x02, 0x00, 0x10, 0x00, 0x12, 0x44, 0xD2, 0x64, 0x8D, 0x7D,
        0x47, 0x18, 0xA0, 0x30, 0xFC, 0x8A, 0x56, 0x58, 0x7D, 0x5C, 0x2E, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x0E, 0x00, 0x00, 0x01, 0x56, 0x65,
        0x72, 0x73, 0x69, 0x6F, 0x6E, 0x53, 0x74, 0x72, 0x69, 0x6E, 0x67, 0x34,
        0x02, 0x00, 0x10, 0x00, 0x12, 0x44, 0xD2, 0x64, 0x8D, 0x7D, 0x47, 0x18,
        0xA0, 0x30, 0xFC, 0x8A, 0x56, 0x58, 0x7D, 0x5D, 0x03, 0x00, 0x0A, 0x00,
        0x64, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x46, 0x01,
        0x00, 0x00, 0x1B, 0x00, 0x00, 0x00, 0x01, 0x0E, 0x56, 0x65, 0x72, 0x73,
        0x69, 0x6F, 0x6E, 0x53, 0x74, 0x72, 0x69, 0x6E, 0x67, 0x35, 0x0A, 0x00,
        0xC8, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x01, 0x00, 0x61, 0x01,
        0x00, 0x00, 0x1B, 0x00, 0x00, 0x00, 0x01, 0x0E, 0x56, 0x65, 0x72, 0x73,
        0x69, 0x6F, 0x6E, 0x53, 0x74, 0x72, 0x69, 0x6E, 0x67, 0x36, 0x0b, 0x00,
        0x2C, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x0C, 0x00, 0x7C, 0x01,
        0x00, 0x00, 0x1B, 0x00, 0x00, 0x00, 0x01, 0x0E, 0x56, 0x65, 0x72, 0x73,
        0x69, 0x6F, 0x6E, 0x53, 0x74, 0x72, 0x69, 0x6E, 0x67, 0x37};

    const uint32_t check = pldm_edac_crc32(fwPkgHdr.data(), fwPkgHdr.size());

    const auto checksum =
        std::bit_cast<std::array<uint8_t, sizeof(check)>>(check);
    fwPkgHdr.insert(fwPkgHdr.end(), checksum.begin(), checksum.end());

    const uintmax_t pkgHeaderSize = fwPkgHdr.size();
    EXPECT_EQ(pkgHeaderSize, 326);

    constexpr uintmax_t pkgSize = 407;
    const uintmax_t pkgImageSize = (pkgSize - pkgHeaderSize) / 3;
    std::vector<uint8_t> compImage1, compImage2, compImage3;
    imageGenerate(compImage1, pkgImageSize);
    imageGenerate(compImage2, pkgImageSize);
    imageGenerate(compImage3, pkgImageSize);
    imageInsert(fwPkgHdr, compImage1);
    imageInsert(fwPkgHdr, compImage2);
    imageInsert(fwPkgHdr, compImage3);
    constexpr std::string_view pkgVersion{"VersionString1"};
    auto parser = parsePkgHeader(fwPkgHdr.data(), fwPkgHdr.size());
    auto obj = parser.get();
    EXPECT_EQ(typeid(*obj).name(), typeid(PackageParser).name());
    EXPECT_EQ(parser->pkgHeaderSize, pkgHeaderSize);
    EXPECT_EQ(parser->pkgVersion, pkgVersion);

    parser->parse(fwPkgHdr, pkgSize);
    auto outfwDeviceIDRecords = parser->getFwDeviceIDRecords();
    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {0, 1},
         "VersionString2",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x12, 0x44, 0xD2, 0x64, 0x8D, 0x7D, 0x47, 0x18,
                                0xA0, 0x30, 0xFC, 0x8A, 0x56, 0x58, 0x7D,
                                0x5B}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x12, 0x34})}},
         {}},
        {0,
         {0, 1, 2},
         "VersionString3",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x12, 0x44, 0xD2, 0x64, 0x8D, 0x7D, 0x47, 0x18,
                                0xA0, 0x30, 0xFC, 0x8A, 0x56, 0x58, 0x7D,
                                0x5C}}},
         {}},
        {0,
         {0},
         "VersionString4",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x12, 0x44, 0xD2, 0x64, 0x8D, 0x7D, 0x47, 0x18,
                                0xA0, 0x30, 0xFC, 0x8A, 0x56, 0x58, 0x7D,
                                0x5D}}},
         {}},
    };
    EXPECT_EQ(outfwDeviceIDRecords, fwDeviceIDRecords);

    auto outCompImageInfos = parser->getComponentImageInfos();
    ComponentImageInfos compImageInfos{
        {10, 100, 0xFFFFFFFF, 0, 0, 326, 27, "VersionString5"},
        {10, 200, 0xFFFFFFFF, 0, 1, 353, 27, "VersionString6"},
        {11, 300, 0xFFFFFFFF, 1, 12, 380, 27, "VersionString7"}};
    EXPECT_EQ(outCompImageInfos, compImageInfos);
}

namespace
{

constexpr uintmax_t downstreamPkgHeaderSize = 158;
constexpr uintmax_t downstreamPkgSize = 174;

std::vector<uint8_t> validDownstreamPkg()
{
    // FR04H (v1.3) package built with tools/fw-update/pldm_fwup_pkg_creator.py
    // from a metadata file with one FirmwareDeviceIDRecord and one
    // DownstreamDeviceIdentificationArea record (DSP0267 v1.3.0 Table 5),
    // and round-tripped back through tools/fw-update/fwpkg_unpack.py to
    // confirm the bytes below decode to the same downstream record the
    // metadata described.
    std::vector<uint8_t> fwPkgHdr{
        0x7B, 0x29, 0x1C, 0x99, 0x6D, 0xB6, 0x42, 0x08, 0x80, 0x1B, 0x02, 0x02,
        0x6E, 0x46, 0x3C, 0x78, 0x04, 0x9E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x0D, 0x0C, 0xE7, 0x07, 0x00, 0x08, 0x00, 0x01, 0x02,
        0x56, 0x31, 0x01, 0x28, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x04,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x46, 0x44, 0x56, 0x31, 0x02,
        0x00, 0x10, 0x00, 0x12, 0x44, 0xD2, 0x64, 0x8D, 0x7D, 0x47, 0x18, 0xA0,
        0x30, 0xFC, 0x8A, 0x56, 0x58, 0x7D, 0x5B, 0x01, 0x24, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x02, 0x00, 0x10, 0x00, 0x12, 0x44, 0xD2, 0x64, 0x8D, 0x7D, 0x47, 0x18,
        0xA0, 0x30, 0xFC, 0x8A, 0x56, 0x58, 0x7D, 0x5E, 0x01, 0x00, 0x0A, 0x00,
        0x64, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x9E, 0x00,
        0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x01, 0x06, 0x43, 0x6F, 0x6D, 0x70,
        0x56, 0x31, 0x00, 0x00, 0x00, 0x00, 0x5F, 0x87, 0x0C, 0x6D, 0x04, 0xCF,
        0xF9, 0x9C};

    // The exact component image tools/fw-update/pldm_fwup_pkg_creator.py
    // packed when the header bytes above (and their payload checksum) were
    // generated.
    std::vector<uint8_t> compImage{0x5C, 0x73, 0x6B, 0x2A, 0xA4, 0x6D,
                                   0x76, 0xB4, 0x86, 0x2E, 0x10, 0x09,
                                   0xB8, 0x6D, 0x8E, 0xC2};
    imageInsert(fwPkgHdr, compImage);
    return fwPkgHdr;
}

} // namespace

TEST(PackageParser, ValidPkgWithDownstreamDeviceIDRecord)
{
    auto fwPkgHdr = validDownstreamPkg();
    constexpr uintmax_t pkgHeaderSize = downstreamPkgHeaderSize;
    constexpr uintmax_t pkgSize = downstreamPkgSize;
    ASSERT_EQ(fwPkgHdr.size(), pkgSize);

    auto parser = parsePkgHeader(fwPkgHdr.data(), fwPkgHdr.size());
    ASSERT_NE(parser, nullptr);
    EXPECT_EQ(parser->pkgHeaderSize, pkgHeaderSize);
    EXPECT_EQ(parser->pkgVersion, "V1");
    EXPECT_EQ(parser->getFormatVersion(),
              PLDM_PACKAGE_HEADER_FORMAT_REVISION_FR04H);

    parser->parse(fwPkgHdr, pkgSize);

    DownstreamDeviceIDRecords downstreamDeviceIDRecords{
        {0,
         {0},
         {{2, std::vector<uint8_t>{0x12, 0x44, 0xD2, 0x64, 0x8D, 0x7D, 0x47,
                                   0x18, 0xA0, 0x30, 0xFC, 0x8A, 0x56, 0x58,
                                   0x7D, 0x5E}}},
         {}},
    };
    EXPECT_EQ(parser->getDownstreamDeviceIDRecords(),
              downstreamDeviceIDRecords);
    EXPECT_EQ(parser->getFwDeviceIDRecords().size(), 1u);
    EXPECT_EQ(parser->getComponentImageInfos().size(), 1u);
}

namespace
{

using InternalFailure =
    sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure;

constexpr std::array<uint8_t, 16> downstreamUuid{
    0x12, 0x44, 0xD2, 0x64, 0x8D, 0x7D, 0x47, 0x18,
    0xA0, 0x30, 0xFC, 0x8A, 0x56, 0x58, 0x7D, 0x5E};

void putLe(std::vector<uint8_t>& out, uint32_t value, size_t bytes)
{
    for (size_t i = 0; i < bytes; i++)
    {
        out.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}

std::vector<uint8_t> defaultDescriptors()
{
    std::vector<uint8_t> desc;
    putLe(desc, PLDM_FWUP_UUID, 2);
    putLe(desc, downstreamUuid.size(), 2);
    desc.insert(desc.end(), downstreamUuid.begin(), downstreamUuid.end());
    return desc;
}

/** @brief One DownstreamDeviceIDRecord (DSP0267 v1.3.0 Table 5) with every
 *         length field overridable so malformed records can be produced. */
struct DownstreamRecordSpec
{
    uint8_t descriptorCount = 1;
    uint32_t flags = 0;
    std::string selfContainedMinVersion;
    std::optional<uint8_t> selfContainedMinVersionLenField;
    bool omitComparisonStamp = false;
    uint8_t applicable = 0x01;
    std::vector<uint8_t> descriptors = defaultDescriptors();
    std::vector<uint8_t> packageData;
    std::optional<uint16_t> packageDataLenField;
    std::vector<uint8_t> referenceManifest;
    std::optional<uint32_t> referenceManifestLenField;
    std::vector<uint8_t> trailing;
    std::optional<uint16_t> recordLengthField;

    std::vector<uint8_t> serialize() const
    {
        std::vector<uint8_t> body;
        body.push_back(descriptorCount);
        putLe(body, flags, 4);
        body.push_back(0);
        body.push_back(selfContainedMinVersionLenField.value_or(
            static_cast<uint8_t>(selfContainedMinVersion.size())));
        putLe(body, packageDataLenField.value_or(packageData.size()), 2);
        putLe(body,
              referenceManifestLenField.value_or(referenceManifest.size()), 4);
        body.push_back(applicable);
        body.insert(body.end(), selfContainedMinVersion.begin(),
                    selfContainedMinVersion.end());
        if ((flags & 1) && !omitComparisonStamp)
        {
            putLe(body, 0xCAFEF00D, 4);
        }
        body.insert(body.end(), descriptors.begin(), descriptors.end());
        body.insert(body.end(), packageData.begin(), packageData.end());
        body.insert(body.end(), referenceManifest.begin(),
                    referenceManifest.end());
        body.insert(body.end(), trailing.begin(), trailing.end());

        std::vector<uint8_t> record;
        putLe(record, recordLengthField.value_or(body.size() + 2), 2);
        record.insert(record.end(), body.begin(), body.end());
        return record;
    }
};

/** @brief Recompute PackageHeaderChecksum (it precedes the 4-byte
 *         PackagePayloadChecksum) after the header bytes were edited. */
void refreshHeaderChecksum(std::vector<uint8_t>& pkg)
{
    constexpr size_t hdrSizeIdx = 17;
    constexpr size_t trailerLen = 8;
    size_t hdrSize = pkg[hdrSizeIdx] | (pkg[hdrSizeIdx + 1] << 8);
    uint32_t hdrCrc = pldm_edac_crc32(pkg.data(), hdrSize - trailerLen);
    for (size_t i = 0; i < sizeof(hdrCrc); i++)
    {
        pkg[hdrSize - trailerLen + i] = static_cast<uint8_t>(hdrCrc >> (8 * i));
    }
}

/** @brief Re-assemble validDownstreamPkg() with its DownstreamDevice
 *         identification area replaced. The header size, the component
 *         image location offset and the header checksum are patched to follow
 *         the new area; the payload (and so its checksum) is untouched. */
std::vector<uint8_t> pkgWithDownstreamArea(uint8_t recordCount,
                                           const std::vector<uint8_t>& area)
{
    constexpr size_t countIdx = 79;
    constexpr size_t origAreaLen = 36;
    constexpr size_t hdrSizeIdx = 17;
    constexpr size_t compLocationIdx = countIdx + 1 + origAreaLen + 2 + 12;

    auto base = validDownstreamPkg();
    std::vector<uint8_t> pkg(base.begin(), base.begin() + countIdx);
    pkg.push_back(recordCount);
    pkg.insert(pkg.end(), area.begin(), area.end());
    pkg.insert(pkg.end(), base.begin() + countIdx + 1 + origAreaLen,
               base.end());

    uint16_t hdrSize = downstreamPkgHeaderSize - origAreaLen + area.size();
    pkg[hdrSizeIdx] = hdrSize & 0xFF;
    pkg[hdrSizeIdx + 1] = hdrSize >> 8;
    size_t newLocationIdx = compLocationIdx - origAreaLen + area.size();
    pkg[newLocationIdx] = hdrSize & 0xFF;
    pkg[newLocationIdx + 1] = hdrSize >> 8;

    refreshHeaderChecksum(pkg);
    return pkg;
}

std::vector<uint8_t> pkgWithRecords(const std::vector<DownstreamRecordSpec>& rs)
{
    std::vector<uint8_t> area;
    for (const auto& r : rs)
    {
        auto bytes = r.serialize();
        area.insert(area.end(), bytes.begin(), bytes.end());
    }
    return pkgWithDownstreamArea(static_cast<uint8_t>(rs.size()), area);
}

std::unique_ptr<PackageParser> parseDownstreamPkg(
    const std::vector<uint8_t>& pkg)
{
    auto parser = parsePkgHeader(pkg.data(), pkg.size());
    EXPECT_NE(parser, nullptr);
    if (parser)
    {
        parser->parse(pkg, pkg.size());
    }
    return parser;
}

} // namespace

TEST(PackageParser, DownstreamRecordBuilderReproducesValidPkg)
{
    auto pkg = pkgWithRecords({DownstreamRecordSpec{}});
    EXPECT_EQ(pkg, validDownstreamPkg());
}

TEST(PackageParser, DownstreamRecordWithAllOptionalFields)
{
    DownstreamRecordSpec rec;
    rec.flags = 1;
    rec.selfContainedMinVersion = "1.2";
    rec.packageData = {0xA1, 0xA2, 0xA3};
    rec.referenceManifest = {0xB1, 0xB2};
    auto pkg = pkgWithRecords({rec});

    auto parser = parseDownstreamPkg(pkg);
    ASSERT_NE(parser, nullptr);
    ASSERT_EQ(parser->getDownstreamDeviceIDRecords().size(), 1u);
    const auto& out = parser->getDownstreamDeviceIDRecords()[0];
    EXPECT_EQ(std::get<DownstreamDeviceUpdateOptionFlags>(out), 1u);
    EXPECT_EQ(std::get<DownstreamApplicableComponents>(out),
              DownstreamApplicableComponents{0});
    EXPECT_EQ(std::get<DownstreamPackageData>(out),
              (DownstreamPackageData{0xA1, 0xA2, 0xA3}));
}

TEST(PackageParser, DownstreamMultipleRecords)
{
    DownstreamRecordSpec first;
    DownstreamRecordSpec second;
    second.packageData = {0x55};
    auto pkg = pkgWithRecords({first, second});

    auto parser = parseDownstreamPkg(pkg);
    ASSERT_NE(parser, nullptr);
    ASSERT_EQ(parser->getDownstreamDeviceIDRecords().size(), 2u);
    EXPECT_TRUE(std::get<DownstreamPackageData>(
                    parser->getDownstreamDeviceIDRecords()[0])
                    .empty());
    EXPECT_EQ(std::get<DownstreamPackageData>(
                  parser->getDownstreamDeviceIDRecords()[1]),
              DownstreamPackageData{0x55});
}

TEST(PackageParser, DownstreamVendorDefinedDescriptor)
{
    DownstreamRecordSpec rec;
    std::vector<uint8_t> vendorData{0x01, 0x03, 'a', 'b', 'c', 0xDE, 0xAD};
    rec.descriptors.clear();
    putLe(rec.descriptors, PLDM_FWUP_VENDOR_DEFINED, 2);
    putLe(rec.descriptors, vendorData.size(), 2);
    rec.descriptors.insert(rec.descriptors.end(), vendorData.begin(),
                           vendorData.end());
    auto pkg = pkgWithRecords({rec});

    auto parser = parseDownstreamPkg(pkg);
    ASSERT_NE(parser, nullptr);
    ASSERT_EQ(parser->getDownstreamDeviceIDRecords().size(), 1u);
    const auto& descriptors =
        std::get<Descriptors>(parser->getDownstreamDeviceIDRecords()[0]);
    auto it = descriptors.find(PLDM_FWUP_VENDOR_DEFINED);
    ASSERT_NE(it, descriptors.end());
    auto info = std::get<VendorDefinedDescriptorInfo>(it->second);
    EXPECT_EQ(std::get<VendorDefinedDescriptorTitle>(info), "abc");
    EXPECT_EQ(std::get<VendorDefinedDescriptorData>(info),
              (VendorDefinedDescriptorData{0xDE, 0xAD}));
}

TEST(PackageParser, DownstreamOutOfRangeComponentDropsRecord)
{
    DownstreamRecordSpec rec;
    rec.applicable = 0x08; // only component index 3, package has 1 component
    auto pkg = pkgWithRecords({rec});

    auto parser = parseDownstreamPkg(pkg);
    ASSERT_NE(parser, nullptr);
    EXPECT_TRUE(parser->getDownstreamDeviceIDRecords().empty());
}

TEST(PackageParser, DownstreamOutOfRangeComponentKeepsValidIndex)
{
    DownstreamRecordSpec rec;
    rec.applicable = 0x09; // indices 0 (valid) and 3 (out of range)
    auto pkg = pkgWithRecords({rec});

    auto parser = parseDownstreamPkg(pkg);
    ASSERT_NE(parser, nullptr);
    ASSERT_EQ(parser->getDownstreamDeviceIDRecords().size(), 1u);
    EXPECT_EQ(std::get<DownstreamApplicableComponents>(
                  parser->getDownstreamDeviceIDRecords()[0]),
              DownstreamApplicableComponents{0});
}

TEST(PackageParser, FirmwareDeviceOutOfRangeComponentDropsRecord)
{
    constexpr size_t fdApplicableBitmapIdx = 54;
    auto pkg = pkgWithRecords({DownstreamRecordSpec{}});
    pkg[fdApplicableBitmapIdx] = 0x08;
    refreshHeaderChecksum(pkg);

    auto parser = parseDownstreamPkg(pkg);
    ASSERT_NE(parser, nullptr);
    EXPECT_TRUE(parser->getFwDeviceIDRecords().empty());
    EXPECT_EQ(parser->getDownstreamDeviceIDRecords().size(), 1u);
}

TEST(PackageParser, FirmwareDeviceOutOfRangeComponentKeepsValidIndex)
{
    constexpr size_t fdApplicableBitmapIdx = 54;
    auto pkg = pkgWithRecords({DownstreamRecordSpec{}});
    pkg[fdApplicableBitmapIdx] = 0x09;
    refreshHeaderChecksum(pkg);

    auto parser = parseDownstreamPkg(pkg);
    ASSERT_NE(parser, nullptr);
    ASSERT_EQ(parser->getFwDeviceIDRecords().size(), 1u);
    EXPECT_EQ(std::get<ApplicableComponents>(parser->getFwDeviceIDRecords()[0]),
              ApplicableComponents{0});
}

TEST(PackageParser, DownstreamMalformedRecordsAreRejected)
{
    DownstreamRecordSpec recordLengthTooBig;
    recordLengthTooBig.recordLengthField = 0xFFFF;

    DownstreamRecordSpec selfContainedStringTooLong;
    selfContainedStringTooLong.selfContainedMinVersionLenField = 200;

    DownstreamRecordSpec missingComparisonStamp;
    missingComparisonStamp.flags = 1;
    missingComparisonStamp.omitComparisonStamp = true;
    // Descriptors would be eaten as the stamp, leaving the count unmet.
    missingComparisonStamp.descriptors.clear();

    DownstreamRecordSpec descriptorLengthTooBig;
    descriptorLengthTooBig.descriptors.clear();
    putLe(descriptorLengthTooBig.descriptors, PLDM_FWUP_UUID, 2);
    putLe(descriptorLengthTooBig.descriptors, 0xFF, 2);
    descriptorLengthTooBig.descriptors.push_back(0x00);

    DownstreamRecordSpec vendorDescriptorTruncated;
    vendorDescriptorTruncated.descriptors.clear();
    putLe(vendorDescriptorTruncated.descriptors, PLDM_FWUP_VENDOR_DEFINED, 2);
    putLe(vendorDescriptorTruncated.descriptors, 1, 2);
    vendorDescriptorTruncated.descriptors.push_back(0x00);

    DownstreamRecordSpec trailingBytes;
    trailingBytes.trailing = {0xEE, 0xEE};

    DownstreamRecordSpec packageDataTooLong;
    packageDataTooLong.packageDataLenField = 0x1000;

    DownstreamRecordSpec referenceManifestTooLong;
    referenceManifestTooLong.referenceManifestLenField = 100;

    DownstreamRecordSpec applicableBitmapMissing;
    applicableBitmapMissing.descriptors.clear();
    applicableBitmapMissing.descriptorCount = 0;
    applicableBitmapMissing.recordLengthField = 15;

    const std::vector<std::pair<std::string, std::vector<uint8_t>>> cases{
        {"RecordLengthExceedsHeader", recordLengthTooBig.serialize()},
        {"SelfContainedStringTooLong", selfContainedStringTooLong.serialize()},
        {"MissingComparisonStamp", missingComparisonStamp.serialize()},
        {"DescriptorLengthTooBig", descriptorLengthTooBig.serialize()},
        {"VendorDescriptorTruncated", vendorDescriptorTruncated.serialize()},
        {"TrailingBytes", trailingBytes.serialize()},
        {"PackageDataTooLong", packageDataTooLong.serialize()},
        {"ReferenceManifestTooLong", referenceManifestTooLong.serialize()},
        {"ApplicableBitmapMissing", applicableBitmapMissing.serialize()},
        // RecordLength smaller than each fixed-size field in turn.
        {"NoDescriptorCount", {0x02, 0x00}},
        {"NoUpdateOptionFlags", {0x04, 0x00, 0x01, 0x00}},
        {"NoPackageDataLength",
         {0x0A, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {"NoReferenceManifestLength",
         {0x0D, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
          0x00, 0x00}},
    };

    for (const auto& [name, area] : cases)
    {
        SCOPED_TRACE(name);
        auto pkg = pkgWithDownstreamArea(1, area);
        auto parser = parsePkgHeader(pkg.data(), pkg.size());
        ASSERT_NE(parser, nullptr);
        EXPECT_THROW(parser->parse(pkg, pkg.size()), InternalFailure);
    }
}

TEST(PackageParser, DownstreamRecordCountExceedsRecordsPresent)
{
    auto pkg = pkgWithDownstreamArea(2, DownstreamRecordSpec{}.serialize());
    auto parser = parsePkgHeader(pkg.data(), pkg.size());
    ASSERT_NE(parser, nullptr);
    EXPECT_THROW(parser->parse(pkg, pkg.size()), InternalFailure);
}

TEST(PackageParser, DownstreamCountMissingFromHeader)
{
    constexpr size_t hdrSizeIdx = 17;
    constexpr uint16_t hdrSizeEndingAtCount = 79;
    auto pkg = validDownstreamPkg();
    pkg[hdrSizeIdx] = hdrSizeEndingAtCount;
    pkg[hdrSizeIdx + 1] = 0;
    refreshHeaderChecksum(pkg);
    auto parser = parsePkgHeader(pkg.data(), pkg.size());
    ASSERT_NE(parser, nullptr);
    EXPECT_THROW(parser->parse(pkg, pkg.size()), InternalFailure);
}

TEST(PackageParser, PayloadChecksumMismatchIsRejected)
{
    auto pkg = validDownstreamPkg();
    pkg.back() ^= 0xFF;
    auto parser = parsePkgHeader(pkg.data(), pkg.size());
    ASSERT_NE(parser, nullptr);
    EXPECT_THROW(parser->parse(pkg, pkg.size()),
                 sdbusplus::error::xyz::openbmc_project::software::update::
                     InvalidSignature);
}

TEST(PackageParser, PackageSmallerThanCalculatedSizeIsRejected)
{
    auto pkg = validDownstreamPkg();
    auto parser = parsePkgHeader(pkg.data(), pkg.size());
    ASSERT_NE(parser, nullptr);
    EXPECT_THROW(parser->parse(pkg, pkg.size() - 1), InternalFailure);
}

TEST(PackageParser, InvalidPkgHeaderInfoIncomplete)
{
    std::vector<uint8_t> fwPkgHdr{0xF0, 0x18, 0x87, 0x8C, 0xCB, 0x7D,
                                  0x49, 0x43, 0x98, 0x00, 0xA0, 0x2F,
                                  0x05, 0x9A, 0xCA, 0x02};

    auto parser = parsePkgHeader(fwPkgHdr.data(), fwPkgHdr.size());
    EXPECT_EQ(parser, nullptr);
}

TEST(PackageParser, InvalidPkgNotSupportedHeaderFormat)
{
    std::vector<uint8_t> fwPkgHdr{
        0x12, 0x44, 0xD2, 0x64, 0x8D, 0x7D, 0x47, 0x18, 0xA0, 0x30,
        0xFC, 0x8A, 0x56, 0x58, 0x7D, 0x5B, 0x02, 0x8B, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0x0C, 0xE5,
        0x07, 0x00, 0x08, 0x00, 0x01, 0x0E, 0x56, 0x65, 0x72, 0x73,
        0x69, 0x6F, 0x6E, 0x53, 0x74, 0x72, 0x69, 0x6E, 0x67, 0x31};

    auto parser = parsePkgHeader(fwPkgHdr.data(), fwPkgHdr.size());
    EXPECT_EQ(parser, nullptr);
}

TEST(PackageParser, InvalidPkgBadChecksum)
{
    std::vector<uint8_t> fwPkgHdr{
        0xF0, 0x18, 0x87, 0x8C, 0xCB, 0x7D, 0x49, 0x43, 0x98, 0x00, 0xA0, 0x2F,
        0x05, 0x9A, 0xCA, 0x02, 0x01, 0x8B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x19, 0x0C, 0xE5, 0x07, 0x00, 0x08, 0x00, 0x01, 0x0E,
        0x56, 0x65, 0x72, 0x73, 0x69, 0x6F, 0x6E, 0x53, 0x74, 0x72, 0x69, 0x6E,
        0x67, 0x31, 0x01, 0x2E, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0E,
        0x00, 0x00, 0x01, 0x56, 0x65, 0x72, 0x73, 0x69, 0x6F, 0x6E, 0x53, 0x74,
        0x72, 0x69, 0x6E, 0x67, 0x32, 0x02, 0x00, 0x10, 0x00, 0x16, 0x20, 0x23,
        0xC9, 0x3E, 0xC5, 0x41, 0x15, 0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
        0x75, 0x01, 0x00, 0x0A, 0x00, 0x64, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
        0x00, 0x00, 0x00, 0x8B, 0x00, 0x00, 0x00, 0x1B, 0x00, 0x00, 0x00, 0x01,
        0x0E, 0x56, 0x65, 0x72, 0x73, 0x69, 0x6F, 0x6E, 0x53, 0x74, 0x72, 0x69,
        0x6E, 0x67, 0x33, 0x4F, 0x96, 0xAE, 0x57};

    constexpr uintmax_t pkgSize = 166;
    constexpr uintmax_t pkgHeaderSize = 139;
    constexpr uintmax_t pkgImageSize = pkgSize - pkgHeaderSize;
    std::vector<uint8_t> compImage;
    imageGenerate(compImage, pkgImageSize);
    imageInsert(fwPkgHdr, compImage);
    EXPECT_EQ(parsePkgHeader(fwPkgHdr.data(), fwPkgHdr.size()), nullptr);
}

TEST(PackageParser, SkipPackageSizeCheck)
{
#if SKIP_PACKAGE_SIZE_CHECK
    std::vector<uint8_t> fwPkgHdr{
        0xf0, 0x18, 0x87, 0x8c, 0xcb, 0x7d, 0x49, 0x43, 0x98, 0x00, 0xa0, 0x2f,
        0x05, 0x9a, 0xca, 0x02, 0x01, 0x8b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x19, 0x0c, 0xe5, 0x07, 0x00, 0x08, 0x00, 0x01, 0x0e,
        0x56, 0x65, 0x72, 0x73, 0x69, 0x6f, 0x6e, 0x53, 0x74, 0x72, 0x69, 0x6e,
        0x67, 0x31, 0x01, 0x2e, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x0e,
        0x00, 0x00, 0x01, 0x56, 0x65, 0x72, 0x73, 0x69, 0x6f, 0x6e, 0x53, 0x74,
        0x72, 0x69, 0x6e, 0x67, 0x32, 0x02, 0x00, 0x10, 0x00, 0x12, 0x44, 0xd2,
        0x64, 0x8d, 0x7d, 0x47, 0x18, 0xa0, 0x30, 0xfc, 0x8a, 0x56, 0x58, 0x7d,
        0x5b, 0x01, 0x00, 0x0a, 0x00, 0x64, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00,
        0x00, 0x00, 0x00, 0x8b, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x01,
        0x0e, 0x56, 0x65, 0x72, 0x73, 0x69, 0x6f, 0x6e, 0x53, 0x74, 0x72, 0x69,
        0x6e, 0x67, 0x33, 0x79, 0xd5, 0xd5, 0xb7, 0x61, 0x62, 0x63, 0x64, 0x31,
        0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x0a};

    constexpr uintmax_t pkgSize = 151;
    auto parser = parsePkgHeader(fwPkgHdr.data(), fwPkgHdr.size());

    parser->parse(fwPkgHdr, pkgSize);
    auto outfwDeviceIDRecords = parser->getFwDeviceIDRecords();
    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {0,
         {0},
         "VersionString2",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x12, 0x44, 0xD2, 0x64, 0x8D, 0x7D, 0x47, 0x18,
                                0xA0, 0x30, 0xFC, 0x8A, 0x56, 0x58, 0x7D,
                                0x5B}}},
         {}},
    };
    EXPECT_EQ(outfwDeviceIDRecords, fwDeviceIDRecords);

    auto outCompImageInfos = parser->getComponentImageInfos();
    ComponentImageInfos compImageInfos{
        {10, 100, 0xFFFFFFFF, 0, 0, 139, 5, "VersionString3"}};
    EXPECT_EQ(outCompImageInfos, compImageInfos);
#endif
}

class PackageParserMultipleDescSameType : public testing::Test
{
  protected:
    PackageParserMultipleDescSameType() {}
    std::vector<uint8_t> fwPkgHdr{
        0xF0, 0x18, 0x87, 0x8C, 0xCB, 0x7D, 0x49, 0x43, 0x98, 0x00, 0xA0, 0x2F,
        0x05, 0x9A, 0xCA, 0x02, 0x01, 0xB6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x19, 0x0C, 0xE5, 0x07, 0x00, 0x08, 0x00, 0x01, 0x0E,
        0x56, 0x65, 0x72, 0x73, 0x69, 0x6F, 0x6E, 0x53, 0x74, 0x72, 0x69, 0x6E,
        0x67, 0x31, 0x01, 0x59, 0x00, 0x05, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0E,
        0x00, 0x00, 0x01, 0x56, 0x65, 0x72, 0x73, 0x69, 0x6F, 0x6E, 0x53, 0x74,
        0x72, 0x69, 0x6E, 0x67, 0x32, 0x02, 0x00, 0x10, 0x00, 0x12, 0x44, 0xD2,
        0x64, 0x8D, 0x7D, 0x47, 0x18, 0xA0, 0x30, 0xFC, 0x8A, 0x56, 0x58, 0x7D,
        0x5B, 0x01, 0x00, 0x04, 0x00, 0x47, 0x16, 0x00, 0x00, 0x01, 0x00, 0x04,
        0x00, 0x47, 0x16, 0x10, 0x00, 0xFF, 0xFF, 0x0B, 0x00, 0x01, 0x07, 0x4F,
        0x70, 0x65, 0x6E, 0x42, 0x4D, 0x43, 0x12, 0x34, 0xFF, 0xFF, 0x08, 0x00,
        0x01, 0x03, 0x53, 0x4B, 0x55, 0x12, 0x34, 0x56, 0x01, 0x00, 0x0A, 0x00,
        0x64, 0x00, 0xFE, 0xFF, 0xFF, 0xFF, 0x02, 0x00, 0x00, 0x00, 0xB6, 0x00,
        0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x0E, 0x56, 0x65, 0x72, 0x73,
        0x69, 0x6F, 0x6E, 0x53, 0x74, 0x72, 0x69, 0x6E, 0x67, 0x35, 0x11, 0x16,
        0x1E, 0x1B, 0xFF, 0xFF};
    static constexpr uintmax_t pkgHeaderSize = 182;
    static constexpr uintmax_t pkgSize = 184;
    std::string_view pkgVersion{"VersionString1"};
    std::unique_ptr<PackageParser> parser =
        parsePkgHeader(fwPkgHdr.data(), fwPkgHdr.size());
};

TEST_F(PackageParserMultipleDescSameType, DescriptorsMatch)
{
    const auto& parserRef = *parser;
    EXPECT_EQ(typeid(parserRef).name(), typeid(PackageParser).name());
    EXPECT_EQ(parser->pkgHeaderSize, pkgHeaderSize);
    EXPECT_EQ(parser->pkgVersion, pkgVersion);
    parser->parse(fwPkgHdr, pkgSize);
    FirmwareDeviceIDRecords outfwDeviceIDRecords =
        parser->getFwDeviceIDRecords();
    FirmwareDeviceIDRecords fwDeviceIDRecords{
        {1,
         {0},
         "VersionString2",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x12, 0x44, 0xD2, 0x64, 0x8D, 0x7D, 0x47, 0x18,
                                0xA0, 0x30, 0xFC, 0x8A, 0x56, 0x58, 0x7D,
                                0x5B}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x10, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x12, 0x34})},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("SKU", std::vector<uint8_t>{0x12, 0x34, 0x56})}},
         {}}};
    EXPECT_EQ(outfwDeviceIDRecords, fwDeviceIDRecords);
}

TEST_F(PackageParserMultipleDescSameType, DescriptorsNoMatch)
{
    parser->parse(fwPkgHdr, pkgSize);
    FirmwareDeviceIDRecords outfwDeviceIDRecords =
        parser->getFwDeviceIDRecords();
    FirmwareDeviceIDRecords incorrectfwDeviceIDRecords{
        {1,
         {0},
         "VersionString2",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x12, 0x44, 0xD2, 0x64, 0x8D, 0x7D, 0x47, 0x18,
                                0xA0, 0x30, 0xFC, 0x8A, 0x56, 0x58, 0x7D,
                                0x5B}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x10, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x12, 0x34})},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("SKU", std::vector<uint8_t>{0x12, 0x34, 0x57})}},
         {}}};
    EXPECT_NE(outfwDeviceIDRecords, incorrectfwDeviceIDRecords);
}
