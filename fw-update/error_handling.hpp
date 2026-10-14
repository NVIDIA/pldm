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
#pragma once

#include "common/types.hpp"
#include "dbusutil.hpp"

#include <libpldm/base.h>
#include <libpldm/firmware_update.h>

#include <phosphor-logging/lg2.hpp>

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

PHOSPHOR_LOG2_USING;

namespace pldm
{
namespace fw_update
{
using ErrorCode = uint8_t;
using OemMessage = std::string;
using OemResolution = std::string;
using CompCompatibilityMessageId = std::string;
using CompCompatibilityMessage = std::string;
using CompCompatibilityResolution = std::string;
enum class NvidiaUpdateMessage;

struct MessageMapping
{
    OemMessage message;
    OemResolution resolution;
    std::optional<NvidiaUpdateMessage> nvidiaMessage{};
};
struct ComponentCompatibilityMessageMapping
{
    CompCompatibilityMessageId messageId;
    CompCompatibilityMessage message;
    CompCompatibilityResolution resolution;
    std::optional<NvidiaUpdateMessage> nvidiaMessage{};
};
using ErrorMapping = std::unordered_map<ErrorCode, MessageMapping>;
using CompCompatibilityMapping =
    std::unordered_map<ErrorCode, ComponentCompatibilityMessageMapping>;
using CommandMapping = std::map<pldm_firmware_update_commands, ErrorMapping>;
using CommandToCompCompatibilityMap =
    std::map<pldm_firmware_update_commands, CompCompatibilityMapping>;

#ifdef OEM_NVIDIA
ErrorCode constexpr reqGrantError = 0x70;
ErrorCode constexpr writeProtectEnabled = 0x71;
ErrorCode constexpr internalError = 0x72;
ErrorCode constexpr imageIdentical = 0x90;
ErrorCode constexpr metadataAuthFailure = 0x91;
ErrorCode constexpr secVersionCheckFailure = 0x93;
ErrorCode constexpr secKeysReovked = 0x94;
ErrorCode constexpr imageAuthFailure = 0x95;
ErrorCode constexpr skuMismatch = 0x97;
ErrorCode constexpr firmwarePackageSizeFailure = 0x98;
ErrorCode constexpr apReqGrantOnHold = 0x99;
ErrorCode constexpr applyAuthFailure = 0xB0;
ErrorCode constexpr fwUpdateBlocked = 0xD0;
#endif

enum class NvidiaUpdateMessage
{
    firmwarePackageNoMatchingDevices,
    requestedTargetUnavailableForUpdate,
    requestedTargetNotSupportedByPackage,
    firmwarePackageHeaderUnavailable,
    firmwarePackageHeaderInvalid,
    packageHeaderChecksumMismatch,
    packagePayloadChecksumMismatch,
    firmwarePackageMetadataInvalid,
    signatureHeaderMalformed,
    signatureHeaderVersionUnsupported,
    packageSignatureIntegrityFailed,
    packageSignatureVerificationFailed,
    packageSignatureRequired,
    componentImageIdentical,
    firmwareComponentComparisonStampLower,
    firmwareDownloadTimedOut,
    firmwareUpdateBlockedByWriteProtection,
    firmwareTransferCompletionTimedOut,
    firmwareVerificationCompletionTimedOut,
    firmwareMetadataAuthenticationFailed,
    firmwareImageSecurityVersionRejected,
    firmwareImageSigningKeyRevoked,
    firmwareImageAuthenticationFailed,
    firmwareImageSkuMismatch,
    firmwareImageSizeVerificationFailed,
    firmwareImageGenericVerificationFailed,
    firmwareImageSecurityChecksFailed,
    firmwareImageTransferIncomplete,
    firmwareImageSecurityRevisionFailed,
    firmwareImageGenericError,
    applyCompleteTimedOut,
    pldmCommandTimedOut,
};

enum class NvidiaUpdateMessageArgumentType
{
    none,
    packageVersion,
    checksumPair,
    packageVersionChecksumPair,
    packageVersionReason,
    packageVersionSignatureVersion,
    target,
    targetPackageVersion,
    componentPackageVersionComparisonStamp,
    componentVersionPackageVersionReason,
    componentTransferProgress,
    component,
    componentVersionPackageVersion,
    commandComponent,
};

enum class NvidiaUpdateMessageSeverity
{
    informational,
    critical,
};

struct NvidiaUpdateMessageDefinition
{
    std::string_view messageId;
    std::string_view resolution;
    NvidiaUpdateMessageArgumentType argumentType;
    NvidiaUpdateMessageSeverity severity;
    std::string_view reason{};
};

/** @brief Return the Redfish definition for a typed NVIDIA update failure. */
inline std::optional<NvidiaUpdateMessageDefinition>
    getNvidiaUpdateMessageDefinition(
        [[maybe_unused]] NvidiaUpdateMessage message)
{
#ifndef OEM_NVIDIA
    return std::nullopt;
#else
    switch (message)
    {
        case NvidiaUpdateMessage::firmwarePackageNoMatchingDevices:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.FirmwarePackageNoMatchingDevices",
                "Obtain a valid firmware package for this platform from a trusted source that contains images for the devices in this system, then retry the firmware update.",
                NvidiaUpdateMessageArgumentType::packageVersion,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::requestedTargetUnavailableForUpdate:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.RequestedTargetUnavailableForUpdate",
                "Power on the target and verify its management connection, then retry the firmware update. If the target remains unavailable, follow the platform firmware recovery procedure.",
                NvidiaUpdateMessageArgumentType::target,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::requestedTargetNotSupportedByPackage:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.RequestedTargetNotSupportedByPackage",
                "Obtain a valid firmware package for this platform from a trusted source that contains an image for the requested target, then retry the firmware update.",
                NvidiaUpdateMessageArgumentType::targetPackageVersion,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::firmwarePackageHeaderUnavailable:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.FirmwarePackageHeaderUnavailable",
                "Obtain a valid firmware package for this platform from a trusted source, verify that it was transferred without corruption, then retry the firmware update. If the failure persists, collect the logs and package details, then contact support.",
                NvidiaUpdateMessageArgumentType::none,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::firmwarePackageHeaderInvalid:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.FirmwarePackageHeaderInvalid",
                "Obtain a valid firmware package for this platform from a trusted source, verify that it was transferred without corruption, then retry the firmware update. If the failure persists, collect the logs and package details, then contact support.",
                NvidiaUpdateMessageArgumentType::none,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::packageHeaderChecksumMismatch:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.PackageHeaderChecksumMismatch",
                "Obtain a valid firmware package for this platform from a trusted source, verify that it was transferred without corruption, then retry the firmware update. If the failure persists, collect the logs and package details, then contact support.",
                NvidiaUpdateMessageArgumentType::checksumPair,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::packagePayloadChecksumMismatch:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.PackagePayloadChecksumMismatch",
                "Obtain a valid firmware package for this platform from a trusted source, verify that it was transferred without corruption, then retry the firmware update. If the failure persists, collect the logs and package details, then contact support.",
                NvidiaUpdateMessageArgumentType::packageVersionChecksumPair,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::firmwarePackageMetadataInvalid:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.FirmwarePackageMetadataInvalid",
                "Obtain a valid firmware package for this platform from a trusted source, verify that it was transferred without corruption, then retry the firmware update. If the failure persists, collect the logs and package details, then contact support.",
                NvidiaUpdateMessageArgumentType::none,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::signatureHeaderMalformed:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.SignatureHeaderMalformed",
                "Obtain a valid firmware package for this platform from a trusted source, verify that it was transferred without corruption, then retry the firmware update. If the failure persists, collect the logs and package details, then contact support.",
                NvidiaUpdateMessageArgumentType::packageVersionReason,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::signatureHeaderVersionUnsupported:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.SignatureHeaderVersionUnsupported",
                "Obtain a valid firmware package for this platform from a trusted source that uses a supported NVIDIA signature-header version, then retry the firmware update.",
                NvidiaUpdateMessageArgumentType::packageVersionSignatureVersion,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::packageSignatureIntegrityFailed:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.PackageSignatureIntegrityFailed",
                "Obtain a valid firmware package for this platform from a trusted source, verify that it was transferred without corruption, then retry the firmware update. If the failure persists, collect the logs and package details, then contact support.",
                NvidiaUpdateMessageArgumentType::packageVersion,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::packageSignatureVerificationFailed:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.PackageSignatureVerificationFailed",
                "Obtain a valid firmware package for this platform from a trusted source that is signed by an authority trusted by this BMC, then retry the firmware update. If verification still fails with a known-good package, verify that the BMC is provisioned with the required verification key, then collect the logs and contact support.",
                NvidiaUpdateMessageArgumentType::packageVersion,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::packageSignatureRequired:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.PackageSignatureRequired",
                "Obtain a valid firmware package for this platform from a trusted source that is signed by an authority trusted by this BMC and carries the NVIDIA signature header, then retry the firmware update.",
                NvidiaUpdateMessageArgumentType::packageVersion,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::componentImageIdentical:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.ComponentImageIdentical",
                "No action is required. To reinstall the same firmware, set the Redfish ForceUpdate parameter to true.",
                NvidiaUpdateMessageArgumentType::
                    componentPackageVersionComparisonStamp,
                NvidiaUpdateMessageSeverity::informational};
        case NvidiaUpdateMessage::firmwareComponentComparisonStampLower:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.ComponentDowngradeRejected",
                "Use a firmware package containing an upgrade for the installed component. To intentionally downgrade firmware, set the Redfish ForceUpdate parameter to true, then retry the firmware update.",
                NvidiaUpdateMessageArgumentType::
                    componentPackageVersionComparisonStamp,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::firmwareDownloadTimedOut:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.FirmwareDownloadTimedOut",
                "Retry the firmware update. If the failure persists, collect the BMC and target-device logs and contact support.",
                NvidiaUpdateMessageArgumentType::componentTransferProgress,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::firmwareUpdateBlockedByWriteProtection:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.FirmwareUpdateBlockedByWriteProtection",
                "Disable write protection on the device, then retry the firmware update.",
                NvidiaUpdateMessageArgumentType::component,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::firmwareTransferCompletionTimedOut:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.FirmwareTransferCompletionTimedOut",
                "Retry the firmware update. If the failure persists, collect the BMC and target-device logs and contact support.",
                NvidiaUpdateMessageArgumentType::component,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::firmwareVerificationCompletionTimedOut:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.FirmwareVerificationCompletionTimedOut",
                "Retry the firmware update. If the failure persists, collect the BMC and target-device logs and contact support.",
                NvidiaUpdateMessageArgumentType::component,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::firmwareMetadataAuthenticationFailed:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.ComponentMetadataAuthenticationFailed",
                "Use a firmware package containing a correctly signed image for the target device, then retry the firmware update. If the failure persists, verify the target device's trusted-key provisioning, then collect the BMC and target-device logs and contact support.",
                NvidiaUpdateMessageArgumentType::componentVersionPackageVersion,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::firmwareImageSecurityVersionRejected:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.ComponentImageSecurityVersionRejected",
                "Use a firmware package containing an image whose security version meets or exceeds the device's current security version, then retry the firmware update. If the failure persists, collect the BMC and target-device logs and contact support.",
                NvidiaUpdateMessageArgumentType::componentVersionPackageVersion,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::firmwareImageSigningKeyRevoked:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.ComponentImageSigningKeyRevoked",
                "Use a firmware package signed with a key that the target device trusts. In production systems, this error indicates that a debug-signed image update was attempted without the debug token installed. Verify the debug token installation status using the LogService.CollectDiagnosticData API with the DebugTokenStatus parameter. If the tokens are not installed, follow the debug token installation steps, then retry the firmware update.",
                NvidiaUpdateMessageArgumentType::componentVersionPackageVersion,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::firmwareImageAuthenticationFailed:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.ComponentImageAuthenticationFailed",
                "Use a firmware package containing a correctly signed image for the target device, then retry the firmware update. If the failure persists, collect the BMC and target-device logs and contact support.",
                NvidiaUpdateMessageArgumentType::componentVersionPackageVersion,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::firmwareImageSkuMismatch:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.ComponentImageSkuMismatch",
                "Use a firmware package containing an image for the target SKU, then retry the firmware update. If the failure persists, collect the BMC and target-device logs and contact support.",
                NvidiaUpdateMessageArgumentType::componentVersionPackageVersion,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::firmwareImageSizeVerificationFailed:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.ComponentImageVerificationFailed",
                "Retry the firmware update with a known-good firmware package. If the failure persists, collect the BMC and target-device logs and contact support.",
                NvidiaUpdateMessageArgumentType::
                    componentVersionPackageVersionReason,
                NvidiaUpdateMessageSeverity::critical,
                "image size check failed (VerifyComplete: OEM firmwarePackageSizeFailure (0x98))"};
        case NvidiaUpdateMessage::firmwareImageGenericVerificationFailed:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.ComponentImageVerificationFailed",
                "Retry the firmware update with a known-good firmware package. If the failure persists, collect the BMC and target-device logs and contact support.",
                NvidiaUpdateMessageArgumentType::
                    componentVersionPackageVersionReason,
                NvidiaUpdateMessageSeverity::critical,
                "generic verification failure (VerifyComplete: PLDM_FWUP_VERIFY_ERROR_VERIFICATION_FAILURE (0x01))"};
        case NvidiaUpdateMessage::firmwareImageSecurityChecksFailed:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.ComponentImageVerificationFailed",
                "Retry the firmware update with a known-good firmware package. If the failure persists, collect the BMC and target-device logs and contact support.",
                NvidiaUpdateMessageArgumentType::
                    componentVersionPackageVersionReason,
                NvidiaUpdateMessageSeverity::critical,
                "did not pass the device's security checks (VerifyComplete: PLDM_FWUP_VERIFY_FAILED_FD_SECURITY_CHECKS (0x03))"};
        case NvidiaUpdateMessage::firmwareImageTransferIncomplete:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.ComponentImageVerificationFailed",
                "Retry the firmware update with a known-good firmware package. If the failure persists, collect the BMC and target-device logs and contact support.",
                NvidiaUpdateMessageArgumentType::
                    componentVersionPackageVersionReason,
                NvidiaUpdateMessageSeverity::critical,
                "image transfer was incomplete (VerifyComplete: PLDM_FWUP_VERIFY_ERROR_IMAGE_INCOMPLETE (0x04))"};
        case NvidiaUpdateMessage::firmwareImageSecurityRevisionFailed:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.ComponentImageVerificationFailed",
                "Retry the firmware update with a known-good firmware package. If the failure persists, collect the BMC and target-device logs and contact support.",
                NvidiaUpdateMessageArgumentType::
                    componentVersionPackageVersionReason,
                NvidiaUpdateMessageSeverity::critical,
                "security version does not meet the device's requirement (VerifyComplete: PLDM_FWUP_VERIFY_FAILURE_SECURITY_REVISION_ERROR (0x10))"};
        case NvidiaUpdateMessage::firmwareImageGenericError:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.ComponentImageVerificationFailed",
                "Retry the firmware update with a known-good firmware package. If the failure persists, collect the BMC and target-device logs and contact support.",
                NvidiaUpdateMessageArgumentType::
                    componentVersionPackageVersionReason,
                NvidiaUpdateMessageSeverity::critical,
                "generic verification failure (VerifyComplete: PLDM_FWUP_GENERIC_ERROR (0x0A))"};
        case NvidiaUpdateMessage::applyCompleteTimedOut:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.ApplyCompleteTimedOut",
                "Retry the firmware update. If the failure persists, collect the BMC and target-device logs and contact support.",
                NvidiaUpdateMessageArgumentType::component,
                NvidiaUpdateMessageSeverity::critical};
        case NvidiaUpdateMessage::pldmCommandTimedOut:
            return NvidiaUpdateMessageDefinition{
                "NvidiaUpdate.1.3.PLDMCommandTimedOut",
                "Retry the firmware update. If the failure persists, collect the BMC and target-device logs and contact support.",
                NvidiaUpdateMessageArgumentType::commandComponent,
                NvidiaUpdateMessageSeverity::critical};
        default:
            break;
    }
#endif
    return std::nullopt;
}

/** @brief Facts a caller can supply for a Redfish update message.
 *
 *  logNvidiaUpdateMessage orders the arguments from argumentType.
 *  Set only the fields that message uses.
 */
struct NvidiaUpdateMessageFacts
{
    std::string_view packageVersion{};
    std::string_view target{};
    std::string_view component{};
    std::string_view componentVersion{};
    std::string_view command{};
    std::string_view reason{};
    std::string_view signatureVersion{};
    std::optional<uint32_t> comparisonStamp{};
    std::optional<uint32_t> expectedChecksum{};
    std::optional<uint32_t> calculatedChecksum{};
    std::optional<uint32_t> bytesTransferred{};
    std::optional<uint32_t> componentSize{};
};

/** @brief Name of a PLDM firmware update command for message arguments. */
inline std::string_view firmwareUpdateCommandName(
    pldm_firmware_update_commands command)
{
    switch (command)
    {
        case PLDM_QUERY_DEVICE_IDENTIFIERS:
            return "QueryDeviceIdentifiers";
        case PLDM_GET_FIRMWARE_PARAMETERS:
            return "GetFirmwareParameters";
        case PLDM_REQUEST_UPDATE:
            return "RequestUpdate";
        case PLDM_PASS_COMPONENT_TABLE:
            return "PassComponentTable";
        case PLDM_UPDATE_COMPONENT:
            return "UpdateComponent";
        case PLDM_REQUEST_FIRMWARE_DATA:
            return "RequestFirmwareData";
        case PLDM_TRANSFER_COMPLETE:
            return "TransferComplete";
        case PLDM_VERIFY_COMPLETE:
            return "VerifyComplete";
        case PLDM_APPLY_COMPLETE:
            return "ApplyComplete";
        case PLDM_ACTIVATE_FIRMWARE:
            return "ActivateFirmware";
        case PLDM_GET_STATUS:
            return "GetStatus";
        case PLDM_CANCEL_UPDATE_COMPONENT:
            return "CancelUpdateComponent";
        case PLDM_CANCEL_UPDATE:
            return "CancelUpdate";
        default:
            return "";
    }
}

#ifdef OEM_NVIDIA

inline std::string nvidiaUpdateHex(uint32_t value, int width)
{
    return std::format("0x{:0{}X}", value, width);
}

inline std::vector<std::string> nvidiaUpdateMessageArguments(
    const NvidiaUpdateMessageDefinition& definition,
    const NvidiaUpdateMessageFacts& facts)
{
    using Arg = NvidiaUpdateMessageArgumentType;
    const auto checksum = [](std::optional<uint32_t> value) {
        return nvidiaUpdateHex(value.value_or(0), 8);
    };
    std::string reason{facts.reason};
    if (reason.empty())
    {
        reason.assign(definition.reason);
    }

    switch (definition.argumentType)
    {
        case Arg::none:
            return {};
        case Arg::packageVersion:
            return {std::string{facts.packageVersion}};
        case Arg::checksumPair:
            return {checksum(facts.expectedChecksum),
                    checksum(facts.calculatedChecksum)};
        case Arg::packageVersionChecksumPair:
            return {std::string{facts.packageVersion},
                    checksum(facts.expectedChecksum),
                    checksum(facts.calculatedChecksum)};
        case Arg::packageVersionReason:
            return {std::string{facts.packageVersion}, reason};
        case Arg::packageVersionSignatureVersion:
            return {std::string{facts.packageVersion},
                    std::string{facts.signatureVersion}};
        case Arg::target:
            return {std::string{facts.target}};
        case Arg::targetPackageVersion:
            return {std::string{facts.target},
                    std::string{facts.packageVersion}};
        case Arg::componentPackageVersionComparisonStamp:
            return {std::string{facts.component},
                    std::string{facts.packageVersion},
                    checksum(facts.comparisonStamp)};
        case Arg::componentVersionPackageVersionReason:
            return {std::string{facts.component},
                    std::string{facts.componentVersion},
                    std::string{facts.packageVersion}, reason};
        case Arg::componentTransferProgress:
            return {std::string{facts.component},
                    std::to_string(facts.bytesTransferred.value_or(0)),
                    std::to_string(facts.componentSize.value_or(0))};
        case Arg::component:
            return {std::string{facts.component}};
        case Arg::componentVersionPackageVersion:
            return {std::string{facts.component},
                    std::string{facts.componentVersion},
                    std::string{facts.packageVersion}};
        case Arg::commandComponent:
            return {std::string{facts.command}, std::string{facts.component}};
    }
    return {};
}

inline void logNvidiaUpdateMessage(
    const NvidiaUpdateMessageDefinition& definition,
    const NvidiaUpdateMessageFacts& facts)
{
    const auto level =
        definition.severity == NvidiaUpdateMessageSeverity::informational
            ? LogLevel::Informational
            : LogLevel::Critical;
    createLogEntry(std::string{definition.messageId},
                   nvidiaUpdateMessageArguments(definition, facts),
                   std::string{definition.resolution}, "FWUpdate", level);
}

inline void logNvidiaUpdateMessage(NvidiaUpdateMessage message,
                                   const NvidiaUpdateMessageFacts& facts = {})
{
    const auto definition = getNvidiaUpdateMessageDefinition(message);
    if (!definition)
    {
        error("No Redfish definition for NVIDIA update message {MSG}", "MSG",
              static_cast<unsigned>(message));
        return;
    }
    logNvidiaUpdateMessage(*definition, facts);
}

inline void logFirmwareUpdateMessage(NvidiaUpdateMessage message,
                                     const NvidiaUpdateMessageFacts& facts = {})
{
    logNvidiaUpdateMessage(message, facts);
}

inline void logFirmwareUpdateMessage(
    const NvidiaUpdateMessageDefinition& definition,
    const NvidiaUpdateMessageFacts& facts)
{
    logNvidiaUpdateMessage(definition, facts);
}
#else

inline void logFirmwareUpdateMessage(NvidiaUpdateMessage,
                                     const NvidiaUpdateMessageFacts& = {})
{}

inline void logFirmwareUpdateMessage(const NvidiaUpdateMessageDefinition&,
                                     const NvidiaUpdateMessageFacts&)
{}

#endif

constexpr uint8_t maxDecodeFailureRetries = 3;

/* request update error mapping */
static ErrorMapping requestUpdateMapping{
    {PLDM_FWUP_TIME_OUT,
     {"The device did not respond to the update request, so the update failed.",
      "Retry firmware update operation"}},
    {PLDM_FWUP_ALREADY_IN_UPDATE_MODE,
     {"Device is already in update mode", "Retry firmware update operation"}},
    {PLDM_FWUP_INVALID_STATE_FOR_COMMAND,
     {"Invalid state in FD while initiating firmware update",
      "Retry firmware update operation"}},
    {PLDM_FWUP_UNABLE_TO_INITIATE_UPDATE,
     {"Device is not able to enter into update mode to start firmware update",
#ifdef OEM_NVIDIA
      "Wait for background copy operation to complete and rate"
      " limit threshold to be cleared."
#else
      "Retry firmware update operation"
#endif
     }},
    {PLDM_FWUP_RETRY_REQUEST_UPDATE,
     {"Device needs more time to prepare for firmware update",
      "Retry firmware update operation"}},
    {PLDM_ERROR,
     {"Received an invalid or corrupted response for the update request; so the update failed.",
      "Retry firmware update operation"}},
    {PLDM_ERROR_INVALID_DATA,
     {"Received a response with invalid data for the update request; so the update failed.",
      "Verify the contents of the FW package and retry firmware update operation"}},
    {PLDM_ERROR_INVALID_LENGTH,
     {"Received a response with invalid length for the update request; so the update failed.",
      "Verify the contents of the FW package and retry firmware update operation"}},
    {PLDM_ERROR_NOT_READY,
     {"Device is not ready to process the update request",
      "Retry firmware update operation"}},
};

/* pass component table error mapping */
static ErrorMapping passComponentTblMapping{
    {PLDM_FWUP_TIME_OUT,
     {"The device did not respond to the request containing the list of components to be updated, so the update failed.",
      "Retry firmware update operation"}},
    {PLDM_FWUP_NOT_IN_UPDATE_MODE,
     {"Device is not in update mode", "Retry firmware update operation"}},
    {PLDM_FWUP_INVALID_STATE_FOR_COMMAND,
     {"Invalid state in FD while initiating firmware update",
      "Retry firmware update operation"}},
    {PLDM_FWUP_PACKAGE_DATA_ERROR,
     {"Device received invalid package data for the component table request; so the update failed.",
      "Verify the contents of the FW package and retry firmware update operation"}},
    {PLDM_ERROR,
     {"Received an invalid or corrupted response for the request containing the list of components to be updated; so the update failed.",
      "Retry firmware update operation"}},
    {PLDM_ERROR_INVALID_DATA,
     {"Received a response with invalid data for the component table request; so the update failed.",
      "Verify the contents of the FW package and retry firmware update operation"}},
    {PLDM_ERROR_INVALID_LENGTH,
     {"Received a response with invalid length for the component table request; so the update failed.",
      "Verify the contents of the FW package and retry firmware update operation"}},
    {PLDM_ERROR_NOT_READY,
     {"Device is not ready to process the component table request",
      "Retry firmware update operation"}},
};
/* update component error mapping */
static ErrorMapping updateComponentMapping{
    {PLDM_FWUP_TIME_OUT,
     {"The device did not respond to the request to update the component, so the update failed.",
      "Retry firmware update operation"}},
    {PLDM_FWUP_NOT_IN_UPDATE_MODE,
     {"Device is not in update mode", "Retry firmware update operation"}},
    {PLDM_FWUP_INVALID_STATE_FOR_COMMAND,
     {"Invalid state in FD while initiating component update",
      "Retry firmware update operation"}},
    {PLDM_FWUP_BUSY_IN_BACKGROUND,
     {"Cannot execute command because device performing other critical tasks",
      "Retry firmware update operation"}},
    {PLDM_ERROR,
     {"Received an invalid or corrupted response for the request to update the component; so the update failed.",
      "Retry firmware update operation"}},
    {PLDM_ERROR_INVALID_DATA,
     {"Received a response with invalid data for the update component request; so the update failed.",
      "Verify the contents of the FW package and retry firmware update operation"}},
    {PLDM_ERROR_INVALID_LENGTH,
     {"Received a response with invalid length for the update component request; so the update failed.",
      "Verify the contents of the FW package and retry firmware update operation"}},
    {PLDM_ERROR_NOT_READY,
     {"Device is not ready to process the update component request",
      "Retry firmware update operation"}},
};

/* request firmware data error mapping */
static ErrorMapping requestFwDataMapping{
    {PLDM_FWUP_TIME_OUT,
     {"The device stopped requesting component data during the download, so the update failed.",
      "Retry firmware update operation"}},
    {PLDM_ERROR,
     {"Received an invalid or corrupted request to fetch component data; so the update failed.",
      "Retry firmware update operation"}}};

/* transfer complete error mapping */
static ErrorMapping transferCompleteMapping{
    {PLDM_FWUP_TRANSFER_ERROR_VERSION_MISMATCH,
     {"Version mismatch", "Verify the contents of the FW package"}},
    {PLDM_FWUP_TIME_OUT,
     {"The device did not send the request indicating that the component data transfer was complete within the operation timeout, so the update failed.",
      "Retry firmware update operation",
      NvidiaUpdateMessage::firmwareTransferCompletionTimedOut}},
    {PLDM_ERROR,
     {"Received an invalid or corrupted request for the component transfer status; so the update failed.",
      "Retry firmware update operation"}},
    {PLDM_FWUP_TRANSFER_ERROR_IMAGE_CORRUPT,
     {"Component image is corrupt", "Verify the contents of the FW package"}},
    {PLDM_FWUP_FD_ABORTED_TRANSFER,
     {"Component image transfer aborted by FD",
      "Retry firmware update operation"}},
    {PLDM_FWUP_FD_ABORTED_TRANSFER_LOW_POWER_STATE,
     {"Component image transfer aborted due to low power state",
      "Ensure device has sufficient power and retry firmware update operation"}},
    {PLDM_FWUP_FD_ABORTED_TRANSFER_RESET_NEEDED,
     {"Component image transfer aborted because reset is needed",
      "Reset the device and retry firmware update operation"}},
    {PLDM_FWUP_FD_ABORTED_TRANSFER_STORAGE_ISSUE,
     {"Component image transfer aborted due to storage issue",
      "Retry firmware update operation"}},
    {PLDM_FWUP_FD_ABORTED_TRANSFER_INVALID_COMPONENT_OPAQUE_DATA,
     {"Component image transfer aborted due to invalid component opaque data",
      "Verify the contents of the FW package"}},
    {PLDM_FWUP_FD_ABORTED_TRANSFER_SECURITY_REVISION_ERROR,
     {"Component image transfer aborted due to security revision error",
      "Verify the contents of the FW package"}},
    {PLDM_FWUP_GENERIC_ERROR,
     {"Component image transfer failed with a generic error.",
      "Retry firmware update operation"}},
#ifdef OEM_NVIDIA
    {reqGrantError,
     {"SPI Access Error",
      "Make sure device AP flash is not accessed by other application and"
      " retry the firmware update operation."}},
    {writeProtectEnabled,
     {"Write Protect Enabled",
      "Disable write protect on the device and retry the firmware update"
      " operation.",
      NvidiaUpdateMessage::firmwareUpdateBlockedByWriteProtection}},
    {internalError, {"Internal Error", "Retry firmware update operation"}},
#endif
};

/* verify result error mapping */
static ErrorMapping verifyCompleteMapping{
    {PLDM_FWUP_VERIFY_ERROR_VERSION_MISMATCH,
     {"Version mismatch", "Verify the contents of the FW package"}},
    {PLDM_FWUP_TIME_OUT,
     {"The device did not send the request indicating that the verification of the component image was complete within the operation timeout, so the update failed.",
      "Retry firmware update operation",
      NvidiaUpdateMessage::firmwareVerificationCompletionTimedOut}},
    // PLDM_ERROR and PLDM_FWUP_VERIFY_ERROR_VERIFICATION_FAILURE both use
    // 0x01. Preserve the legacy decode-error text while attaching the NVIDIA
    // message selected for the VerifyComplete result.
    {PLDM_ERROR,
     {"Received an invalid or corrupted request for the component verification status; so the update failed.",
      "Retry firmware update operation",
      NvidiaUpdateMessage::firmwareImageGenericVerificationFailed}},
    {PLDM_FWUP_VERIFY_FAILED_FD_SECURITY_CHECKS,
     {"Component image verification failed due to device security checks",
      "Verify the contents of the FW package",
      NvidiaUpdateMessage::firmwareImageSecurityChecksFailed}},
    {PLDM_FWUP_VERIFY_ERROR_IMAGE_INCOMPLETE,
     {"Component image verification failed due to incomplete image transfer",
      "Verify the contents of the FW package",
      NvidiaUpdateMessage::firmwareImageTransferIncomplete}},
    {PLDM_FWUP_VERIFY_FAILURE_SECURITY_REVISION_ERROR,
     {"Component image verification failed due to security revision error",
      "Verify the contents of the FW package",
      NvidiaUpdateMessage::firmwareImageSecurityRevisionFailed}},
    {PLDM_FWUP_GENERIC_ERROR,
     {"Component image verification failed with a generic error.",
      "Retry firmware update operation",
      NvidiaUpdateMessage::firmwareImageGenericError}},
#ifdef OEM_NVIDIA
    {imageIdentical,
     {"Component image is identical",
      "Retry firmware update operation with the force flag"}},
    {metadataAuthFailure,
     {"MetaData authentication failure",
      "Verify the contents of the FW package",
      NvidiaUpdateMessage::firmwareMetadataAuthenticationFailed}},
    {secVersionCheckFailure,
     {"Security version check failed", "Verify the contents of the FW package",
      NvidiaUpdateMessage::firmwareImageSecurityVersionRejected}},
    {secKeysReovked,
     {"Image authentication failed because security keys used to sign the"
      " image has been revoked",
      "Verify the contents of the FW package. In "
      "production systems, this error suggests that a debug-signed image update"
      " was attempted without the debug token being installed. To resolve this,"
      " verify the debug token installation status using the "
      "LogService.CollectDiagnosticData API with the DebugTokenStatus parameter."
      " If the tokens are not installed, follow the debug token installation"
      " steps and retry the update.",
      NvidiaUpdateMessage::firmwareImageSigningKeyRevoked}},
    {imageAuthFailure,
     {"Component image authentication check failed",
      "Verify the contents of the FW package",
      NvidiaUpdateMessage::firmwareImageAuthenticationFailed}},
    {skuMismatch,
     {"SKU mismatch", "Verify the contents of the FW package",
      NvidiaUpdateMessage::firmwareImageSkuMismatch}},
    {firmwarePackageSizeFailure,
     {"Firmware image size is incorrect",
      "Verify the contents of the FW package",
      NvidiaUpdateMessage::firmwareImageSizeVerificationFailed}},
    {apReqGrantOnHold,
     {"AP request grant on hold", "Retry firmware update operation"}}
#endif
};

/* apply complete command error mapping */
static ErrorMapping applyCompleteMapping{
    {PLDM_FWUP_TIME_OUT,
     {"The device did not send the request indicating that the component image was applied within the operation timeout, so the update failed.",
      "Retry firmware update operation",
      NvidiaUpdateMessage::applyCompleteTimedOut}},
    {PLDM_FWUP_APPLY_FAILURE_MEMORY_ISSUE,
     {"Applying the image failed due to write operation failure",
      "Retry firmware update operation."}},
    {PLDM_ERROR,
     {"Received an invalid or corrupted request for the component apply status; so the update failed.",
      "Retry firmware update operation."}},
    {PLDM_FWUP_APPLY_FAILURE_SECURITY_REVISION_ERROR,
     {"Applying the image failed due to security revision error",
      "Verify the contents of the FW package"}},
    {PLDM_FWUP_GENERIC_ERROR,
     {"Applying the image failed with a generic error.",
      "Retry firmware update operation"}},
#ifdef OEM_NVIDIA
    {applyAuthFailure,
     {"Authentication failed after applying the image",
      "Retry firmware update operation."}}
#endif
};

/* activate firmware error mapping */
static ErrorMapping activateFirmwareMapping{
    {PLDM_FWUP_TIME_OUT,
     {"The device did not respond to the activation request, so the update failed.",
      "Retry firmware update operation"}},
    {PLDM_ERROR,
     {"Received an invalid or corrupted response for the firmware activation request; so the update failed.",
      "Retry firmware update operation."}},
    {PLDM_FWUP_INCOMPLETE_UPDATE,
     {"Firmware activation failed because a dependent component image update was unsuccessful.",
      "Verify the contents of the FW package and retry firmware update operation"}}};

/* get status command error mapping */
static ErrorMapping getStatusMapping{
    {COMMAND_TIMEOUT,
     {"The device did not respond to the device status request, and the communication timed out.",
      "Retry firmware update operation.",
      NvidiaUpdateMessage::pldmCommandTimedOut}},
    {PLDM_ERROR,
     {"Received an invalid or corrupted response for the device status request; so the update failed.",
      "Retry firmware update operation."}}};

static CompCompatibilityMapping updateComponentResponseCodeMapping{
    // Reported by ComponentImageIdentical alone; no legacy entry.
    {PLDM_CCRC_COMP_COMPARISON_STAMP_IDENTICAL,
     {{}, {}, {}, NvidiaUpdateMessage::componentImageIdentical}},
    {PLDM_CCRC_COMP_COMPARISON_STAMP_LOWER,
     {"ResourceEvent.1.0.ResourceErrorsDetected",
      "Component comparison stamp is lower than the firmware component comparison stamp in the FD",
      "Retry firmware update operation with the force flag",
      NvidiaUpdateMessage::firmwareComponentComparisonStampLower}},
    {PLDM_CCRC_INCOMPLETE_COMP_IMAGE_SET,
     {"ResourceEvent.1.0.ResourceErrorsDetected",
      "Incomplete component image set",
      "Retry Firmware Update with complete component image set"}},
#ifdef OEM_NVIDIA
    {fwUpdateBlocked,
     {"ResourceEvent.1.0.ResourceErrorsDetected",
      "FW update blocked to preserve valid firmware on atleast one slot since background copy operation failed.",
      "Reset AP to complete background copy operation and retry firmware update."}}
#endif
};

/* Error mapping table for each pldm command */
static const CommandMapping commandMappingTbl = {
    {PLDM_REQUEST_UPDATE, requestUpdateMapping},
    {PLDM_PASS_COMPONENT_TABLE, passComponentTblMapping},
    {PLDM_UPDATE_COMPONENT, updateComponentMapping},
    {PLDM_REQUEST_FIRMWARE_DATA, requestFwDataMapping},
    {PLDM_TRANSFER_COMPLETE, transferCompleteMapping},
    {PLDM_VERIFY_COMPLETE, verifyCompleteMapping},
    {PLDM_APPLY_COMPLETE, applyCompleteMapping},
    {PLDM_ACTIVATE_FIRMWARE, activateFirmwareMapping},
    {PLDM_GET_STATUS, getStatusMapping}};

static const CommandToCompCompatibilityMap CommandToCompCompatibilityTbl = {
    {PLDM_UPDATE_COMPONENT, updateComponentResponseCodeMapping},
};

/** @brief Return the NVIDIA message attached to an existing UpdateComponent
 *         compatibility mapping. */
inline std::optional<NvidiaUpdateMessageDefinition>
    getUpdateComponentMessageDefinition(uint8_t compatibilityResponseCode)
{
#ifndef OEM_NVIDIA
    static_cast<void>(compatibilityResponseCode);
    return std::nullopt;
#else
    const auto command =
        CommandToCompCompatibilityTbl.find(PLDM_UPDATE_COMPONENT);
    if (command == CommandToCompCompatibilityTbl.end())
    {
        return std::nullopt;
    }
    const auto mapping = command->second.find(compatibilityResponseCode);
    if (mapping == command->second.end() || !mapping->second.nvidiaMessage)
    {
        return std::nullopt;
    }
    return getNvidiaUpdateMessageDefinition(*mapping->second.nvidiaMessage);
#endif
}

/** @brief Return the optional NVIDIA message attached to an existing
 *         command/error mapping. */
inline std::optional<NvidiaUpdateMessage> nvidiaCommandMessage(
    pldm_firmware_update_commands commandType, uint8_t result)
{
#ifndef OEM_NVIDIA
    static_cast<void>(commandType);
    static_cast<void>(result);
    return std::nullopt;
#else
    const auto command = commandMappingTbl.find(commandType);
    if (command == commandMappingTbl.end())
    {
        return std::nullopt;
    }
    const auto mapping = command->second.find(result);
    if (mapping == command->second.end())
    {
        return std::nullopt;
    }
    return mapping->second.nvidiaMessage;
#endif
}

/** @brief Return a specific message for a PLDM command result. */
inline std::optional<NvidiaUpdateMessageDefinition>
    getNvidiaCommandMessageDefinition(pldm_firmware_update_commands commandType,
                                      uint8_t result)
{
    const auto message = nvidiaCommandMessage(commandType, result);
    if (!message)
    {
        return std::nullopt;
    }
    return getNvidiaUpdateMessageDefinition(*message);
}

/**
 * @brief Get the Oem Message for enhanced message registry
 *
 * @param[in] commandType - pldm command type
 * @param[in] errorCode - oem error code
 * @return true, message id, error and resolution - error code mapping is
 * present
 * @return false - error code mapping is not present
 */
inline std::tuple<bool, std::string, std::string, std::string> getOemMessage(
    const pldm_firmware_update_commands& commandType,
    const ErrorCode& errorCode)
{
    using namespace pldm::fw_update;
    bool status = false;
    std::string oemMessageId;
    OemMessage oemMessageError;
    OemResolution oemResolution;
    oemMessageId = "ResourceEvent.1.0.ResourceErrorsDetected";
    if (commandMappingTbl.contains(commandType))
    {
        auto commandMapping = commandMappingTbl.find(commandType);
        if (commandMapping->second.contains(errorCode))
        {
            auto errorCodeSearch = commandMapping->second.find(errorCode);
            oemMessageError = errorCodeSearch->second.message;
            oemResolution = errorCodeSearch->second.resolution;
            status = true;
        }
        else
        {
            error(
                "Error Code: {ERRORCODE} not found for command: {COMMANDTYPE}",
                "ERRORCODE", errorCode, "COMMANDTYPE", (unsigned)commandType);
        }
    }
    else
    {
        error("No mapping found for command: {COMMANDTYPE}", "COMMANDTYPE",
              (unsigned)commandType);
    }
    return {status, oemMessageId, oemMessageError, oemResolution};
}

/**
 * @brief Get the Component Compatibility Message for enhanced message registry
 *
 * @param[in] commandType - pldm command type
 * @param[in] errorCode - error code
 * @return true, message id, error and resolution - error code mapping is
 * present
 * @return false - error code mapping is not present
 */
inline std::tuple<bool, std::string, std::string, std::string>
    getCompCompatibilityMessage(
        const pldm_firmware_update_commands& commandType,
        const ErrorCode& errorCode)
{
    using namespace pldm::fw_update;
    bool status = false;
    CompCompatibilityMessageId messageId;
    CompCompatibilityMessage messageError;
    CompCompatibilityResolution resolution;
    if (CommandToCompCompatibilityTbl.contains(commandType))
    {
        auto commandMapping = CommandToCompCompatibilityTbl.find(commandType);
        if (commandMapping->second.contains(errorCode))
        {
            auto errorCodeSearch = commandMapping->second.find(errorCode);
            messageId = errorCodeSearch->second.messageId;
            messageError = errorCodeSearch->second.message;
            resolution = errorCodeSearch->second.resolution;
            status = !messageId.empty();
        }
        else
        {
            error(
                "Component Compatibility Response Code: {ERRORCODE} not found for command: {COMMANDTYPE}",
                "ERRORCODE", errorCode, "COMMANDTYPE", (unsigned)commandType);
        }
    }
    else
    {
        error(
            "No component compatibility response code mapping found for command: {COMMANDTYPE}",
            "COMMANDTYPE", (unsigned)commandType);
    }
    return {status, messageId, messageError, resolution};
}

} // namespace fw_update
} // namespace pldm
