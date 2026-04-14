/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "platform-mc/oem_events.hpp"
#include "platform-mc/smbios_mdr.hpp"

#include <array>

#include <gtest/gtest.h>

using namespace pldm::oem_events;

TEST(OemEventsCoverage, oemEventValidationCoverage)
{
    const std::array<uint8_t, 3> tooSmall{{0x01, 0x00, 0x00}};
    EXPECT_FALSE(handleCperErrorCountEvent("ProcessorModule_0", tooSmall.data(),
                                           tooSmall.size()));

    const std::array<uint8_t, 4> truncatedPayload{{0x01, 0x00, 0x08, 0x00}};
    EXPECT_FALSE(handlePcieLtssmEvent(
        "ProcessorModule_1", truncatedPayload.data(), truncatedPayload.size()));
    EXPECT_FALSE(handlePcieTelemetryEvent(
        "ProcessorModule_2", truncatedPayload.data(), truncatedPayload.size()));
}

TEST(OemEventsCoverage, inventoryAndSmbiosValidationCoverage)
{
    const std::array<uint8_t, 3> shortInventory{{0x01, 0x00, 0x02}};
    EXPECT_FALSE(handleInventoryEvent(
        "ProcessorModule_3", shortInventory.data(), shortInventory.size()));

    const std::array<uint8_t, 4> emptyInventory{{0x01, 0x00, 0x00, 0x00}};
    EXPECT_FALSE(handleInventoryEvent(
        "ProcessorModule_4", emptyInventory.data(), emptyInventory.size()));

    const std::array<uint8_t, 6> truncatedInventory{
        {0x01, 0x00, 0x04, 0x00, '{', '}'}};
    EXPECT_FALSE(
        handleInventoryEvent("ProcessorModule_5", truncatedInventory.data(),
                             truncatedInventory.size()));

    const std::array<uint8_t, 1> invalidSmbios{{0x00}};
    EXPECT_FALSE(handleSmbiosEvent(const_cast<uint8_t*>(invalidSmbios.data()),
                                   invalidSmbios.size()));
}

TEST(OemEventsCoverage, oemEventHeaderMismatchCoverage)
{
    const std::array<uint8_t, 6> truncatedWithWarnings{
        {0x02, 0x01, 0x08, 0x00, 0x11, 0x22}};

    EXPECT_FALSE(handleCperErrorCountEvent(
        "../unsafe\\terminus", truncatedWithWarnings.data(),
        truncatedWithWarnings.size()));
    EXPECT_FALSE(handlePcieTelemetryEvent(
        "ProcessorModule_6", truncatedWithWarnings.data(),
        truncatedWithWarnings.size()));
}

TEST(OemEventsCoverage, inventoryEmptyPayloadAndSyncCoverage)
{
    const std::array<uint8_t, 4> emptyPayloadWithReservedHeader{
        {0x02, 0x01, 0x00, 0x00}};
    EXPECT_FALSE(handleInventoryEvent("../unsafe\\terminus",
                                      emptyPayloadWithReservedHeader.data(),
                                      emptyPayloadWithReservedHeader.size()));

    EXPECT_FALSE(mdr::syncSmbiosData());
}

TEST(OemEventsCoverage, validPayloadPermissionFailureCoverage)
{
    const std::array<uint8_t, 6> validOemPayload{
        {0x01, 0x00, 0x02, 0x00, 0xAA, 0x55}};
    EXPECT_FALSE(handleCperErrorCountEvent(
        "../unsafe\\terminus", validOemPayload.data(), validOemPayload.size()));
    EXPECT_FALSE(handlePcieLtssmEvent(
        "../unsafe\\terminus", validOemPayload.data(), validOemPayload.size()));
    EXPECT_FALSE(handlePcieTelemetryEvent(
        "../unsafe\\terminus", validOemPayload.data(), validOemPayload.size()));

    const std::array<uint8_t, 6> validInventoryPayload{
        {0x01, 0x00, 0x02, 0x00, '{', '}'}};
    EXPECT_FALSE(handleInventoryEvent("../unsafe\\terminus",
                                      validInventoryPayload.data(),
                                      validInventoryPayload.size()));

    uint8_t smbiosPayload[] = {0x10, 0x20};
    EXPECT_THROW(
        (void)mdr::saveSmbiosData(sizeof(smbiosPayload), smbiosPayload),
        std::filesystem::filesystem_error);

    const std::array<uint8_t, 5> validSmbiosEvent{
        {0x01, 0x02, 0x00, 0x10, 0x20}};
    EXPECT_THROW((void)handleSmbiosEvent(validSmbiosEvent.data(),
                                         validSmbiosEvent.size()),
                 std::filesystem::filesystem_error);
}

TEST(OemEventsCoverage, sanitizeUnknownAndWarningHeaderCoverage)
{
    const std::array<uint8_t, 5> validPayloadWithWarnings{
        {0x02, 0x01, 0x01, 0x00, 0x5A}};

    EXPECT_FALSE(
        handleCperErrorCountEvent("..", validPayloadWithWarnings.data(),
                                  validPayloadWithWarnings.size()));
    EXPECT_FALSE(handlePcieLtssmEvent("..", validPayloadWithWarnings.data(),
                                      validPayloadWithWarnings.size()));
    EXPECT_FALSE(handlePcieTelemetryEvent("..", validPayloadWithWarnings.data(),
                                          validPayloadWithWarnings.size()));

    const std::array<uint8_t, 6> validInventoryPayloadWithWarnings{
        {0x02, 0x01, 0x02, 0x00, '{', '}'}};
    EXPECT_FALSE(
        handleInventoryEvent("..", validInventoryPayloadWithWarnings.data(),
                             validInventoryPayloadWithWarnings.size()));
}
