// Override pldm_instance_db_init_default via --wrap linker flag
#include <libpldm/instance-id.h>
#include <unistd.h>

#include <deque>
#include <filesystem>
#include <fstream>
#include <string>

extern "C" int __wrap_pldm_instance_db_init_default(
    struct pldm_instance_db** ctx)
{
    static uint64_t dbIndex = 0;
    static std::deque<std::string> dbPaths;
    std::filesystem::create_directories("/tmp/claude");
    dbPaths.emplace_back(
        "/tmp/claude/pldm_test_iid_" + std::to_string(::getpid()) + "_" +
        std::to_string(dbIndex++));
    auto& dbPath = dbPaths.back();
    std::ofstream ofs(dbPath, std::ios::binary | std::ios::trunc);
    std::string data(256 * 32, '\0');
    ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    return pldm_instance_db_init(ctx, dbPath.c_str());
}

// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../pldm_cmd_helper.cpp"
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../pldm_fw_update_cmd.cpp"
#undef private
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <libpldm/firmware_update.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldmtool::helper;
using namespace pldmtool::fw_update;

namespace
{

void parseArgs(CLI::App& app, const std::vector<std::string>& args)
{
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (const auto& arg : args)
    {
        argv.push_back(arg.c_str());
    }

    try
    {
        app.parse(static_cast<int>(argv.size()),
                  const_cast<char**>(argv.data()));
    }
    catch (...)
    {}
}

template <typename T>
T readPayloadStruct(const pldm_msg* msg)
{
    T value{};
    memcpy(&value, msg->payload, sizeof(T));
    return value;
}

std::vector<uint8_t> makePassComponentTableResponse(
    uint8_t completionCode, uint8_t compResp, uint8_t compRespCode)
{
    std::vector<uint8_t> responseData(
        sizeof(pldm_msg_hdr) + sizeof(pldm_pass_component_table_resp), 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    auto response =
        pldm_pass_component_table_resp{completionCode, compResp, compRespCode};
    memcpy(resp->payload, &response, sizeof(response));
    return responseData;
}

std::vector<uint8_t> makeQueryDeviceIdentifiersResponse(
    uint8_t descriptorCount, const pldm_descriptor* descriptors)
{
    size_t descriptorsLen = 0;
    for (uint8_t index = 0; index < descriptorCount; ++index)
    {
        descriptorsLen += sizeof(uint16_t) + sizeof(uint16_t) +
                          descriptors[index].descriptor_length;
    }

    std::vector<uint8_t> responseData(
        sizeof(pldm_msg_hdr) + sizeof(pldm_query_device_identifiers_resp) +
            descriptorsLen,
        0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    auto header = pldm_query_device_identifiers_resp{
        PLDM_SUCCESS, static_cast<uint32_t>(descriptorsLen), descriptorCount};
    memcpy(resp->payload, &header, sizeof(header));

    auto* cursor = resp->payload + sizeof(header);
    for (uint8_t index = 0; index < descriptorCount; ++index)
    {
        memcpy(cursor, &descriptors[index].descriptor_type,
               sizeof(descriptors[index].descriptor_type));
        cursor += sizeof(descriptors[index].descriptor_type);
        memcpy(cursor, &descriptors[index].descriptor_length,
               sizeof(descriptors[index].descriptor_length));
        cursor += sizeof(descriptors[index].descriptor_length);
        memcpy(cursor, descriptors[index].descriptor_data,
               descriptors[index].descriptor_length);
        cursor += descriptors[index].descriptor_length;
    }

    return responseData;
}

} // namespace

// ===== convertStringTypeToUInt8 Tests =====

TEST(ConvertStringTypeToUInt8, KnownStrings)
{
    EXPECT_EQ(convertStringTypeToUInt8("UNKNOWN"), PLDM_STR_TYPE_UNKNOWN);
    EXPECT_EQ(convertStringTypeToUInt8("ASCII"), PLDM_STR_TYPE_ASCII);
    EXPECT_EQ(convertStringTypeToUInt8("UTF_8"), PLDM_STR_TYPE_UTF_8);
    EXPECT_EQ(convertStringTypeToUInt8("UTF_16"), PLDM_STR_TYPE_UTF_16);
    EXPECT_EQ(convertStringTypeToUInt8("UTF_16LE"), PLDM_STR_TYPE_UTF_16LE);
    EXPECT_EQ(convertStringTypeToUInt8("UTF_16BE"), PLDM_STR_TYPE_UTF_16BE);
}

TEST(ConvertStringTypeToUInt8, NumericString)
{
    EXPECT_EQ(convertStringTypeToUInt8("0"), 0);
    EXPECT_EQ(convertStringTypeToUInt8("1"), 1);
    EXPECT_EQ(convertStringTypeToUInt8("255"), 255);
    EXPECT_EQ(convertStringTypeToUInt8("42"), 42);
}

// ===== GetStatus Tests =====

TEST(GetStatus, CreateRequestMsg)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetStatus cmd("fw_update", "GetStatus", sub);

    auto [rc, requestMsg] = cmd.createRequestMsg();
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(requestMsg.size(),
              sizeof(pldm_msg_hdr) + PLDM_GET_STATUS_REQ_BYTES);
}

TEST(GetStatus, ParseResponseMsgSuccess)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetStatus cmd("fw_update", "GetStatus", sub);

    // Build response
    size_t payloadLen = sizeof(struct pldm_get_status_resp);
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto resp = reinterpret_cast<pldm_msg*>(responseData.data());

    // Manually fill the response
    struct pldm_get_status_resp* statusResp =
        reinterpret_cast<struct pldm_get_status_resp*>(resp->payload);
    statusResp->completion_code = PLDM_SUCCESS;
    statusResp->current_state = PLDM_FD_STATE_IDLE;
    statusResp->previous_state = PLDM_FD_STATE_IDLE;
    statusResp->aux_state = PLDM_FD_IDLE_LEARN_COMPONENTS_READ_XFER;
    statusResp->aux_state_status = PLDM_FD_AUX_STATE_IN_PROGRESS_OR_SUCCESS;
    statusResp->progress_percent = 0;
    statusResp->reason_code = PLDM_FD_INITIALIZATION;
    statusResp->update_option_flags_enabled.value = 0;

    testing::internal::CaptureStdout();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStdout();

    EXPECT_NE(output.find("SUCCESS"), std::string::npos);
    EXPECT_NE(output.find("IDLE"), std::string::npos);
}

TEST(GetStatus, ParseResponseMsgDecodeError)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetStatus cmd("fw_update", "GetStatus", sub);

    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + 1, 0);
    auto resp = reinterpret_cast<pldm_msg*>(responseData.data());

    testing::internal::CaptureStderr();
    cmd.parseResponseMsg(resp, 1);
    std::string errOutput = testing::internal::GetCapturedStderr();

    EXPECT_NE(errOutput.find("Response Message Error"), std::string::npos);
}

// ===== GetFwParams Tests =====

TEST(GetFwParams, CreateRequestMsg)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetFwParams cmd("fw_update", "GetFwParams", sub);

    auto [rc, requestMsg] = cmd.createRequestMsg();
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(requestMsg.size(),
              sizeof(pldm_msg_hdr) + PLDM_GET_FIRMWARE_PARAMETERS_REQ_BYTES);
}

// ===== QueryDeviceIdentifiers Tests =====

TEST(QueryDeviceIdentifiers, CreateRequestMsg)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    QueryDeviceIdentifiers cmd("fw_update", "QueryDeviceIdentifiers", sub);

    auto [rc, requestMsg] = cmd.createRequestMsg();
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(requestMsg.size(),
              sizeof(pldm_msg_hdr) + PLDM_QUERY_DEVICE_IDENTIFIERS_REQ_BYTES);
}

// ===== CancelUpdateComponent Tests =====

TEST(CancelUpdateComponent, CreateRequestMsg)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    CancelUpdateComponent cmd("fw_update", "CancelUpdateComponent", sub);

    auto [rc, requestMsg] = cmd.createRequestMsg();
    EXPECT_EQ(rc, PLDM_SUCCESS);
}

TEST(CancelUpdateComponent, ParseResponseMsgSuccess)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    CancelUpdateComponent cmd("fw_update", "CancelUpdateComponent", sub);

    size_t payloadLen = sizeof(uint8_t); // just completion code
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto resp = reinterpret_cast<pldm_msg*>(responseData.data());
    resp->payload[0] = PLDM_SUCCESS;

    testing::internal::CaptureStdout();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStdout();

    EXPECT_NE(output.find("SUCCESS"), std::string::npos);
}

// ===== CancelUpdate Tests =====

TEST(CancelUpdate, CreateRequestMsg)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    CancelUpdate cmd("fw_update", "CancelUpdate", sub);

    auto [rc, requestMsg] = cmd.createRequestMsg();
    EXPECT_EQ(rc, PLDM_SUCCESS);
}

TEST(CancelUpdate, ParseResponseMsgSuccess)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    CancelUpdate cmd("fw_update", "CancelUpdate", sub);

    // CancelUpdate response: cc + nonFunctioningComponentIndication +
    // nonFunctioningComponentBitmap
    size_t payloadLen = sizeof(uint8_t) + sizeof(bool8_t) + sizeof(uint64_t);
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto resp = reinterpret_cast<pldm_msg*>(responseData.data());

    // Fill response manually
    resp->payload[0] = PLDM_SUCCESS;
    resp->payload[1] = 0; // nonFunctioningComponentIndication = false
    // bitmap is 0

    testing::internal::CaptureStdout();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStdout();

    EXPECT_NE(output.find("SUCCESS"), std::string::npos);
    EXPECT_NE(output.find("False"), std::string::npos);
}

TEST(CancelUpdate, ParseResponseMsgWithNonFunctioning)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    CancelUpdate cmd("fw_update", "CancelUpdate", sub);

    size_t payloadLen = sizeof(uint8_t) + sizeof(bool8_t) + sizeof(uint64_t);
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto resp = reinterpret_cast<pldm_msg*>(responseData.data());

    resp->payload[0] = PLDM_SUCCESS;
    resp->payload[1] = 1; // nonFunctioningComponentIndication = true
    // Set bitmap to some value
    uint64_t bitmap = 0x03;
    memcpy(&resp->payload[2], &bitmap, sizeof(bitmap));

    testing::internal::CaptureStdout();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStdout();

    EXPECT_NE(output.find("SUCCESS"), std::string::npos);
    EXPECT_NE(output.find("True"), std::string::npos);
}

// ===== ActivateFirmware Tests =====

TEST(ActivateFirmware, ParseResponseMsgSuccess)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    ActivateFirmware cmd("fw_update", "ActivateFirmware", sub);

    // Parse CLI args
    std::vector<std::string> args = {
        "test", "--self_contained_activation_request", "False"};
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (auto& a : args)
        argv.push_back(a.c_str());
    try
    {
        app.parse(static_cast<int>(argv.size()),
                  const_cast<char**>(argv.data()));
    }
    catch (...)
    {}

    // Build response: cc + estimatedTimeForActivation (uint16_t)
    size_t payloadLen = sizeof(uint8_t) + sizeof(uint16_t);
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto resp = reinterpret_cast<pldm_msg*>(responseData.data());
    resp->payload[0] = PLDM_SUCCESS;
    uint16_t time = 10;
    memcpy(&resp->payload[1], &time, sizeof(time));

    testing::internal::CaptureStdout();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStdout();

    EXPECT_NE(output.find("SUCCESS"), std::string::npos);
    EXPECT_NE(output.find("10s"), std::string::npos);
}

// ===== RequestUpdate Tests =====

TEST(RequestUpdate, ParseResponseMsgSuccess)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    RequestUpdate cmd("fw_update", "RequestUpdate", sub);

    // Build response: cc + fdMetaDataLen (uint16_t) + fdWillSendPkgData
    // (uint8_t)
    size_t payloadLen = sizeof(struct pldm_request_update_resp);
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto resp = reinterpret_cast<pldm_msg*>(responseData.data());

    struct pldm_request_update_resp* updateResp =
        reinterpret_cast<struct pldm_request_update_resp*>(resp->payload);
    updateResp->completion_code = PLDM_SUCCESS;
    updateResp->fd_meta_data_len = 0;
    updateResp->fd_will_send_pkg_data = 0;

    testing::internal::CaptureStdout();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStdout();

    EXPECT_NE(output.find("SUCCESS"), std::string::npos);
}

// ===== PassComponentTable Tests =====

TEST(PassComponentTable, ParseResponseMsgSuccess)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    PassComponentTable cmd("fw_update", "PassComponentTable", sub);

    // Build response
    size_t payloadLen =
        sizeof(uint8_t) * 3; // cc + compResponse + compResponseCode
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto resp = reinterpret_cast<pldm_msg*>(responseData.data());
    resp->payload[0] = PLDM_SUCCESS;
    resp->payload[1] = 0; // Component can be updated
    resp->payload[2] = 0; // Component response code

    testing::internal::CaptureStdout();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStdout();

    EXPECT_NE(output.find("SUCCESS"), std::string::npos);
    EXPECT_NE(output.find("Component can be updated"), std::string::npos);
}

// ===== UpdateComponent Tests =====

TEST(UpdateComponent, ParseResponseMsgSuccess)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    UpdateComponent cmd("fw_update", "UpdateComponent", sub);

    // Build response
    size_t payloadLen = sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint8_t) +
                        sizeof(uint32_t) + sizeof(uint16_t);
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto resp = reinterpret_cast<pldm_msg*>(responseData.data());
    resp->payload[0] = PLDM_SUCCESS;
    resp->payload[1] = 0; // Component can be updated
    resp->payload[2] = 0; // Component compatibility response code

    testing::internal::CaptureStdout();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStdout();

    EXPECT_NE(output.find("SUCCESS"), std::string::npos);
}

// ===== Map Existence Tests =====

TEST(FwUpdateMaps, FdStateMachineHasEntries)
{
    EXPECT_FALSE(fdStateMachine.empty());
    EXPECT_EQ(fdStateMachine.at(PLDM_FD_STATE_IDLE), "IDLE");
    EXPECT_EQ(fdStateMachine.at(PLDM_FD_STATE_DOWNLOAD), "DOWNLOAD");
    EXPECT_EQ(fdStateMachine.at(PLDM_FD_STATE_VERIFY), "VERIFY");
    EXPECT_EQ(fdStateMachine.at(PLDM_FD_STATE_APPLY), "APPLY");
    EXPECT_EQ(fdStateMachine.at(PLDM_FD_STATE_ACTIVATE), "ACTIVATE");
}

TEST(FwUpdateMaps, FdAuxStateHasEntries)
{
    EXPECT_FALSE(fdAuxState.empty());
    EXPECT_NE(fdAuxState.find(PLDM_FD_OPERATION_IN_PROGRESS), fdAuxState.end());
    EXPECT_NE(fdAuxState.find(PLDM_FD_OPERATION_SUCCESSFUL), fdAuxState.end());
}

TEST(FwUpdateMaps, FdAuxStateStatusHasEntries)
{
    EXPECT_FALSE(fdAuxStateStatus.empty());
    EXPECT_NE(fdAuxStateStatus.find(PLDM_FD_AUX_STATE_IN_PROGRESS_OR_SUCCESS),
              fdAuxStateStatus.end());
}

TEST(FwUpdateMaps, FdReasonCodeHasEntries)
{
    EXPECT_FALSE(fdReasonCode.empty());
    EXPECT_NE(fdReasonCode.find(PLDM_FD_INITIALIZATION), fdReasonCode.end());
}

TEST(FwUpdateMaps, DescriptorNameHasEntries)
{
    EXPECT_FALSE(descriptorName.empty());
    EXPECT_NE(descriptorName.find(PLDM_FWUP_PCI_VENDOR_ID),
              descriptorName.end());
    EXPECT_NE(descriptorName.find(PLDM_FWUP_UUID), descriptorName.end());
}

TEST(FwUpdateMaps, ComponentClassificationHasEntries)
{
    EXPECT_FALSE(componentClassification.empty());
    EXPECT_EQ(componentClassification.at(PLDM_COMP_FIRMWARE), "Firmware");
}

// ===== registerCommand Tests =====

TEST(FwUpdateRegisterCommand, RegistersSubcommands)
{
    CLI::App app{"test"};
    pldmtool::fw_update::registerCommand(app);

    auto fwUpdate = app.get_subcommand("fw_update");
    EXPECT_NE(fwUpdate, nullptr);

    EXPECT_NE(fwUpdate->get_subcommand("GetStatus"), nullptr);
    EXPECT_NE(fwUpdate->get_subcommand("GetFwParams"), nullptr);
    EXPECT_NE(fwUpdate->get_subcommand("QueryDeviceIdentifiers"), nullptr);
    EXPECT_NE(fwUpdate->get_subcommand("RequestUpdate"), nullptr);
    EXPECT_NE(fwUpdate->get_subcommand("PassComponentTable"), nullptr);
    EXPECT_NE(fwUpdate->get_subcommand("UpdateComponent"), nullptr);
    EXPECT_NE(fwUpdate->get_subcommand("ActivateFirmware"), nullptr);
    EXPECT_NE(fwUpdate->get_subcommand("CancelUpdateComponent"), nullptr);
    EXPECT_NE(fwUpdate->get_subcommand("CancelUpdate"), nullptr);
}

TEST(GetFwParams, ParseResponseMsgSuccess)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetFwParams cmd("fw_update", "GetFwParams", sub);

    constexpr uint8_t activeVerLen = 1;
    constexpr uint8_t pendingVerLen = 1;
    size_t payloadLen = sizeof(pldm_get_firmware_parameters_resp) +
                        activeVerLen + pendingVerLen;
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());

    auto* fwParams =
        reinterpret_cast<pldm_get_firmware_parameters_resp*>(resp->payload);
    fwParams->completion_code = PLDM_SUCCESS;
    fwParams->capabilities_during_update.value = 0x1F;
    fwParams->comp_count = 0;
    fwParams->active_comp_image_set_ver_str_type = PLDM_STR_TYPE_ASCII;
    fwParams->active_comp_image_set_ver_str_len = activeVerLen;
    fwParams->pending_comp_image_set_ver_str_type = PLDM_STR_TYPE_ASCII;
    fwParams->pending_comp_image_set_ver_str_len = pendingVerLen;

    auto* payload = resp->payload + sizeof(pldm_get_firmware_parameters_resp);
    payload[0] = 'A';
    payload[1] = 'B';

    testing::internal::CaptureStdout();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStdout();

    EXPECT_NE(output.find("SUCCESS"), std::string::npos);
    EXPECT_NE(output.find("CapabilitiesDuringUpdate"), std::string::npos);
}

TEST(QueryDeviceIdentifiers, ParseResponseMsgSuccess)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    QueryDeviceIdentifiers cmd("fw_update", "QueryDeviceIdentifiers", sub);

    std::array<uint8_t, 2> pciVendorId{0x34, 0x12};
    std::array<uint8_t, 5> vendorDefined{PLDM_STR_TYPE_ASCII, 1, 'V', 0xAA,
                                         0xBB};
    pldm_descriptor descriptors[] = {
        {.descriptor_type = PLDM_FWUP_PCI_VENDOR_ID,
         .descriptor_length = static_cast<uint16_t>(pciVendorId.size()),
         .descriptor_data = pciVendorId.data()},
        {.descriptor_type = PLDM_FWUP_VENDOR_DEFINED,
         .descriptor_length = static_cast<uint16_t>(vendorDefined.size()),
         .descriptor_data = vendorDefined.data()}};

    auto responseData = makeQueryDeviceIdentifiersResponse(2, descriptors);
    size_t payloadLen = responseData.size() - sizeof(pldm_msg_hdr);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());

    testing::internal::CaptureStdout();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStdout();

    EXPECT_NE(output.find("SUCCESS"), std::string::npos);
    EXPECT_NE(output.find("PCI Vendor ID"), std::string::npos);
    EXPECT_NE(output.find("\"V\""), std::string::npos);
}

TEST(RequestUpdate, CreateRequestMsgSuccess)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    RequestUpdate cmd("fw_update", "RequestUpdate", sub);

    parseArgs(app,
              {"test", "test", "--max_transfer_size", "1024", "--num_comps",
               "2", "--max_transfer_reqs", "1", "--package_data_length", "10",
               "--comp_img_ver_str_type", "5", "--comp_img_ver_str_len", "3",
               "--comp_img_set_ver_str", "abc"});

    auto [rc, requestMsg] = cmd.createRequestMsg();
    ASSERT_EQ(rc, PLDM_SUCCESS);

    auto* req = reinterpret_cast<pldm_msg*>(requestMsg.data());
    auto decoded = readPayloadStruct<pldm_request_update_req>(req);
    EXPECT_EQ(decoded.max_transfer_size, 1024u);
    EXPECT_EQ(decoded.num_of_comp, 2);
    EXPECT_EQ(decoded.max_outstanding_transfer_req, 1);
    EXPECT_EQ(decoded.pkg_data_len, 10);
    EXPECT_EQ(decoded.comp_image_set_ver_str_type, 5);
    EXPECT_EQ(decoded.comp_image_set_ver_str_len, 3);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(
                              req->payload + sizeof(pldm_request_update_req)),
                          decoded.comp_image_set_ver_str_len),
              "abc");
}

TEST(PassComponentTable, CreateRequestMsgSuccess)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    PassComponentTable cmd("fw_update", "PassComponentTable", sub);

    parseArgs(app, {"test", "test", "--transfer_flag", "START",
                    "--comp_classification", "10", "--comp_identifier", "20",
                    "--comp_classification_idx", "1", "--comp_compare_stamp",
                    "100", "--comp_ver_str_type", "5", "--comp_ver_str_len",
                    "3", "--comp_ver_str", "v1a"});

    auto [rc, requestMsg] = cmd.createRequestMsg();
    ASSERT_EQ(rc, PLDM_SUCCESS);

    auto* req = reinterpret_cast<pldm_msg*>(requestMsg.data());
    auto decoded = readPayloadStruct<pldm_pass_component_table_req>(req);
    EXPECT_EQ(decoded.transfer_flag, PLDM_START);
    EXPECT_EQ(decoded.comp_classification, 10);
    EXPECT_EQ(decoded.comp_identifier, 20);
    EXPECT_EQ(decoded.comp_classification_index, 1);
    EXPECT_EQ(decoded.comp_comparison_stamp, 100u);
    EXPECT_EQ(decoded.comp_ver_str_type, 5);
    EXPECT_EQ(decoded.comp_ver_str_len, 3);
    EXPECT_EQ(
        std::string(reinterpret_cast<const char*>(
                        req->payload + sizeof(pldm_pass_component_table_req)),
                    decoded.comp_ver_str_len),
        "v1a");
}

TEST(UpdateComponent, CreateRequestMsgSuccess)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    UpdateComponent cmd("fw_update", "UpdateComponent", sub);

    parseArgs(
        app,
        {"test",
         "test",
         "--component_classification",
         "10",
         "--component_identifier",
         "20",
         "--component_classification_index",
         "1",
         "--component_comparison_stamp",
         "5",
         "--component_image_size",
         "256",
         "--update_option_flags",
         "0x3",
         "--component_version_string_type",
         "5",
         "--component_version_string_length",
         "3",
         "--component_version_string",
         "abc"});

    auto [rc, requestMsg] = cmd.createRequestMsg();
    ASSERT_EQ(rc, PLDM_SUCCESS);

    auto* req = reinterpret_cast<pldm_msg*>(requestMsg.data());
    auto decoded = readPayloadStruct<pldm_update_component_req>(req);
    EXPECT_EQ(decoded.comp_classification, 10);
    EXPECT_EQ(decoded.comp_identifier, 20);
    EXPECT_EQ(decoded.comp_classification_index, 1);
    EXPECT_EQ(decoded.comp_comparison_stamp, 5u);
    EXPECT_EQ(decoded.comp_image_size, 256u);
    EXPECT_EQ(decoded.update_option_flags.value, 0x3u);
    EXPECT_EQ(decoded.comp_ver_str_type, 5);
    EXPECT_EQ(decoded.comp_ver_str_len, 3);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(
                              req->payload + sizeof(pldm_update_component_req)),
                          decoded.comp_ver_str_len),
              "abc");
}

TEST(UpdateComponent, CreateRequestMsgInvalidFlags)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    UpdateComponent cmd("fw_update", "UpdateComponent", sub);

    parseArgs(app, {"test", "--component_classification", "10",
                    "--component_identifier", "20",
                    "--component_classification_index", "1",
                    "--component_comparison_stamp", "5",
                    "--component_image_size", "256", "--update_option_flags",
                    "bad-value", "--component_version_string_type", "ASCII",
                    "--component_version_string_length", "3",
                    "--component_version_string", "abc"});

    auto [rc, requestMsg] = cmd.createRequestMsg();
    EXPECT_EQ(rc, PLDM_ERROR_INVALID_DATA);
    EXPECT_TRUE(requestMsg.empty());
}

TEST(ActivateFirmware, CreateRequestMsgSuccess)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    ActivateFirmware cmd("fw_update", "ActivateFirmware", sub);

    parseArgs(app,
              {"test", "test", "--self_contained_activation_request", "True"});

    auto [rc, requestMsg] = cmd.createRequestMsg();
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(requestMsg.size(),
              sizeof(pldm_msg_hdr) + sizeof(pldm_activate_firmware_req));
}

TEST(GetStatus, ParseResponseMsgSuccessVendorDefinedFields)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetStatus cmd("fw_update", "GetStatus", sub);

    size_t payloadLen = sizeof(pldm_get_status_resp);
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    auto* statusResp = reinterpret_cast<pldm_get_status_resp*>(resp->payload);
    statusResp->completion_code = PLDM_SUCCESS;
    statusResp->current_state = PLDM_FD_STATE_DOWNLOAD;
    statusResp->previous_state = PLDM_FD_STATE_VERIFY;
    statusResp->aux_state = PLDM_FD_OPERATION_FAILED;
    statusResp->aux_state_status = PLDM_FD_VENDOR_DEFINED_STATUS_CODE_START;
    statusResp->progress_percent = 55;
    statusResp->reason_code = PLDM_FD_STATUS_VENDOR_DEFINED_MIN;
    statusResp->update_option_flags_enabled.value = 0x5;

    testing::internal::CaptureStdout();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("DOWNLOAD"), std::string::npos);
    EXPECT_NE(output.find("VERIFY"), std::string::npos);
    EXPECT_NE(output.find("\"AuxStateStatus\": 112"), std::string::npos);
    EXPECT_NE(output.find("\"ReasonCode\": 200"), std::string::npos);
}

TEST(GetStatus, ParseResponseMsgCompletionCodeFailure)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetStatus cmd("fw_update", "GetStatus", sub);

    size_t payloadLen = sizeof(pldm_get_status_resp);
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    auto* statusResp = reinterpret_cast<pldm_get_status_resp*>(resp->payload);
    statusResp->completion_code = PLDM_FWUP_INCOMPLETE_UPDATE;

    testing::internal::CaptureStdout();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("INCOMPLETE_UPDATE"), std::string::npos);
    EXPECT_EQ(output.find("CurrentState"), std::string::npos);
}

TEST(GetFwParams, ParseResponseMsgDecodeError)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetFwParams cmd("fw_update", "GetFwParams", sub);

    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + 1, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());

    testing::internal::CaptureStderr();
    cmd.parseResponseMsg(resp, 1);
    std::string output = testing::internal::GetCapturedStderr();
    EXPECT_NE(output.find("Response Message Error"), std::string::npos);
}

TEST(GetFwParams, ParseResponseMsgComponentEntryCoversElsePaths)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetFwParams cmd("fw_update", "GetFwParams", sub);

    constexpr uint8_t setVerLen = 1;
    constexpr uint8_t activeCompVerLen = 3;
    constexpr uint8_t pendingCompVerLen = 2;
    const size_t payloadLen =
        sizeof(pldm_get_firmware_parameters_resp) + setVerLen + setVerLen +
        sizeof(pldm_component_parameter_entry) + activeCompVerLen +
        pendingCompVerLen;
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());

    auto* fwParams =
        reinterpret_cast<pldm_get_firmware_parameters_resp*>(resp->payload);
    fwParams->completion_code = PLDM_SUCCESS;
    fwParams->capabilities_during_update.value = 0;
    fwParams->comp_count = 1;
    fwParams->active_comp_image_set_ver_str_type = PLDM_STR_TYPE_ASCII;
    fwParams->active_comp_image_set_ver_str_len = setVerLen;
    fwParams->pending_comp_image_set_ver_str_type = PLDM_STR_TYPE_ASCII;
    fwParams->pending_comp_image_set_ver_str_len = setVerLen;

    auto* cursor = resp->payload + sizeof(pldm_get_firmware_parameters_resp);
    cursor[0] = 'A';
    cursor[1] = 'B';
    cursor += 2;

    pldm_component_parameter_entry compEntry{};
    compEntry.comp_classification = 0x1234;
    compEntry.comp_identifier = 9;
    compEntry.comp_classification_index = 2;
    compEntry.active_comp_comparison_stamp = 0x11;
    memcpy(compEntry.active_comp_release_date, "20240101", 8);
    compEntry.pending_comp_comparison_stamp = 0x22;
    memcpy(compEntry.pending_comp_release_date, "20240202", 8);
    compEntry.comp_activation_methods.value = 0x3F;
    compEntry.capabilities_during_update.value = 0;
    compEntry.active_comp_ver_str_type = PLDM_STR_TYPE_ASCII;
    compEntry.active_comp_ver_str_len = activeCompVerLen;
    compEntry.pending_comp_ver_str_type = PLDM_STR_TYPE_ASCII;
    compEntry.pending_comp_ver_str_len = pendingCompVerLen;
    memcpy(cursor, &compEntry, sizeof(compEntry));
    cursor += sizeof(compEntry);
    memcpy(cursor, "abc", activeCompVerLen);
    cursor += activeCompVerLen;
    memcpy(cursor, "de", pendingCompVerLen);

    testing::internal::CaptureStdout();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("\"ComponentClassification\": 4660"),
              std::string::npos);
    EXPECT_NE(output.find("20240101"), std::string::npos);
    EXPECT_NE(output.find("20240202"), std::string::npos);
    EXPECT_NE(output.find("System reboot"), std::string::npos);
}

TEST(QueryDeviceIdentifiers, ParseResponseMsgDecodeError)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    QueryDeviceIdentifiers cmd("fw_update", "QueryDeviceIdentifiers", sub);

    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + 1, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());

    testing::internal::CaptureStderr();
    cmd.parseResponseMsg(resp, 1);
    std::string output = testing::internal::GetCapturedStderr();
    EXPECT_NE(output.find("Decoding QueryDeviceIdentifiers response failed"),
              std::string::npos);
}

TEST(QueryDeviceIdentifiers, ParseResponseMsgCompletionCodeError)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    QueryDeviceIdentifiers cmd("fw_update", "QueryDeviceIdentifiers", sub);

    const size_t payloadLen = 1 + sizeof(uint32_t) + 1;
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    resp->payload[0] = PLDM_ERROR;

    testing::internal::CaptureStderr();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStderr();
    EXPECT_NE(output.find("completion code"), std::string::npos);
}

TEST(QueryDeviceIdentifiers, ParseResponseMsgVendorDefinedDecodeError)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    QueryDeviceIdentifiers cmd("fw_update", "QueryDeviceIdentifiers", sub);

    std::array<uint8_t, 1> badVendorDefined{0xFF};
    pldm_descriptor descriptors[] = {
        {.descriptor_type = PLDM_FWUP_VENDOR_DEFINED,
         .descriptor_length = static_cast<uint16_t>(badVendorDefined.size()),
         .descriptor_data = badVendorDefined.data()}};

    auto responseData = makeQueryDeviceIdentifiersResponse(1, descriptors);
    size_t payloadLen = responseData.size() - sizeof(pldm_msg_hdr);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());

    testing::internal::CaptureStderr();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStderr();
    EXPECT_NE(output.find("Decoding Vendor-defined descriptor valuefailed"),
              std::string::npos);
}

TEST(RequestUpdate, ParseResponseMsgDecodeError)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    RequestUpdate cmd("fw_update", "RequestUpdate", sub);

    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + 1, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());

    testing::internal::CaptureStderr();
    cmd.parseResponseMsg(resp, 1);
    std::string output = testing::internal::GetCapturedStderr();
    EXPECT_NE(output.find("Response Message Error"), std::string::npos);
}

TEST(RequestUpdate, ParseResponseMsgNonSuccessCompletion)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    RequestUpdate cmd("fw_update", "RequestUpdate", sub);

    size_t payloadLen = sizeof(pldm_request_update_resp);
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    auto* updateResp =
        reinterpret_cast<pldm_request_update_resp*>(resp->payload);
    updateResp->completion_code = PLDM_FWUP_BUSY_IN_BACKGROUND;

    testing::internal::CaptureStdout();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("BUSY_IN_BACKGROUND"), std::string::npos);
    EXPECT_EQ(output.find("FirmwareDeviceMetaDataLength"), std::string::npos);
}

TEST(RequestUpdate, ParseArgsInvalidStringType)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    RequestUpdate cmd("fw_update", "RequestUpdate", sub);

    parseArgs(app,
              {"test", "test", "--max_transfer_size", "1024", "--num_comps",
               "2", "--max_transfer_reqs", "1", "--package_data_length", "10",
               "--comp_img_ver_str_type", "INVALID", "--comp_img_ver_str_len",
               "3", "--comp_img_set_ver_str", "abc"});
}

TEST(PassComponentTable, ParseResponseMsgMayBeUpdateable)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    PassComponentTable cmd("fw_update", "PassComponentTable", sub);

    auto responseData = makePassComponentTableResponse(
        PLDM_SUCCESS, PLDM_CR_COMP_MAY_BE_UPDATEABLE,
        PLDM_CRC_COMP_CAN_BE_UPDATED);
    size_t payloadLen = responseData.size() - sizeof(pldm_msg_hdr);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());

    testing::internal::CaptureStdout();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("Component may be updateable"), std::string::npos);
    EXPECT_NE(output.find("0x00"), std::string::npos);
}

TEST(PassComponentTable, ParseResponseMsgDecodeError)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    PassComponentTable cmd("fw_update", "PassComponentTable", sub);

    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + 1, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    testing::internal::CaptureStderr();
    cmd.parseResponseMsg(resp, 1);
    std::string output = testing::internal::GetCapturedStderr();
    EXPECT_NE(output.find("Response Message Error"), std::string::npos);
}

TEST(PassComponentTable, ParseArgsInvalidStringType)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    PassComponentTable cmd("fw_update", "PassComponentTable", sub);

    parseArgs(app, {"test", "test", "--transfer_flag", "START",
                    "--comp_classification", "10", "--comp_identifier", "20",
                    "--comp_classification_idx", "1", "--comp_compare_stamp",
                    "100", "--comp_ver_str_type", "INVALID",
                    "--comp_ver_str_len", "3", "--comp_ver_str", "v1a"});
}

TEST(UpdateComponent, ParseResponseMsgComponentNotUpdateable)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    UpdateComponent cmd("fw_update", "UpdateComponent", sub);

    size_t payloadLen = sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint8_t) +
                        sizeof(uint32_t) + sizeof(uint16_t);
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    resp->payload[0] = PLDM_SUCCESS;
    resp->payload[1] = 1;
    resp->payload[2] = 0x8;

    testing::internal::CaptureStdout();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("Component will not be updated"), std::string::npos);
    EXPECT_NE(output.find("0x08"), std::string::npos);
}

TEST(UpdateComponent, ParseResponseMsgDecodeError)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    UpdateComponent cmd("fw_update", "UpdateComponent", sub);

    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + 1, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    testing::internal::CaptureStderr();
    cmd.parseResponseMsg(resp, 1);
    std::string output = testing::internal::GetCapturedStderr();
    EXPECT_NE(output.find("Parsing UpdateComponent response failed"),
              std::string::npos);
}

TEST(UpdateComponent, ParseArgsInvalidStringType)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    UpdateComponent cmd("fw_update", "UpdateComponent", sub);

    parseArgs(
        app,
        {"test",
         "test",
         "--component_classification",
         "10",
         "--component_identifier",
         "20",
         "--component_classification_index",
         "1",
         "--component_comparison_stamp",
         "5",
         "--component_image_size",
         "256",
         "--update_option_flags",
         "0x3",
         "--component_version_string_type",
         "INVALID",
         "--component_version_string_length",
         "3",
         "--component_version_string",
         "abc"});
}

TEST(ActivateFirmware, ParseResponseMsgDecodeError)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    ActivateFirmware cmd("fw_update", "ActivateFirmware", sub);

    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + 1, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    testing::internal::CaptureStderr();
    cmd.parseResponseMsg(resp, 1);
    std::string output = testing::internal::GetCapturedStderr();
    EXPECT_NE(output.find("Parsing ActivateFirmware response failed"),
              std::string::npos);
}

TEST(ActivateFirmware, ParseResponseMsgNonSuccessCompletion)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    ActivateFirmware cmd("fw_update", "ActivateFirmware", sub);

    const size_t payloadLen = sizeof(uint8_t) + sizeof(uint16_t);
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    resp->payload[0] = PLDM_FWUP_ACTIVATION_NOT_REQUIRED;

    testing::internal::CaptureStdout();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("ACTIVATION_NOT_REQUIRED"), std::string::npos);
    EXPECT_EQ(output.find("EstimatedTimeForSelfContainedActivation"),
              std::string::npos);
}

TEST(CancelUpdateComponent, ParseResponseMsgDecodeError)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    CancelUpdateComponent cmd("fw_update", "CancelUpdateComponent", sub);

    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr), 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    testing::internal::CaptureStderr();
    cmd.parseResponseMsg(resp, 0);
    std::string output = testing::internal::GetCapturedStderr();
    EXPECT_NE(output.find("Parsing CancelUpdateComponent response failed"),
              std::string::npos);
}

TEST(CancelUpdate, ParseResponseMsgCompletionCodeFailure)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    CancelUpdate cmd("fw_update", "CancelUpdate", sub);

    const size_t payloadLen =
        sizeof(uint8_t) + sizeof(bool8_t) + sizeof(uint64_t);
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    resp->payload[0] = PLDM_FWUP_CANCEL_PENDING;

    testing::internal::CaptureStdout();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("CANCEL_PENDING"), std::string::npos);
    EXPECT_EQ(output.find("NonFunctioningComponentIndication"),
              std::string::npos);
}

TEST(CancelUpdate, ParseResponseMsgDecodeError)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    CancelUpdate cmd("fw_update", "CancelUpdate", sub);

    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + 1, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    testing::internal::CaptureStderr();
    cmd.parseResponseMsg(resp, 1);
    std::string output = testing::internal::GetCapturedStderr();
    EXPECT_NE(output.find("Parsing CancelUpdate response failed"),
              std::string::npos);
}

TEST(RequestUpdate, ParseArgsStringTypeOutOfRange)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    RequestUpdate cmd("fw_update", "RequestUpdate", sub);

    parseArgs(app,
              {"test", "test", "--max_transfer_size", "1024", "--num_comps",
               "2", "--max_transfer_reqs", "1", "--package_data_length", "10",
               "--comp_img_ver_str_type", "256", "--comp_img_ver_str_len", "3",
               "--comp_img_set_ver_str", "abc"});
    SUCCEED();
}

TEST(PassComponentTable, ParseArgsStringTypeOutOfRange)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    PassComponentTable cmd("fw_update", "PassComponentTable", sub);

    parseArgs(app, {"test", "test", "--transfer_flag", "START",
                    "--comp_classification", "10", "--comp_identifier", "20",
                    "--comp_classification_idx", "1", "--comp_compare_stamp",
                    "100", "--comp_ver_str_type", "256", "--comp_ver_str_len",
                    "3", "--comp_ver_str", "v1a"});
    SUCCEED();
}

TEST(UpdateComponent, ParseArgsStringTypeOutOfRange)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    UpdateComponent cmd("fw_update", "UpdateComponent", sub);

    parseArgs(
        app,
        {"test",
         "test",
         "--component_classification",
         "10",
         "--component_identifier",
         "20",
         "--component_classification_index",
         "1",
         "--component_comparison_stamp",
         "5",
         "--component_image_size",
         "256",
         "--update_option_flags",
         "0x3",
         "--component_version_string_type",
         "256",
         "--component_version_string_length",
         "3",
         "--component_version_string",
         "abc"});
    SUCCEED();
}

TEST(GetFwParams, ParseResponseMsgComponentEntryKnownClassificationPaths)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetFwParams cmd("fw_update", "GetFwParams", sub);

    constexpr uint8_t setVerLen = 1;
    constexpr uint8_t activeCompVerLen = 3;
    constexpr uint8_t pendingCompVerLen = 2;
    const size_t payloadLen =
        sizeof(pldm_get_firmware_parameters_resp) + setVerLen + setVerLen +
        sizeof(pldm_component_parameter_entry) + activeCompVerLen +
        pendingCompVerLen;
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());

    auto* fwParams =
        reinterpret_cast<pldm_get_firmware_parameters_resp*>(resp->payload);
    fwParams->completion_code = PLDM_SUCCESS;
    fwParams->capabilities_during_update.value = 0x1;
    fwParams->comp_count = 1;
    fwParams->active_comp_image_set_ver_str_type = PLDM_STR_TYPE_ASCII;
    fwParams->active_comp_image_set_ver_str_len = setVerLen;
    fwParams->pending_comp_image_set_ver_str_type = PLDM_STR_TYPE_ASCII;
    fwParams->pending_comp_image_set_ver_str_len = setVerLen;

    auto* cursor = resp->payload + sizeof(pldm_get_firmware_parameters_resp);
    cursor[0] = 'A';
    cursor[1] = 'B';
    cursor += 2;

    pldm_component_parameter_entry compEntry{};
    compEntry.comp_classification = PLDM_COMP_FIRMWARE;
    compEntry.comp_identifier = 9;
    compEntry.comp_classification_index = 2;
    compEntry.active_comp_comparison_stamp = 0x11;
    compEntry.pending_comp_comparison_stamp = 0x22;
    compEntry.comp_activation_methods.value = 0;
    compEntry.capabilities_during_update.value = 0x1;
    compEntry.active_comp_ver_str_type = PLDM_STR_TYPE_ASCII;
    compEntry.active_comp_ver_str_len = activeCompVerLen;
    compEntry.pending_comp_ver_str_type = PLDM_STR_TYPE_ASCII;
    compEntry.pending_comp_ver_str_len = pendingCompVerLen;
    memcpy(cursor, &compEntry, sizeof(compEntry));
    cursor += sizeof(compEntry);
    memcpy(cursor, "abc", activeCompVerLen);
    cursor += activeCompVerLen;
    memcpy(cursor, "de", pendingCompVerLen);

    testing::internal::CaptureStdout();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("\"ComponentClassification\": \"Firmware\""),
              std::string::npos);
    EXPECT_NE(output.find("\"ActiveComponentReleaseDate\": \"\""),
              std::string::npos);
    EXPECT_NE(output.find("\"PendingComponentReleaseDate\": \"\""),
              std::string::npos);
    EXPECT_NE(output.find("Firmware Device performs an auto-apply"),
              std::string::npos);
}

TEST(QueryDeviceIdentifiers, ParseResponseMsgDuplicateDescriptorType)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    QueryDeviceIdentifiers cmd("fw_update", "QueryDeviceIdentifiers", sub);

    std::array<uint8_t, 2> vendorIdA{0x34, 0x12};
    std::array<uint8_t, 2> vendorIdB{0x78, 0x56};
    pldm_descriptor descriptors[] = {
        {.descriptor_type = PLDM_FWUP_PCI_VENDOR_ID,
         .descriptor_length = static_cast<uint16_t>(vendorIdA.size()),
         .descriptor_data = vendorIdA.data()},
        {.descriptor_type = PLDM_FWUP_PCI_VENDOR_ID,
         .descriptor_length = static_cast<uint16_t>(vendorIdB.size()),
         .descriptor_data = vendorIdB.data()}};

    auto responseData = makeQueryDeviceIdentifiersResponse(2, descriptors);
    size_t payloadLen = responseData.size() - sizeof(pldm_msg_hdr);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());

    testing::internal::CaptureStdout();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("PCI Vendor ID"), std::string::npos);
}

TEST(QueryDeviceIdentifiers, ParseResponseMsgUnknownDescriptorType)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    QueryDeviceIdentifiers cmd("fw_update", "QueryDeviceIdentifiers", sub);

    const size_t descriptorLen = 2 + 2 + 2; // type + length + data
    const size_t payloadLen = 1 + sizeof(uint32_t) + 1 + descriptorLen;
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    resp->payload[0] = PLDM_SUCCESS;
    const uint32_t deviceIdentifiersLen = descriptorLen;
    memcpy(resp->payload + 1, &deviceIdentifiersLen,
           sizeof(deviceIdentifiersLen));
    resp->payload[1 + sizeof(uint32_t)] = 1;

    auto* descriptor = resp->payload + 1 + sizeof(uint32_t) + 1;
    uint16_t descriptorType = 0xABCD;
    uint16_t descriptorDataLen = 2;
    memcpy(descriptor, &descriptorType, sizeof(descriptorType));
    memcpy(descriptor + sizeof(descriptorType), &descriptorDataLen,
           sizeof(descriptorDataLen));
    descriptor[sizeof(descriptorType) + sizeof(descriptorDataLen)] = 0xAA;
    descriptor[sizeof(descriptorType) + sizeof(descriptorDataLen) + 1] = 0xBB;

    EXPECT_NO_THROW(cmd.parseResponseMsg(resp, payloadLen));
}

TEST(QueryDeviceIdentifiers, ParseResponseMsgDescriptorDecodeError)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    QueryDeviceIdentifiers cmd("fw_update", "QueryDeviceIdentifiers", sub);

    const size_t payloadLen = 1 + sizeof(uint32_t) + 1;
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    resp->payload[0] = PLDM_SUCCESS;
    const uint32_t deviceIdentifiersLen = 1;
    memcpy(resp->payload + 1, &deviceIdentifiersLen,
           sizeof(deviceIdentifiersLen));
    resp->payload[1 + sizeof(uint32_t)] = 1;

    testing::internal::CaptureStderr();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStderr();
    EXPECT_NE(output.find("Decoding"), std::string::npos);
}

TEST(PassComponentTable, ParseResponseMsgCompletionCodeFailure)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    PassComponentTable cmd("fw_update", "PassComponentTable", sub);

    size_t payloadLen = sizeof(uint8_t) * 3;
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    resp->payload[0] = PLDM_ERROR;
    resp->payload[1] = 0;
    resp->payload[2] = 0;

    testing::internal::CaptureStdout();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("ERROR"), std::string::npos);
    EXPECT_EQ(output.find("ComponentResponse"), std::string::npos);
}

TEST(UpdateComponent, ParseResponseMsgCompletionCodeFailure)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    UpdateComponent cmd("fw_update", "UpdateComponent", sub);

    size_t payloadLen = sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint8_t) +
                        sizeof(uint32_t) + sizeof(uint16_t);
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    resp->payload[0] = PLDM_ERROR;
    resp->payload[1] = 0;
    resp->payload[2] = 0;

    testing::internal::CaptureStdout();
    cmd.parseResponseMsg(resp, payloadLen);
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("ERROR"), std::string::npos);
    EXPECT_EQ(output.find("ComponentCompatibilityResponse"), std::string::npos);
}
