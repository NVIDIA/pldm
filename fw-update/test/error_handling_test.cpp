#include "libpldm/firmware_update.h"
#include "fw-update/error_handling.hpp"

#include <gtest/gtest.h>

using namespace pldm;
using namespace pldm::fw_update;

TEST(error_handling, getOemMessage_request_update)
{
    const std::string oemMessageId = "ResourceEvent.1.0.ResourceErrorsDetected";
    const std::string oemMessageError = "Initiating firmware update timed out";
    const std::string oemResolution = "Retry firmware update operation";
    const bool oemMessageStatus = true;

    auto [outmessageStatus, outoemMessageId, outoemMessageError, outoemResolution] =
                getOemMessage(PLDM_REQUEST_UPDATE, COMMAND_TIMEOUT);

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

    auto [outmessageStatus, outoemMessageId, outoemMessageError, outoemResolution] =
                getOemMessage(PLDM_GET_FIRMWARE_PARAMETERS, COMMAND_TIMEOUT);

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

    auto [outmessageStatus, outoemMessageId, outoemMessageError, outoemResolution] =
                getOemMessage(PLDM_REQUEST_UPDATE, 0xFF);

    EXPECT_EQ(outmessageStatus, oemMessageStatus);
    EXPECT_EQ(outoemMessageId, oemMessageId);
    EXPECT_EQ(outoemMessageError, oemMessageError);
    EXPECT_EQ(outoemResolution, oemResolution);
}

TEST(error_handling, getCompCompatibilityMessage_update_component_identical_comp_stamp)
{
    const std::string oemMessageId = "OpenBMC.0.4.ComponentUpdateSkipped";
    const std::string oemMessageError = "Component image is identical";
    const std::string oemResolution = "Retry firmware update operation with the force flag";
    const bool oemMessageStatus = true;

    auto [outmessageStatus, outoemMessageId, outoemMessageError, outoemResolution] =
                getCompCompatibilityMessage(PLDM_UPDATE_COMPONENT, PLDM_CRC_COMP_COMPARISON_STAMP_IDENTICAL);

    EXPECT_EQ(outmessageStatus, oemMessageStatus);
    EXPECT_EQ(outoemMessageId, oemMessageId);
    EXPECT_EQ(outoemMessageError, oemMessageError);
    EXPECT_EQ(outoemResolution, oemResolution);
}

TEST(error_handling, getCompCompatibilityMessage_update_component_lower_comp_stamp)
{
    const std::string oemMessageId = "ResourceEvent.1.0.ResourceErrorsDetected";
    const std::string oemMessageError = "Component comparison stamp is lower than the firmware component comparison stamp in the FD";
    const std::string oemResolution = "Retry firmware update operation with the force flag";
    const bool oemMessageStatus = true;

    auto [outmessageStatus, outoemMessageId, outoemMessageError, outoemResolution] =
                getCompCompatibilityMessage(PLDM_UPDATE_COMPONENT, PLDM_CRC_COMP_COMPARISON_STAMP_LOWER);

    EXPECT_EQ(outmessageStatus, oemMessageStatus);
    EXPECT_EQ(outoemMessageId, oemMessageId);
    EXPECT_EQ(outoemMessageError, oemMessageError);
    EXPECT_EQ(outoemResolution, oemResolution);
}