#pragma once

#include "libpldm/firmware_update.h"

#include "common/types.hpp"

#include <phosphor-logging/lg2.hpp>

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
using MessageMapping = std::pair<OemMessage, OemResolution>;
using ComponentCompatibilityMessageMapping =
    std::tuple<CompCompatibilityMessageId, CompCompatibilityMessage, CompCompatibilityResolution>;
using ErrorMapping = std::unordered_map<ErrorCode, MessageMapping>;
using CompCompatibilityMapping =
    std::unordered_map<ErrorCode, ComponentCompatibilityMessageMapping>;
using CommandMapping = std::map<pldm_firmware_update_commands, ErrorMapping>;
using CommandToCompCompatibilityMap =
    std::map<pldm_firmware_update_commands, CompCompatibilityMapping>;

#ifdef OEM_NVIDIA
ErrorCode constexpr unableToInitiateUpdate = 0x8A;
ErrorCode constexpr reqGrantError = 0x70;
ErrorCode constexpr writeProtectEnabled = 0x71;
ErrorCode constexpr imageIdentical = 0x90;
ErrorCode constexpr metadataAuthFailure = 0x91;
ErrorCode constexpr secVersionCheckFailure = 0x93;
ErrorCode constexpr secKeysReovked = 0x94;
ErrorCode constexpr skuMismatch = 0x97;
#endif

/* request update error mapping */
static ErrorMapping requestUpdateMapping{
    {COMMAND_TIMEOUT,
     {"Initiating firmware update timed out",
      "Retry firmware update operation"}},
    {PLDM_FWUP_INVALID_STATE_FOR_COMMAND,
     {"Invalid state in FD while initiating firmware update",
      "Retry firmware update operation"}},
#ifdef OEM_NVIDIA
    {unableToInitiateUpdate,
     {"ERoT is busy", "Wait for background copy operation to complete and rate"
                      " limit threshold to be cleared."}},
#endif
};

/* pass component table error mapping */
static ErrorMapping passComponentTblMapping{
    {COMMAND_TIMEOUT,
     {"Initiating firmware update timed out",
      "Retry firmware update operation"}},
    {PLDM_FWUP_INVALID_STATE_FOR_COMMAND,
     {"Invalid state in FD while initiating firmware update",
      "Retry firmware update operation"}}};
/* update component error mapping */
static ErrorMapping updateComponentMapping{
    {COMMAND_TIMEOUT,
     {"Initiating component update timed out",
      "Retry firmware update operation"}},
    {PLDM_FWUP_INVALID_STATE_FOR_COMMAND,
     {"Invalid state in FD while initiating component update",
      "Retry firmware update operation"}}};

/* request firmware data error mapping */
static ErrorMapping requestFwDataMapping{
    {COMMAND_TIMEOUT,
     {"Transferring component timed out", "Retry firmware update operation"}}};

/* transfer complete error mapping */
static ErrorMapping transferCompleteMapping{
    {NO_MATCHING_VERSION,
     {"No Matching Version", "Verify the contents of the FW package"}},
    {COMMAND_TIMEOUT,
     {"Transferring component timed out", "Retry firmware update operation"}},
#ifdef OEM_NVIDIA
    {reqGrantError,
     {"SPI Access Error",
      "Make sure device AP flash is not accessed by other application and"
      " retry the firmware update operation."}},
    {writeProtectEnabled,
     {"Write Protect Enabled",
      "Disable write protect on the device and retry the firmware update"
      " operation."}},
#endif
};

/* verify result error mapping */
static ErrorMapping verifyCompleteMapping{
    {VERSION_MISMATCH,
     {"Version mismatch", "Verify the contents of the FW package"}},
    {COMMAND_TIMEOUT,
     {"Verifying component timed out", "Retry firmware update operation"}},
#ifdef OEM_NVIDIA
    {imageIdentical,
     {"Component image is identical",
      "Retry firmware update operation with the force flag"}},
    {metadataAuthFailure,
     {"MetaData authentication failure",
      "Verify the contents of the FW package"}},
    {secVersionCheckFailure,
     {"Security version check failed",
      "Verify the contents of the FW package"}},
    {secKeysReovked,
     {"Security keys revoked", "Verify the contents of the FW package"}},
    {skuMismatch, {"SKU mismatch", "Verify the contents of the FW package"}},
#endif
};

/* apply complete command error mapping */
static ErrorMapping applyCompleteMapping{
    {COMMAND_TIMEOUT,
     {"Complete Commands Timeout", "Retry firmware update operation."}}};

/* activate firmware error mapping */
static ErrorMapping activateFirmwareMapping{
    {COMMAND_TIMEOUT,
     {"Activating firmware timed out", "Retry firmware update operation."}}};

static CompCompatibilityMapping updateComponentResponseCodeMapping{
    {PLDM_CRC_COMP_COMPARISON_STAMP_IDENTICAL,
     {"OpenBMC.0.4.ComponentUpdateSkipped",
      "Component image is identical",
      "Retry firmware update operation with the force flag"}},
    {PLDM_CRC_COMP_COMPARISON_STAMP_LOWER,
     {"ResourceEvent.1.0.ResourceErrorsDetected",
      "Component comparison stamp is lower than the firmware component comparison stamp in the FD",
      "Retry firmware update operation with the force flag"}},
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
    {PLDM_ACTIVATE_FIRMWARE, activateFirmwareMapping}};

static const CommandToCompCompatibilityMap CommandToCompCompatibilityTbl = {
    {PLDM_UPDATE_COMPONENT, updateComponentResponseCodeMapping},
};

/**
 * @brief Get the Oem Message for enhanced message registry
 *
 * @param[in] commandType - pldm command type
 * @param[in] errorCode - oem error code
 * @return true, message id, error and resolution - error code mapping is
 * present
 * @return false - error code mapping is not present
 */
inline std::tuple<bool, std::string, std::string, std::string>
    getOemMessage(const pldm_firmware_update_commands& commandType,
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
            oemMessageError = errorCodeSearch->second.first;
            oemResolution = errorCodeSearch->second.second;
            status = true;
        }
        else
        {
            lg2::error(
                "Error Code: {ERRORCODE} not found for command: {COMMANDTYPE}",
                "ERRORCODE", errorCode, "COMMANDTYPE", (unsigned)commandType);
        }
    }
    else
    {
        lg2::error("No mapping found for command: {COMMANDTYPE}", "COMMANDTYPE",
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
            messageId = std::get<0>(errorCodeSearch->second);
            messageError = std::get<1>(errorCodeSearch->second);
            resolution = std::get<2>(errorCodeSearch->second);
            status = true;
        }
        else
        {
            lg2::error(
                "Component Compatibility Response Code: {ERRORCODE} not found for command: {COMMANDTYPE}",
                "ERRORCODE", errorCode, "COMMANDTYPE", (unsigned)commandType);
        }
    }
    else
    {
        lg2::error(
            "No component compatibility response code mapping found for command: {COMMANDTYPE}",
            "COMMANDTYPE", (unsigned)commandType);
    }
    return {status, messageId, messageError, resolution};
}

} // namespace fw_update
} // namespace pldm