// Override pldm_instance_db_init_default via --wrap linker flag
#include "test/test_tmp_utils.hpp"

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
    auto root = pldm::test::ensureTempDir();
    dbPaths.emplace_back(
        (root / ("pldm_test_iid_" + std::to_string(::getpid()) + "_" +
                 std::to_string(dbIndex++)))
            .string());
    auto& dbPath = dbPaths.back();
    std::ofstream ofs(dbPath, std::ios::binary | std::ios::trunc);
    std::string data(256 * 32, '\0');
    ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    return pldm_instance_db_init(ctx, dbPath.c_str());
}

#include <libpldm/base.h>
#include <libpldm/firmware_update.h>

// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../pldm_base_cmd.cpp"
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../pldm_cmd_helper.cpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldmtool::helper;
using namespace pldmtool::base;

// ===== GetPLDMTypes Tests =====

TEST(GetPLDMTypes, CreateRequestMsg)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetPLDMTypes cmd("base", "GetPLDMTypes", sub);

    auto [rc, requestMsg] = cmd.createRequestMsg();
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(requestMsg.size(), sizeof(pldm_msg_hdr));
}

TEST(GetPLDMTypes, ParseResponseMsgSuccess)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetPLDMTypes cmd("base", "GetPLDMTypes", sub);

    // Build response: cc=0, types bitfield with base(0) and platform(2) set
    std::vector<uint8_t> responseData(
        sizeof(pldm_msg_hdr) + PLDM_BASE_GET_PLDM_TYPES_RESP_BYTES, 0);
    auto resp = reinterpret_cast<pldm_msg*>(responseData.data());

    // Encode a valid response using new struct-based API
    pldm_base_get_pldm_types_resp typesResp{};
    typesResp.completion_code = PLDM_SUCCESS;
    typesResp.pldm_types[0].byte = 0x05; // base(0) and platform(2)
    size_t payloadLen = PLDM_BASE_GET_PLDM_TYPES_RESP_BYTES;
    auto rc =
        encode_pldm_base_get_pldm_types_resp(0, &typesResp, resp, &payloadLen);
    ASSERT_EQ(rc, PLDM_SUCCESS);

    EXPECT_NO_THROW(
        cmd.parseResponseMsg(resp, responseData.size() - sizeof(pldm_msg_hdr)));
}

TEST(GetPLDMTypes, ParseResponseMsgDecodeError)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetPLDMTypes cmd("base", "GetPLDMTypes", sub);

    // Send too-short payload to trigger decode error
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + 1, 0);
    auto resp = reinterpret_cast<pldm_msg*>(responseData.data());
    resp->payload[0] = PLDM_SUCCESS;

    EXPECT_NO_THROW(cmd.parseResponseMsg(resp, 1)); // too short
}

TEST(GetPLDMTypes, ParseResponseMsgCompletionCodeError)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetPLDMTypes cmd("base", "GetPLDMTypes", sub);

    std::vector<uint8_t> responseData(
        sizeof(pldm_msg_hdr) + PLDM_BASE_GET_PLDM_TYPES_RESP_BYTES, 0);
    auto resp = reinterpret_cast<pldm_msg*>(responseData.data());
    pldm_base_get_pldm_types_resp typesResp{};
    typesResp.completion_code = PLDM_ERROR;
    typesResp.pldm_types[0].byte = 0x01;
    size_t payloadLen2 = PLDM_BASE_GET_PLDM_TYPES_RESP_BYTES;
    auto rc =
        encode_pldm_base_get_pldm_types_resp(0, &typesResp, resp, &payloadLen2);
    ASSERT_EQ(rc, PLDM_SUCCESS);

    EXPECT_NO_THROW(
        cmd.parseResponseMsg(resp, responseData.size() - sizeof(pldm_msg_hdr)));
}

// ===== GetTID Tests =====

TEST(GetTID, CreateRequestMsg)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetTID cmd("base", "GetTID", sub);

    auto [rc, requestMsg] = cmd.createRequestMsg();
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(requestMsg.size(), sizeof(pldm_msg_hdr));
}

TEST(GetTID, ParseResponseMsgSuccess)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetTID cmd("base", "GetTID", sub);

    // Build response using new struct-based API
    size_t payloadLen = PLDM_BASE_GET_TID_RESP_BYTES;
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto resp = reinterpret_cast<pldm_msg*>(responseData.data());

    pldm_base_get_tid_resp tidResp{PLDM_SUCCESS, 5};
    auto rc = encode_pldm_base_get_tid_resp(0, &tidResp, resp, &payloadLen);
    ASSERT_EQ(rc, PLDM_SUCCESS);

    EXPECT_NO_THROW(cmd.parseResponseMsg(resp, PLDM_BASE_GET_TID_RESP_BYTES));
}

TEST(GetTID, ParseResponseMsgErrorCC)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetTID cmd("base", "GetTID", sub);

    size_t payloadLen = PLDM_BASE_GET_TID_RESP_BYTES;
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto resp = reinterpret_cast<pldm_msg*>(responseData.data());

    pldm_base_get_tid_resp tidRespErr{PLDM_ERROR, 0};
    auto rc = encode_pldm_base_get_tid_resp(0, &tidRespErr, resp, &payloadLen);
    ASSERT_EQ(rc, PLDM_SUCCESS);

    EXPECT_NO_THROW(cmd.parseResponseMsg(resp, PLDM_BASE_GET_TID_RESP_BYTES));
}

TEST(GetTID, ParseResponseMsgDecodeError)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetTID cmd("base", "GetTID", sub);

    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + 1, 0);
    auto resp = reinterpret_cast<pldm_msg*>(responseData.data());
    resp->payload[0] = PLDM_SUCCESS;

    EXPECT_NO_THROW(cmd.parseResponseMsg(resp, 1));
}

// ===== SetTID Tests =====

TEST(SetTID, CreateRequestMsg)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    SetTID cmd("base", "SetTID", sub);
    sub->callback([]() {});

    // Parse the CLI args to set tid
    std::vector<std::string> args = {"test", "test", "-t", "5"};
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (auto& a : args)
        argv.push_back(a.c_str());
    try
    {
        app.parse(static_cast<int>(argv.size()),
                  const_cast<char**>(argv.data()));
    }
    catch (const CLI::Success&)
    {}
    catch (const CLI::ParseError&)
    {}

    auto [rc, requestMsg] = cmd.createRequestMsg();
    (void)rc;
    EXPECT_EQ(requestMsg.size(), sizeof(pldm_msg_hdr) + PLDM_SET_TID_REQ_BYTES);
}

TEST(SetTID, ParseResponseMsgSuccess)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    SetTID cmd("base", "SetTID", sub);

    // Build response with success completion code
    std::vector<uint8_t> responseData(
        sizeof(pldm_msg_hdr) + PLDM_SET_TID_RESP_BYTES, 0);
    auto resp = reinterpret_cast<pldm_msg*>(responseData.data());
    resp->payload[0] = PLDM_SUCCESS;

    EXPECT_NO_THROW(cmd.parseResponseMsg(resp, PLDM_SET_TID_RESP_BYTES));
}

TEST(SetTID, ParseResponseMsgInvalidLength)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    SetTID cmd("base", "SetTID", sub);

    // Build response with wrong length
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + 5, 0);
    auto resp = reinterpret_cast<pldm_msg*>(responseData.data());

    EXPECT_NO_THROW(cmd.parseResponseMsg(resp, 5)); // Wrong payload length
}

// ===== GetPLDMVersion Tests =====

TEST(GetPLDMVersion, CreateRequestMsg)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetPLDMVersion cmd("base", "GetPLDMVersion", sub);
    sub->callback([]() {});

    // Need to parse --type option
    std::vector<std::string> args = {"test", "test", "-t", "base"};
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (auto& a : args)
        argv.push_back(a.c_str());
    try
    {
        app.parse(static_cast<int>(argv.size()),
                  const_cast<char**>(argv.data()));
    }
    catch (const CLI::Success&)
    {}
    catch (const CLI::ParseError&)
    {}

    auto [rc, requestMsg] = cmd.createRequestMsg();
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(requestMsg.size(),
              sizeof(pldm_msg_hdr) + PLDM_GET_VERSION_REQ_BYTES);
}

TEST(GetPLDMVersion, ParseResponseMsgSuccess)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetPLDMVersion cmd("base", "GetPLDMVersion", sub);
    sub->callback([]() {});

    // Parse type arg
    std::vector<std::string> args = {"test", "test", "-t", "base"};
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (auto& a : args)
        argv.push_back(a.c_str());
    try
    {
        app.parse(static_cast<int>(argv.size()),
                  const_cast<char**>(argv.data()));
    }
    catch (const CLI::Success&)
    {}
    catch (const CLI::ParseError&)
    {}

    // Build response
    ver32_t version = {0x01, 0x00, 0x00, 0x00};
    size_t payloadLen = PLDM_GET_VERSION_RESP_BYTES;
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen + 4,
                                      0); // +4 for CRC
    auto resp = reinterpret_cast<pldm_msg*>(responseData.data());

    auto rc = encode_get_version_resp(0, PLDM_SUCCESS, 0, PLDM_START_AND_END,
                                      &version, sizeof(version), resp);
    ASSERT_EQ(rc, PLDM_SUCCESS);

    EXPECT_NO_THROW(cmd.parseResponseMsg(resp, payloadLen + 4));
}

TEST(GetPLDMVersion, ParseResponseMsgDecodeError)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetPLDMVersion cmd("base", "GetPLDMVersion", sub);
    sub->callback([]() {});

    std::vector<std::string> args = {"test", "test", "-t", "base"};
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (auto& a : args)
    {
        argv.push_back(a.c_str());
    }
    try
    {
        app.parse(static_cast<int>(argv.size()),
                  const_cast<char**>(argv.data()));
    }
    catch (const CLI::Success&)
    {}
    catch (const CLI::ParseError&)
    {}

    // Too short payload triggers decode_get_version_resp() error path.
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + 1, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    resp->payload[0] = PLDM_SUCCESS;

    EXPECT_NO_THROW(cmd.parseResponseMsg(resp, 1));
}

TEST(GetPLDMVersion, ParseResponseMsgCompletionCodeError)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetPLDMVersion cmd("base", "GetPLDMVersion", sub);
    sub->callback([]() {});

    std::vector<std::string> args = {"test", "test", "-t", "base"};
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (auto& a : args)
    {
        argv.push_back(a.c_str());
    }
    try
    {
        app.parse(static_cast<int>(argv.size()),
                  const_cast<char**>(argv.data()));
    }
    catch (const CLI::Success&)
    {}
    catch (const CLI::ParseError&)
    {}

    ver32_t version = {0x01, 0x00, 0x00, 0x00};
    size_t payloadLen = PLDM_GET_VERSION_RESP_BYTES + 4;
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());

    auto rc = encode_get_version_resp(0, PLDM_ERROR, 0, PLDM_START_AND_END,
                                      &version, sizeof(version), resp);
    ASSERT_EQ(rc, PLDM_SUCCESS);

    EXPECT_NO_THROW(cmd.parseResponseMsg(resp, payloadLen));
}

TEST(GetPLDMVersion, ParseResponseMsgMultipleVersions)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetPLDMVersion cmd("base", "GetPLDMVersion", sub);
    sub->callback([]() {});

    std::vector<std::string> args = {"test", "test", "-t", "base"};
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (auto& a : args)
    {
        argv.push_back(a.c_str());
    }
    try
    {
        app.parse(static_cast<int>(argv.size()),
                  const_cast<char**>(argv.data()));
    }
    catch (const CLI::Success&)
    {}
    catch (const CLI::ParseError&)
    {}

    ver32_t versions[2] = {{0x01, 0x00, 0x00, 0x00}, {0x02, 0x00, 0x00, 0x00}};
    // first version + one extra version + 4-byte checksum trailer.
    size_t payloadLen = PLDM_GET_VERSION_RESP_BYTES + sizeof(ver32_t) + 4;
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());

    auto rc = encode_get_version_resp(0, PLDM_SUCCESS, 0, PLDM_START, versions,
                                      sizeof(versions), resp);
    ASSERT_EQ(rc, PLDM_SUCCESS);

    EXPECT_NO_THROW(cmd.parseResponseMsg(resp, payloadLen));
}

// ===== GetPLDMCommands Tests =====

TEST(GetPLDMCommands, CreateRequestMsg)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetPLDMCommands cmd("base", "GetPLDMCommands", sub);
    sub->callback([]() {});

    std::vector<std::string> args = {"test", "test", "-t", "base"};
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (auto& a : args)
        argv.push_back(a.c_str());
    try
    {
        app.parse(static_cast<int>(argv.size()),
                  const_cast<char**>(argv.data()));
    }
    catch (const CLI::Success&)
    {}
    catch (const CLI::ParseError&)
    {}

    auto [rc, requestMsg] = cmd.createRequestMsg();
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(requestMsg.size(),
              sizeof(pldm_msg_hdr) + PLDM_GET_COMMANDS_REQ_BYTES);
}

TEST(GetPLDMCommands, CreateRequestMsgInvalidVersionLength)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetPLDMCommands cmd("base", "GetPLDMCommands", sub);
    sub->callback([]() {});

    std::vector<std::string> args = {"test", "test", "-t", "base", "-d", "1"};
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (auto& a : args)
    {
        argv.push_back(a.c_str());
    }
    try
    {
        app.parse(static_cast<int>(argv.size()),
                  const_cast<char**>(argv.data()));
    }
    catch (const CLI::ParseError&)
    {}

    auto [rc, requestMsg] = cmd.createRequestMsg();
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(requestMsg.size(),
              sizeof(pldm_msg_hdr) + PLDM_GET_COMMANDS_REQ_BYTES);
}

TEST(GetPLDMCommands, CreateRequestMsgValidVersionLength)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetPLDMCommands cmd("base", "GetPLDMCommands", sub);
    sub->callback([]() {});

    std::vector<std::string> args = {"test", "test", "-t", "base", "-d",
                                     "1",    "2",    "3",  "4"};
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (auto& a : args)
    {
        argv.push_back(a.c_str());
    }
    try
    {
        app.parse(static_cast<int>(argv.size()),
                  const_cast<char**>(argv.data()));
    }
    catch (const CLI::ParseError&)
    {}

    auto [rc, requestMsg] = cmd.createRequestMsg();
    ASSERT_EQ(rc, PLDM_SUCCESS);
    ASSERT_EQ(requestMsg.size(),
              sizeof(pldm_msg_hdr) + PLDM_GET_COMMANDS_REQ_BYTES);

    auto* req = reinterpret_cast<pldm_msg*>(requestMsg.data());
    uint8_t reqType = 0;
    ver32_t reqVersion{};
    rc = decode_get_commands_req(req, PLDM_GET_COMMANDS_REQ_BYTES, &reqType,
                                 &reqVersion);
    ASSERT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(reqType, PLDM_BASE);
    EXPECT_EQ(reqVersion.alpha, 1);
    EXPECT_EQ(reqVersion.update, 2);
    EXPECT_EQ(reqVersion.minor, 3);
    EXPECT_EQ(reqVersion.major, 4);
}

TEST(GetPLDMCommands, ParseResponseMsgSuccess)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetPLDMCommands cmd("base", "GetPLDMCommands", sub);
    sub->callback([]() {});

    std::vector<std::string> args = {"test", "test", "-t", "base"};
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (auto& a : args)
        argv.push_back(a.c_str());
    try
    {
        app.parse(static_cast<int>(argv.size()),
                  const_cast<char**>(argv.data()));
    }
    catch (const CLI::ParseError&)
    {}

    // Build response with some commands set
    size_t payloadLen = 1 + 32; // cc + 32 bytes of command bitfield
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto resp = reinterpret_cast<pldm_msg*>(responseData.data());

    std::vector<bitfield8_t> cmdTypes(32, {0});
    // Set GetTID (cmd 2) and GetPLDMTypes (cmd 4) bits
    cmdTypes[0].byte = (1 << PLDM_GET_TID) | (1 << PLDM_GET_PLDM_TYPES);

    auto rc = encode_get_commands_resp(0, PLDM_SUCCESS, cmdTypes.data(), resp);
    ASSERT_EQ(rc, PLDM_SUCCESS);

    EXPECT_NO_THROW(cmd.parseResponseMsg(resp, payloadLen));
}

TEST(GetPLDMCommands, ParseResponseMsgSuccessBiosType)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetPLDMCommands cmd("base", "GetPLDMCommands", sub);
    sub->callback([]() {});

    std::vector<std::string> args = {"test", "test", "-t", "bios"};
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (auto& a : args)
    {
        argv.push_back(a.c_str());
    }
    try
    {
        app.parse(static_cast<int>(argv.size()),
                  const_cast<char**>(argv.data()));
    }
    catch (const CLI::ParseError&)
    {}

    size_t payloadLen = 1 + 32;
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    std::vector<bitfield8_t> cmdTypes(32, {0});
    cmdTypes[PLDM_GET_BIOS_TABLE / 8].byte |= (1 << (PLDM_GET_BIOS_TABLE % 8));

    auto rc = encode_get_commands_resp(0, PLDM_SUCCESS, cmdTypes.data(), resp);
    ASSERT_EQ(rc, PLDM_SUCCESS);

    EXPECT_NO_THROW(cmd.parseResponseMsg(resp, payloadLen));
}

TEST(GetPLDMCommands, ParseResponseMsgSuccessFwUpdateType)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetPLDMCommands cmd("base", "GetPLDMCommands", sub);
    sub->callback([]() {});

    std::vector<std::string> args = {"test", "test", "-t", "firmware update"};
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (auto& a : args)
    {
        argv.push_back(a.c_str());
    }
    try
    {
        app.parse(static_cast<int>(argv.size()),
                  const_cast<char**>(argv.data()));
    }
    catch (const CLI::ParseError&)
    {}

    size_t payloadLen = 1 + 32;
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    std::vector<bitfield8_t> cmdTypes(32, {0});
    cmdTypes[PLDM_QUERY_DEVICE_IDENTIFIERS / 8].byte |=
        (1 << (PLDM_QUERY_DEVICE_IDENTIFIERS % 8));

    auto rc = encode_get_commands_resp(0, PLDM_SUCCESS, cmdTypes.data(), resp);
    ASSERT_EQ(rc, PLDM_SUCCESS);

    EXPECT_NO_THROW(cmd.parseResponseMsg(resp, payloadLen));
}

TEST(GetPLDMCommands, ParseResponseMsgDecodeError)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetPLDMCommands cmd("base", "GetPLDMCommands", sub);
    sub->callback([]() {});

    std::vector<std::string> args = {"test", "test", "-t", "base"};
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (auto& a : args)
        argv.push_back(a.c_str());
    try
    {
        app.parse(static_cast<int>(argv.size()),
                  const_cast<char**>(argv.data()));
    }
    catch (const CLI::ParseError&)
    {}

    // Too short payload
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + 1, 0);
    auto resp = reinterpret_cast<pldm_msg*>(responseData.data());

    EXPECT_NO_THROW(cmd.parseResponseMsg(resp, 1));
}

TEST(GetPLDMCommands, ParseResponseMsgCompletionCodeError)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetPLDMCommands cmd("base", "GetPLDMCommands", sub);
    sub->callback([]() {});

    std::vector<std::string> args = {"test", "test", "-t", "base"};
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (auto& a : args)
    {
        argv.push_back(a.c_str());
    }
    try
    {
        app.parse(static_cast<int>(argv.size()),
                  const_cast<char**>(argv.data()));
    }
    catch (const CLI::ParseError&)
    {}

    size_t payloadLen = 1 + 32;
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    std::vector<bitfield8_t> cmdTypes(32, {0});
    cmdTypes[0].byte = (1 << PLDM_GET_TID);

    auto rc = encode_get_commands_resp(0, PLDM_ERROR, cmdTypes.data(), resp);
    ASSERT_EQ(rc, PLDM_SUCCESS);

    EXPECT_NO_THROW(cmd.parseResponseMsg(resp, payloadLen));
}

TEST(GetPLDMCommands, ParseResponseMsgUnknownCommandBitForBaseType)
{
    CLI::App app{"test"};
    auto sub = app.add_subcommand("test", "test");
    GetPLDMCommands cmd("base", "GetPLDMCommands", sub);
    sub->callback([]() {});

    std::vector<std::string> args = {"test", "test", "-t", "base"};
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (auto& a : args)
    {
        argv.push_back(a.c_str());
    }
    try
    {
        app.parse(static_cast<int>(argv.size()),
                  const_cast<char**>(argv.data()));
    }
    catch (const CLI::ParseError&)
    {}

    size_t payloadLen = 1 + 32;
    std::vector<uint8_t> responseData(sizeof(pldm_msg_hdr) + payloadLen, 0);
    auto* resp = reinterpret_cast<pldm_msg*>(responseData.data());
    std::vector<bitfield8_t> cmdTypes(32, {0});
    // Set an unsupported command code for base type (31 is not in pldmBaseCmds)
    cmdTypes[31 / 8].byte |= (1 << (31 % 8));

    auto rc = encode_get_commands_resp(0, PLDM_SUCCESS, cmdTypes.data(), resp);
    ASSERT_EQ(rc, PLDM_SUCCESS);

    EXPECT_NO_THROW(cmd.parseResponseMsg(resp, payloadLen));
}

// ===== registerCommand Tests =====

TEST(BaseRegisterCommand, RegistersSubcommands)
{
    CLI::App app{"test"};
    pldmtool::base::registerCommand(app);

    // Verify subcommands were registered
    auto base = app.get_subcommand("base");
    EXPECT_NE(base, nullptr);

    // Check that all expected subcommands exist
    EXPECT_NE(base->get_subcommand("GetPLDMTypes"), nullptr);
    EXPECT_NE(base->get_subcommand("GetPLDMVersion"), nullptr);
    EXPECT_NE(base->get_subcommand("GetTID"), nullptr);
    EXPECT_NE(base->get_subcommand("GetPLDMCommands"), nullptr);
    EXPECT_NE(base->get_subcommand("SetTID"), nullptr);
}
