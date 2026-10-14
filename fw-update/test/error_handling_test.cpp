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

#include <array>
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
     getCompCompatibilityMessage_update_component_identical_has_no_legacy)
{
    // An identical image is reported by ComponentImageIdentical only.
    auto [outmessageStatus, outoemMessageId, outoemMessageError,
          outoemResolution] =
        getCompCompatibilityMessage(PLDM_UPDATE_COMPONENT,
                                    PLDM_CRC_COMP_COMPARISON_STAMP_IDENTICAL);

    EXPECT_FALSE(outmessageStatus);
    EXPECT_TRUE(outoemMessageId.empty());
    EXPECT_TRUE(outoemMessageError.empty());
    EXPECT_TRUE(outoemResolution.empty());
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
        {Message::signatureHeaderMalformed,
         "NvidiaUpdate.1.3.SignatureHeaderMalformed",
         ArgumentType::packageVersionReason,
         Severity::critical,
         {}},
        {Message::signatureHeaderVersionUnsupported,
         "NvidiaUpdate.1.3.SignatureHeaderVersionUnsupported",
         ArgumentType::packageVersionSignatureVersion,
         Severity::critical,
         {}},
        {Message::packageSignatureIntegrityFailed,
         "NvidiaUpdate.1.3.PackageSignatureIntegrityFailed",
         ArgumentType::packageVersion,
         Severity::critical,
         {}},
        {Message::packageSignatureVerificationFailed,
         "NvidiaUpdate.1.3.PackageSignatureVerificationFailed",
         ArgumentType::packageVersion,
         Severity::critical,
         {}},
        {Message::packageSignatureRequired,
         "NvidiaUpdate.1.3.PackageSignatureRequired",
         ArgumentType::packageVersion,
         Severity::critical,
         {}},
        {Message::componentImageIdentical,
         "NvidiaUpdate.1.3.ComponentImageIdentical",
         ArgumentType::componentPackageVersionComparisonStamp,
         Severity::informational,
         {}},
        {Message::firmwareComponentComparisonStampLower,
         "NvidiaUpdate.1.3.ComponentDowngradeRejected",
         ArgumentType::componentPackageVersionComparisonStamp,
         Severity::critical,
         {}},
        {Message::firmwareDownloadTimedOut,
         "NvidiaUpdate.1.3.FirmwareDownloadTimedOut",
         ArgumentType::componentTransferProgress,
         Severity::critical,
         {}},
        {Message::firmwareUpdateBlockedByWriteProtection,
         "NvidiaUpdate.1.3.FirmwareUpdateBlockedByWriteProtection",
         ArgumentType::component,
         Severity::critical,
         {}},
        {Message::firmwareTransferCompletionTimedOut,
         "NvidiaUpdate.1.3.FirmwareTransferCompletionTimedOut",
         ArgumentType::component,
         Severity::critical,
         {}},
        {Message::firmwareVerificationCompletionTimedOut,
         "NvidiaUpdate.1.3.FirmwareVerificationCompletionTimedOut",
         ArgumentType::component,
         Severity::critical,
         {}},
        {Message::firmwareMetadataAuthenticationFailed,
         "NvidiaUpdate.1.3.ComponentMetadataAuthenticationFailed",
         ArgumentType::componentVersionPackageVersion,
         Severity::critical,
         {}},
        {Message::firmwareImageSecurityVersionRejected,
         "NvidiaUpdate.1.3.ComponentImageSecurityVersionRejected",
         ArgumentType::componentVersionPackageVersion,
         Severity::critical,
         {}},
        {Message::firmwareImageSigningKeyRevoked,
         "NvidiaUpdate.1.3.ComponentImageSigningKeyRevoked",
         ArgumentType::componentVersionPackageVersion,
         Severity::critical,
         {}},
        {Message::firmwareImageAuthenticationFailed,
         "NvidiaUpdate.1.3.ComponentImageAuthenticationFailed",
         ArgumentType::componentVersionPackageVersion,
         Severity::critical,
         {}},
        {Message::firmwareImageSkuMismatch,
         "NvidiaUpdate.1.3.ComponentImageSkuMismatch",
         ArgumentType::componentVersionPackageVersion,
         Severity::critical,
         {}},
        {Message::firmwareImageSizeVerificationFailed,
         "NvidiaUpdate.1.3.ComponentImageVerificationFailed",
         ArgumentType::componentVersionPackageVersionReason, Severity::critical,
         "image size check failed (VerifyComplete: OEM firmwarePackageSizeFailure (0x98))"},
        {Message::firmwareImageGenericVerificationFailed,
         "NvidiaUpdate.1.3.ComponentImageVerificationFailed",
         ArgumentType::componentVersionPackageVersionReason, Severity::critical,
         "generic verification failure (VerifyComplete: PLDM_FWUP_VERIFY_ERROR_VERIFICATION_FAILURE (0x01))"},
        {Message::firmwareImageSecurityChecksFailed,
         "NvidiaUpdate.1.3.ComponentImageVerificationFailed",
         ArgumentType::componentVersionPackageVersionReason, Severity::critical,
         "did not pass the device's security checks (VerifyComplete: PLDM_FWUP_VERIFY_FAILED_FD_SECURITY_CHECKS (0x03))"},
        {Message::firmwareImageTransferIncomplete,
         "NvidiaUpdate.1.3.ComponentImageVerificationFailed",
         ArgumentType::componentVersionPackageVersionReason, Severity::critical,
         "image transfer was incomplete (VerifyComplete: PLDM_FWUP_VERIFY_ERROR_IMAGE_INCOMPLETE (0x04))"},
        {Message::firmwareImageSecurityRevisionFailed,
         "NvidiaUpdate.1.3.ComponentImageVerificationFailed",
         ArgumentType::componentVersionPackageVersionReason, Severity::critical,
         "security version does not meet the device's requirement (VerifyComplete: PLDM_FWUP_VERIFY_FAILURE_SECURITY_REVISION_ERROR (0x10))"},
        {Message::firmwareImageGenericError,
         "NvidiaUpdate.1.3.ComponentImageVerificationFailed",
         ArgumentType::componentVersionPackageVersionReason, Severity::critical,
         "generic verification failure (VerifyComplete: PLDM_FWUP_GENERIC_ERROR (0x0A))"},
        {Message::applyCompleteTimedOut,
         "NvidiaUpdate.1.3.ApplyCompleteTimedOut",
         ArgumentType::component,
         Severity::critical,
         {}},
        {Message::pldmCommandTimedOut,
         "NvidiaUpdate.1.3.PLDMCommandTimedOut",
         ArgumentType::commandComponent,
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

TEST(error_handling, getUpdateComponentMessageDefinition)
{
    using ArgumentType = NvidiaUpdateMessageArgumentType;

    struct TestCase
    {
        std::string_view messageId;
        ArgumentType argumentType;
        uint8_t responseCode;
    };

    static constexpr TestCase testCases[]{
        {"NvidiaUpdate.1.3.ComponentImageIdentical",
         ArgumentType::componentPackageVersionComparisonStamp,
         PLDM_CCRC_COMP_COMPARISON_STAMP_IDENTICAL},
        {"NvidiaUpdate.1.3.ComponentDowngradeRejected",
         ArgumentType::componentPackageVersionComparisonStamp,
         PLDM_CCRC_COMP_COMPARISON_STAMP_LOWER},
    };

    for (const auto& testCase : testCases)
    {
        SCOPED_TRACE(static_cast<unsigned>(testCase.responseCode));
        const auto definition =
            getUpdateComponentMessageDefinition(testCase.responseCode);
        ASSERT_TRUE(definition.has_value());
        EXPECT_EQ(definition->messageId, testCase.messageId);
        EXPECT_EQ(definition->argumentType, testCase.argumentType);
    }

    for (const uint8_t responseCode : std::array<uint8_t, 7>{
             PLDM_CCRC_VENDOR_COMP_RESP_CODE_RANGE_MIN,
             PLDM_CCRC_INCOMPLETE_COMP_IMAGE_SET, PLDM_CCRC_COMP_NOT_SUPPORTED,
             PLDM_CCRC_COMP_INFO_NO_MATCH, PLDM_CCRC_COMP_VER_STR_IDENTICAL,
             PLDM_CCRC_COMP_VER_STR_LOWER,
             PLDM_CCRC_VENDOR_COMP_RESP_CODE_RANGE_MAX})
    {
        EXPECT_FALSE(
            getUpdateComponentMessageDefinition(responseCode).has_value());
    }
}

TEST(error_handling, getNvidiaCommandMessageDefinition)
{
    struct TestCase
    {
        pldm_firmware_update_commands command;
        uint8_t result;
        std::string_view messageId;
    };

    static constexpr TestCase testCases[]{
        {PLDM_TRANSFER_COMPLETE, writeProtectEnabled,
         "NvidiaUpdate.1.3.FirmwareUpdateBlockedByWriteProtection"},
        {PLDM_TRANSFER_COMPLETE, PLDM_FWUP_TIME_OUT,
         "NvidiaUpdate.1.3.FirmwareTransferCompletionTimedOut"},
        {PLDM_VERIFY_COMPLETE, PLDM_FWUP_TIME_OUT,
         "NvidiaUpdate.1.3.FirmwareVerificationCompletionTimedOut"},
        {PLDM_VERIFY_COMPLETE, metadataAuthFailure,
         "NvidiaUpdate.1.3.ComponentMetadataAuthenticationFailed"},
        {PLDM_VERIFY_COMPLETE, secVersionCheckFailure,
         "NvidiaUpdate.1.3.ComponentImageSecurityVersionRejected"},
        {PLDM_VERIFY_COMPLETE, secKeysReovked,
         "NvidiaUpdate.1.3.ComponentImageSigningKeyRevoked"},
        {PLDM_VERIFY_COMPLETE, imageAuthFailure,
         "NvidiaUpdate.1.3.ComponentImageAuthenticationFailed"},
        {PLDM_VERIFY_COMPLETE, skuMismatch,
         "NvidiaUpdate.1.3.ComponentImageSkuMismatch"},
        {PLDM_VERIFY_COMPLETE, firmwarePackageSizeFailure,
         "NvidiaUpdate.1.3.ComponentImageVerificationFailed"},
        {PLDM_VERIFY_COMPLETE, PLDM_FWUP_VERIFY_ERROR_VERIFICATION_FAILURE,
         "NvidiaUpdate.1.3.ComponentImageVerificationFailed"},
        {PLDM_VERIFY_COMPLETE, PLDM_FWUP_VERIFY_FAILED_FD_SECURITY_CHECKS,
         "NvidiaUpdate.1.3.ComponentImageVerificationFailed"},
        {PLDM_VERIFY_COMPLETE, PLDM_FWUP_VERIFY_ERROR_IMAGE_INCOMPLETE,
         "NvidiaUpdate.1.3.ComponentImageVerificationFailed"},
        {PLDM_VERIFY_COMPLETE, PLDM_FWUP_VERIFY_FAILURE_SECURITY_REVISION_ERROR,
         "NvidiaUpdate.1.3.ComponentImageVerificationFailed"},
        {PLDM_VERIFY_COMPLETE, PLDM_FWUP_GENERIC_ERROR,
         "NvidiaUpdate.1.3.ComponentImageVerificationFailed"},
        {PLDM_APPLY_COMPLETE, PLDM_FWUP_TIME_OUT,
         "NvidiaUpdate.1.3.ApplyCompleteTimedOut"},
        {PLDM_GET_STATUS, COMMAND_TIMEOUT,
         "NvidiaUpdate.1.3.PLDMCommandTimedOut"},
    };

    for (const auto& testCase : testCases)
    {
        SCOPED_TRACE(static_cast<unsigned>(testCase.command));
        const auto definition = getNvidiaCommandMessageDefinition(
            testCase.command, testCase.result);
        ASSERT_TRUE(definition.has_value());
        EXPECT_EQ(definition->messageId, testCase.messageId);
    }

    EXPECT_FALSE(getNvidiaCommandMessageDefinition(PLDM_TRANSFER_COMPLETE, 0xFF)
                     .has_value());
    EXPECT_FALSE(getNvidiaCommandMessageDefinition(PLDM_VERIFY_COMPLETE, 0x7F)
                     .has_value());
}
#endif
