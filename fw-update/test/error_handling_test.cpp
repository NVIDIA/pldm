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
#include "libpldm/firmware_update.h"

#include "fw-update/error_handling.hpp"

#include <string_view>

#include <gtest/gtest.h>

using namespace pldm;
using namespace pldm::fw_update;

TEST(error_handling, getOemMessage_request_update)
{
    const std::string oemMessageId = "ResourceEvent.1.0.ResourceErrorsDetected";
    const std::string oemMessageError =
        "The device did not respond to the update request, so the update failed.";
    const std::string oemResolution = "Retry firmware update operation";
    const bool oemMessageStatus = true;

    auto [outmessageStatus, outoemMessageId, outoemMessageError,
          outoemResolution] =
        getOemMessage(PLDM_REQUEST_UPDATE, PLDM_FWUP_TIME_OUT);

    EXPECT_EQ(outmessageStatus, oemMessageStatus);
    EXPECT_EQ(outoemMessageId, oemMessageId);
    EXPECT_EQ(outoemMessageError, oemMessageError);
    EXPECT_EQ(outoemResolution, oemResolution);
}

TEST(error_handling, getOemMessage_get_firmware_parameters)
{
    const std::string oemMessageId = "ResourceEvent.1.0.ResourceErrorsDetected";
    const std::string oemMessageError = "";
    const std::string oemResolution = "";
    const bool oemMessageStatus = false;

    auto [outmessageStatus, outoemMessageId, outoemMessageError,
          outoemResolution] =
        getOemMessage(PLDM_GET_FIRMWARE_PARAMETERS, PLDM_FWUP_TIME_OUT);

    EXPECT_EQ(outmessageStatus, oemMessageStatus);
    EXPECT_EQ(outoemMessageId, oemMessageId);
    EXPECT_EQ(outoemMessageError, oemMessageError);
    EXPECT_EQ(outoemResolution, oemResolution);
}

TEST(error_handling, getOemMessage_unknown_error_code)
{
    const std::string oemMessageId = "ResourceEvent.1.0.ResourceErrorsDetected";
    const std::string oemMessageError = "";
    const std::string oemResolution = "";
    const bool oemMessageStatus = false;

    auto [outmessageStatus, outoemMessageId, outoemMessageError,
          outoemResolution] = getOemMessage(PLDM_REQUEST_UPDATE, 0xFF);

    EXPECT_EQ(outmessageStatus, oemMessageStatus);
    EXPECT_EQ(outoemMessageId, oemMessageId);
    EXPECT_EQ(outoemMessageError, oemMessageError);
    EXPECT_EQ(outoemResolution, oemResolution);
}

TEST(error_handling,
     getCompCompatibilityMessage_update_component_identical_comp_stamp)
{
    const std::string oemMessageId = "NvidiaUpdate.1.0.ComponentUpdateSkipped";
    const std::string oemMessageError = "Component image is identical";
    const std::string oemResolution =
        "Retry firmware update operation with the force flag";
    const bool oemMessageStatus = true;

    auto [outmessageStatus, outoemMessageId, outoemMessageError,
          outoemResolution] =
        getCompCompatibilityMessage(PLDM_UPDATE_COMPONENT,
                                    PLDM_CRC_COMP_COMPARISON_STAMP_IDENTICAL);

    EXPECT_EQ(outmessageStatus, oemMessageStatus);
    EXPECT_EQ(outoemMessageId, oemMessageId);
    EXPECT_EQ(outoemMessageError, oemMessageError);
    EXPECT_EQ(outoemResolution, oemResolution);
}

TEST(error_handling,
     getCompCompatibilityMessage_update_component_lower_comp_stamp)
{
    const std::string oemMessageId = "ResourceEvent.1.0.ResourceErrorsDetected";
    const std::string oemMessageError =
        "Component comparison stamp is lower than the firmware component comparison stamp in the FD";
    const std::string oemResolution =
        "Retry firmware update operation with the force flag";
    const bool oemMessageStatus = true;

    auto [outmessageStatus, outoemMessageId, outoemMessageError,
          outoemResolution] =
        getCompCompatibilityMessage(PLDM_UPDATE_COMPONENT,
                                    PLDM_CRC_COMP_COMPARISON_STAMP_LOWER);

    EXPECT_EQ(outmessageStatus, oemMessageStatus);
    EXPECT_EQ(outoemMessageId, oemMessageId);
    EXPECT_EQ(outoemMessageError, oemMessageError);
    EXPECT_EQ(outoemResolution, oemResolution);
}

TEST(error_handling,
     getCompCompatibilityMessage_update_component_unknown_response_code)
{
    const bool expectedStatus = false;

    auto [outmessageStatus, outoemMessageId, outoemMessageError,
          outoemResolution] =
        getCompCompatibilityMessage(PLDM_UPDATE_COMPONENT, 0xFF);

    EXPECT_EQ(outmessageStatus, expectedStatus);
    EXPECT_TRUE(outoemMessageId.empty());
    EXPECT_TRUE(outoemMessageError.empty());
    EXPECT_TRUE(outoemResolution.empty());
}

TEST(error_handling, getCompCompatibilityMessage_unknown_command)
{
    const bool expectedStatus = false;

    auto [outmessageStatus, outoemMessageId, outoemMessageError,
          outoemResolution] =
        getCompCompatibilityMessage(PLDM_REQUEST_UPDATE,
                                    PLDM_CRC_COMP_COMPARISON_STAMP_IDENTICAL);

    EXPECT_EQ(outmessageStatus, expectedStatus);
    EXPECT_TRUE(outoemMessageId.empty());
    EXPECT_TRUE(outoemMessageError.empty());
    EXPECT_TRUE(outoemResolution.empty());
}

#ifdef OEM_NVIDIA
TEST(error_handling, getNvidiaUpdateMessageDefinition)
{
    using ArgumentType = NvidiaUpdateMessageArgumentType;
    using Message = NvidiaUpdateMessage;
    using Severity = NvidiaUpdateMessageSeverity;

    struct TestCase
    {
        Message message;
        std::string_view messageId;
        ArgumentType argumentType;
        Severity severity;
        std::string_view reason;
    };

    static constexpr TestCase testCases[]{
        {Message::firmwarePackageNoMatchingDevices,
         "NvidiaUpdate.1.3.FirmwarePackageNoMatchingDevices",
         ArgumentType::packageVersion,
         Severity::critical,
         {}},
        {Message::requestedTargetUnavailableForUpdate,
         "NvidiaUpdate.1.3.RequestedTargetUnavailableForUpdate",
         ArgumentType::target,
         Severity::critical,
         {}},
        {Message::requestedTargetNotSupportedByPackage,
         "NvidiaUpdate.1.3.RequestedTargetNotSupportedByPackage",
         ArgumentType::targetPackageVersion,
         Severity::critical,
         {}},
        {Message::firmwarePackageHeaderUnavailable,
         "NvidiaUpdate.1.3.FirmwarePackageHeaderUnavailable",
         ArgumentType::none,
         Severity::critical,
         {}},
        {Message::firmwarePackageHeaderInvalid,
         "NvidiaUpdate.1.3.FirmwarePackageHeaderInvalid",
         ArgumentType::none,
         Severity::critical,
         {}},
        {Message::packageHeaderChecksumMismatch,
         "NvidiaUpdate.1.3.PackageHeaderChecksumMismatch",
         ArgumentType::checksumPair,
         Severity::critical,
         {}},
        {Message::packagePayloadChecksumMismatch,
         "NvidiaUpdate.1.3.PackagePayloadChecksumMismatch",
         ArgumentType::packageVersionChecksumPair,
         Severity::critical,
         {}},
        {Message::firmwarePackageMetadataInvalid,
         "NvidiaUpdate.1.3.FirmwarePackageMetadataInvalid",
         ArgumentType::none,
         Severity::critical,
         {}},
    };

    for (const auto& testCase : testCases)
    {
        SCOPED_TRACE(static_cast<unsigned>(testCase.message));
        const auto definition =
            getNvidiaUpdateMessageDefinition(testCase.message);
        ASSERT_TRUE(definition.has_value());
        EXPECT_EQ(definition->messageId, testCase.messageId);
        EXPECT_EQ(definition->argumentType, testCase.argumentType);
        EXPECT_EQ(definition->severity, testCase.severity);
        EXPECT_EQ(definition->reason, testCase.reason);
        EXPECT_FALSE(definition->resolution.empty());
    }
}
#endif
