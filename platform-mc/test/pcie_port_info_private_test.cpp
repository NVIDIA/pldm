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
#include <endian.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

// White-box include: pull in the anonymous-namespace internals (genToGbps,
// moduleIndex, the packed PciePortInfoHeader/PciePortInfoRecord structs) plus
// the public handlePciePortInfoEvent entry point so both the pure decode
// helpers and the event dispatch branches can be exercised directly.
#include "../../oem/nvidia/platform-mc/pcie_port_info.cpp" // NOLINT(bugprone-suspicious-include)

namespace pldm::oem_nvidia
{
namespace
{

constexpr size_t recordCount = 48; // PCIE_MAX_RP_NUM
constexpr size_t recordBytes = 7;  // packed pcie_rp_link_info size
constexpr uint16_t bodyBytes = static_cast<uint16_t>(recordCount * recordBytes);
// Synthetic PCIe field values used to build the test records.
constexpr uint8_t formatVersionV1 = 0x01;
constexpr uint8_t formatTypeFull = 0x00;
constexpr uint8_t formatTypeReserved = 0x01;
constexpr uint8_t flagSet = 1;       // is_enabled / data_valid = true
constexpr uint8_t flagClear = 0;     // false
constexpr uint8_t noLink = 0;        // down link: gen code 0 / 0 lanes
constexpr uint8_t genGen6 = 6;       // PCIe Gen6 speed code
constexpr uint8_t widthX16 = 16;     // x16 negotiated lanes
constexpr uint8_t rpOutOfRange = 48; // == PCIE_MAX_RP_NUM (invalid rp_num)
constexpr uint8_t noDownRp = 255;    // sentinel: makeFullPayload has no down RP

// Build the 4-byte header with the size field encoded little-endian on the
// wire.
static std::vector<uint8_t> makeHeader(uint8_t version, uint8_t type,
                                       uint16_t size)
{
    return {version, type, static_cast<uint8_t>(size & 0xff),
            static_cast<uint8_t>((size >> 8) & 0xff)};
}

// Build one 7-byte pcie_rp_link_info record.
static std::array<uint8_t, 7> makeRecord(
    uint8_t enabled, uint8_t valid, uint8_t rpNum, uint8_t linkSpeed,
    uint8_t linkWidth, uint8_t maxLinkSpeed, uint8_t maxLinkWidth)
{
    return {enabled,   valid,        rpNum,       linkSpeed,
            linkWidth, maxLinkSpeed, maxLinkWidth};
}

// A full valid payload: header + 48 records. Every record is Gen6 x16 on RP i,
// except RP `downRp` which is left as a down link (all zero speed/width).
static std::vector<uint8_t> makeFullPayload(uint8_t downRp = noDownRp)
{
    auto buf = makeHeader(formatVersionV1, formatTypeFull, bodyBytes);
    for (uint8_t rp = 0; rp < recordCount; ++rp)
    {
        auto rec = (rp == downRp) ? makeRecord(flagClear, flagSet, rp, noLink,
                                               noLink, noLink, noLink)
                                  : makeRecord(flagSet, flagSet, rp, genGen6,
                                               widthX16, genGen6, widthX16);
        buf.insert(buf.end(), rec.begin(), rec.end());
    }
    return buf;
}

TEST(PciePortInfoDecode, GenToGbpsMapping)
{
    EXPECT_DOUBLE_EQ(genToGbps(0), 0.0); // no link
    EXPECT_DOUBLE_EQ(genToGbps(1), 2.0);
    EXPECT_DOUBLE_EQ(genToGbps(2), 4.0);
    EXPECT_DOUBLE_EQ(genToGbps(3), 7.877);
    EXPECT_DOUBLE_EQ(genToGbps(4), 15.754);
    EXPECT_DOUBLE_EQ(genToGbps(5), 31.508);
    EXPECT_DOUBLE_EQ(genToGbps(6), 63.0);
    EXPECT_DOUBLE_EQ(genToGbps(7), 0.0);   // unknown gen -> no link
    EXPECT_DOUBLE_EQ(genToGbps(255), 0.0); // out of range -> no link
}

TEST(PciePortInfoDecode, ModuleIndexParsing)
{
    EXPECT_EQ(moduleIndex("ProcessorModule_0"), "0");
    EXPECT_EQ(moduleIndex("ProcessorModule_1"), "1");
    EXPECT_EQ(moduleIndex("ProcessorModule_12"), "12");
    EXPECT_EQ(moduleIndex("NoDigits"), "0"); // no trailing digits -> default
    EXPECT_EQ(moduleIndex(""), "0");         // empty -> default
    EXPECT_EQ(moduleIndex("42"), "42");      // whole string is digits
}

TEST(PciePortInfoDecode, WireLayoutIsPackedAndStable)
{
    // Packed sizes must match the on-wire framing exactly.
    EXPECT_EQ(sizeof(PciePortInfoHeader), 4u);
    EXPECT_EQ(sizeof(PciePortInfoRecord), 7u);

    EXPECT_EQ(offsetof(PciePortInfoHeader, formatVersion), 0u);
    EXPECT_EQ(offsetof(PciePortInfoHeader, formatType), 1u);
    EXPECT_EQ(offsetof(PciePortInfoHeader, size), 2u);

    EXPECT_EQ(offsetof(PciePortInfoRecord, isEnabled), 0u);
    EXPECT_EQ(offsetof(PciePortInfoRecord, dataValid), 1u);
    EXPECT_EQ(offsetof(PciePortInfoRecord, rpNum), 2u);
    EXPECT_EQ(offsetof(PciePortInfoRecord, linkSpeed), 3u);
    EXPECT_EQ(offsetof(PciePortInfoRecord, linkWidth), 4u);
    EXPECT_EQ(offsetof(PciePortInfoRecord, maxLinkSpeed), 5u);
    EXPECT_EQ(offsetof(PciePortInfoRecord, maxLinkWidth), 6u);
}

TEST(PciePortInfoDecode, HeaderSizeIsLittleEndian)
{
    // Wire bytes 0x50,0x01 for the size field => 0x0150 = 336 in host order.
    const uint8_t bytes[4] = {0x01, 0x00, 0x50, 0x01};
    PciePortInfoHeader hdr{};
    std::memcpy(&hdr, bytes, sizeof(hdr));
    EXPECT_EQ(hdr.formatVersion, formatVersionV1);
    EXPECT_EQ(hdr.formatType, formatTypeFull);
    EXPECT_EQ(le16toh(hdr.size), 0x0150u);
    EXPECT_EQ(le16toh(hdr.size), bodyBytes);
}

TEST(PciePortInfoEvent, RejectsEmptyPayload)
{
    EXPECT_FALSE(handlePciePortInfoEvent("ProcessorModule_0", nullptr, 0));
    const std::vector<uint8_t> buf =
        makeHeader(formatVersionV1, formatTypeFull, 0);
    EXPECT_FALSE(handlePciePortInfoEvent("ProcessorModule_0", buf.data(), 0));
}

TEST(PciePortInfoEvent, TooSmallPayloadDroppedSafely)
{
    // Smaller than header + one record: dropped with an error, no D-Bus writes.
    const std::vector<uint8_t> buf =
        makeHeader(formatVersionV1, formatTypeFull, bodyBytes);
    EXPECT_TRUE(
        handlePciePortInfoEvent("ProcessorModule_0", buf.data(), buf.size()));
}

TEST(PciePortInfoEvent, ReservedFormatTypeIgnored)
{
    // Non-zero (reserved) formatType: ignored before any D-Bus work.
    auto buf = makeHeader(formatVersionV1, formatTypeReserved, bodyBytes);
    auto rec =
        makeRecord(flagSet, flagSet, 0, genGen6, widthX16, genGen6, widthX16);
    buf.insert(buf.end(), rec.begin(), rec.end());
    EXPECT_TRUE(
        handlePciePortInfoEvent("ProcessorModule_0", buf.data(), buf.size()));
}

TEST(PciePortInfoEvent, FullArrayPublishesAndReuses)
{
    const auto buf = makeFullPayload(/*downRp=*/32);
    // First delivery creates the per-RP objects; second reuses them in place.
    EXPECT_TRUE(
        handlePciePortInfoEvent("ProcessorModule_0", buf.data(), buf.size()));
    EXPECT_TRUE(
        handlePciePortInfoEvent("ProcessorModule_0", buf.data(), buf.size()));
}

TEST(PciePortInfoEvent, OutOfRangeRpNumSkipped)
{
    // A record with rp_num >= PCIE_MAX_RP_NUM is skipped; valid ones still
    // pass.
    auto buf = makeHeader(formatVersionV1, formatTypeFull,
                          static_cast<uint16_t>(2 * recordBytes));
    auto good =
        makeRecord(flagSet, flagSet, 5, genGen6, widthX16, genGen6, widthX16);
    auto bad = makeRecord(flagSet, flagSet, rpOutOfRange, genGen6, widthX16,
                          genGen6, widthX16); // out of range
    buf.insert(buf.end(), good.begin(), good.end());
    buf.insert(buf.end(), bad.begin(), bad.end());
    EXPECT_TRUE(
        handlePciePortInfoEvent("ProcessorModule_0", buf.data(), buf.size()));
}

} // namespace
} // namespace pldm::oem_nvidia
