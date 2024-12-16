/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION &
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
#include "libpldm/base.h"
#include "libpldm/pdr.h"
#include "libpldm/platform.h"
#include "pdr.h"
#include "pldm_types.h"

#include "../pdr_json_parser.hpp"
#include "../sensor_to_dbus.hpp"
#include "common/types.hpp"
#include "common/utils.hpp"
#include "mockup_responder.hpp"

#include <string.h>

#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdeventplus/event.hpp>

#include <array>
#include <fstream>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#define private public
#define protected public

#include "mockup_responder.hpp"

using Type = uint8_t;

const uint8_t MCTP_TAG_PLDM = 0;
const uint8_t MCTP_MSG_TAG_RESP = MCTP_TAG_PLDM;
const uint8_t MCTP_MSG_TYPE_PLDM = 1;
const uint8_t MCTP_TAG_OWNER_REQ = 0x01;
const uint8_t MCTP_MSG_TAG_REQ = (MCTP_TAG_OWNER_REQ << 3) | MCTP_TAG_PLDM;

static const std::map<uint8_t, std::vector<uint8_t>> capabilities{
    {PLDM_BASE,
     {PLDM_GET_TID, PLDM_GET_PLDM_VERSION, PLDM_GET_PLDM_TYPES,
      PLDM_GET_PLDM_COMMANDS}},
    {PLDM_PLATFORM,
     {PLDM_GET_PDR, PLDM_SET_STATE_EFFECTER_STATES, PLDM_SET_EVENT_RECEIVER,
      PLDM_GET_SENSOR_READING, PLDM_GET_STATE_SENSOR_READINGS,
      PLDM_SET_NUMERIC_EFFECTER_VALUE, PLDM_GET_NUMERIC_EFFECTER_VALUE,
      PLDM_PLATFORM_EVENT_MESSAGE}},
    {PLDM_BIOS,
     {PLDM_GET_DATE_TIME, PLDM_SET_DATE_TIME, PLDM_GET_BIOS_TABLE,
      PLDM_GET_BIOS_ATTRIBUTE_CURRENT_VALUE_BY_HANDLE,
      PLDM_SET_BIOS_ATTRIBUTE_CURRENT_VALUE, PLDM_SET_BIOS_TABLE}},
    {PLDM_FRU,
     {PLDM_GET_FRU_RECORD_TABLE_METADATA, PLDM_GET_FRU_RECORD_TABLE,
      PLDM_GET_FRU_RECORD_BY_OPTION}}};

constexpr auto hdrSize = sizeof(pldm_msg_hdr);

using ::testing::ElementsAre;
using Request = std::vector<uint8_t>;
using Response = std::vector<uint8_t>;

class MockupResponderTest : public testing::Test
{
  protected:
    MockupResponderTest() : event(sdeventplus::Event::get_default())
    {
        uint8_t testUUID[16] = {0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11};
        systemBus = std::make_shared<sdbusplus::asio::connection>(io);
        objServer = std::make_shared<sdbusplus::asio::object_server>(systemBus);
        mockupResponder = std::make_shared<MockupResponder::MockupResponder>(
            true, event, *objServer, 31, "pdr.json", 256, testUUID);
    }

    ~MockupResponderTest()
    {
        tearDownResources();
    }

  private:
    void tearDownResources()
    {
        if (mockupResponder && mockupResponder->getPdrRepo())
        {
            pldm_pdr_destroy(mockupResponder->getPdrRepo());
        }

        if (objServer)
        {
            cleanupSensorsAndEffecters(*objServer);
        }

        mockupResponder.reset();
        objServer.reset();
        systemBus.reset();
    }

    void cleanupSensorsAndEffecters(sdbusplus::asio::object_server& objServer)
    {
        for (auto& sensor : sensors)
        {
            if (sensor->iface)
            {
                objServer.remove_interface(sensor->iface);
                sensor->iface.reset();
            }
            if (sensor->operationalIface)
            {
                objServer.remove_interface(sensor->operationalIface);
                sensor->operationalIface.reset();
            }
        }
        sensors.clear();

        for (auto& effecter : effecters)
        {
            if (effecter->iface)
            {
                objServer.remove_interface(effecter->iface);
                effecter->iface.reset();
            }
            if (effecter->operationalIface)
            {
                objServer.remove_interface(effecter->operationalIface);
                effecter->operationalIface.reset();
            }
        }
        effecters.clear();
    }

    std::shared_ptr<sdbusplus::asio::connection> systemBus;
    std::shared_ptr<sdbusplus::asio::object_server> objServer;
    std::shared_ptr<MockupResponder::MockupResponder> mockupResponder;
    boost::asio::io_context io;
    sdeventplus::Event event;
};

TEST_F(MockupResponderTest, testGoodGetTID)
{
    uint8_t instanceID = 0;
    uint8_t eid = 31;
    Request request(hdrSize);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    auto rc = encode_get_tid_req(instanceID, requestMsg);
    ASSERT_EQ(rc, PLDM_SUCCESS);

    uint8_t hdr[] = {MCTP_MSG_TAG_REQ, eid, MCTP_MSG_TYPE_PLDM};

    std::vector<uint8_t> fullMessage;
    fullMessage.insert(fullMessage.end(), std::begin(hdr), std::end(hdr));
    fullMessage.insert(fullMessage.end(), request.begin(), request.end());

    auto responseOpt = mockupResponder->processRxMsg(fullMessage);
    ASSERT_TRUE(responseOpt.has_value());

    const auto& response = responseOpt.value();

    EXPECT_EQ(response.size(), hdrSize + PLDM_GET_TID_RESP_BYTES);

    auto* responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responsePtr->hdr.instance_id, instanceID);

    uint8_t completionCode{};
    uint8_t tid{};
    rc = decode_get_tid_resp(responsePtr, response.size() - hdrSize,
                             &completionCode, &tid);

    ASSERT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_SUCCESS);
    EXPECT_EQ(tid, mockupResponder->getTid());
}

TEST_F(MockupResponderTest, testGoodGetPLDMTypes)
{
    uint8_t instanceID = 0;
    uint8_t eid = 31;
    Request request(hdrSize);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    auto rc = encode_get_types_req(instanceID, requestMsg);
    ASSERT_EQ(rc, PLDM_SUCCESS);

    uint8_t hdr[] = {MCTP_MSG_TAG_REQ, eid, MCTP_MSG_TYPE_PLDM};

    std::vector<uint8_t> fullMessage;
    fullMessage.insert(fullMessage.end(), std::begin(hdr), std::end(hdr));
    fullMessage.insert(fullMessage.end(), request.begin(), request.end());

    auto responseOpt = mockupResponder->processRxMsg(fullMessage);
    ASSERT_TRUE(responseOpt.has_value());

    const auto& response = responseOpt.value();

    EXPECT_EQ(response.size(), hdrSize + PLDM_GET_TYPES_RESP_BYTES);

    auto* responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responsePtr->hdr.instance_id, instanceID);

    uint8_t completionCode{};
    std::array<bitfield8_t, 8> types{};
    rc = decode_get_types_resp(responsePtr, response.size() - hdrSize,
                               &completionCode, types.data());

    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_SUCCESS);

    for (const auto& type : capabilities)
    {
        auto index = type.first / 8;
        auto bit = type.first % 8;
        EXPECT_TRUE(types[index].byte & (1 << bit));
    }
}

TEST_F(MockupResponderTest, testGoodGetPLDMCommands)
{
    uint8_t instanceID = 0;
    uint8_t eid = 31;
    Request request(hdrSize + PLDM_GET_COMMANDS_REQ_BYTES);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    uint8_t pldmType = 0x00;
    ver32_t version{0xFF, 0xFF, 0xFF, 0xFF};

    auto rc =
        encode_get_commands_req(instanceID, pldmType, version, requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    uint8_t hdr[] = {MCTP_MSG_TAG_REQ, eid, MCTP_MSG_TYPE_PLDM};

    std::vector<uint8_t> fullMessage;
    fullMessage.insert(fullMessage.end(), std::begin(hdr), std::end(hdr));
    fullMessage.insert(fullMessage.end(), request.begin(), request.end());

    auto responseOpt = mockupResponder->processRxMsg(fullMessage);
    ASSERT_TRUE(responseOpt.has_value());

    const auto& response = responseOpt.value();

    EXPECT_EQ(response.size(), hdrSize + PLDM_GET_COMMANDS_RESP_BYTES);

    auto* responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responsePtr->hdr.instance_id, instanceID);

    uint8_t completionCode{};
    std::array<bitfield8_t, 32> cmds{};
    rc = decode_get_commands_resp(responsePtr, response.size() - hdrSize,
                                  &completionCode, cmds.data());

    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_SUCCESS);

    auto it = capabilities.find(pldmType);
    ASSERT_NE(it, capabilities.end());
    for (const auto& cmd : it->second)
    {
        auto index = cmd / 8;
        auto bit = cmd % 8;
        EXPECT_TRUE(cmds[index].byte & (1 << bit));
    }
}

TEST_F(MockupResponderTest, testBadGetPLDMCommands)
{
    uint8_t instanceID = 0;
    Request request(hdrSize + PLDM_GET_COMMANDS_REQ_BYTES);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    uint8_t pldmType = 0x00;
    ver32_t version{0xFF, 0xFF, 0xFF, 0xFF};
    auto rc =
        encode_get_commands_req(instanceID, pldmType, version, requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    auto response =
        mockupResponder->getPLDMCommands(requestMsg, request.size());

    ASSERT_FALSE(response.empty());
    EXPECT_EQ(response.size(), hdrSize + PLDM_CC_ONLY_RESP_BYTES);

    auto* responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    EXPECT_EQ(responsePtr->hdr.instance_id, instanceID);

    uint8_t completionCode{};
    std::array<bitfield8_t, 32> cmds{};
    rc = decode_get_commands_resp(responsePtr, response.size() - hdrSize,
                                  &completionCode, cmds.data());
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_ERROR_INVALID_LENGTH);
}

TEST_F(MockupResponderTest, testGoodGetPLDMVersion)
{
    uint8_t instanceID = 0;
    uint8_t eid = 31;

    Request request(hdrSize + PLDM_GET_VERSION_REQ_BYTES, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    uint32_t transferHandle = 0;
    uint8_t transferFlag = PLDM_START_AND_END;
    Type type = PLDM_BASE;

    auto rc = encode_get_version_req(instanceID, transferHandle, transferFlag,
                                     type, requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    uint8_t hdr[] = {MCTP_MSG_TAG_REQ, eid, MCTP_MSG_TYPE_PLDM};

    std::vector<uint8_t> fullMessage;
    fullMessage.insert(fullMessage.end(), std::begin(hdr), std::end(hdr));
    fullMessage.insert(fullMessage.end(), request.begin(), request.end());

    auto responseOpt = mockupResponder->processRxMsg(fullMessage);
    ASSERT_TRUE(responseOpt.has_value());

    const auto& response = responseOpt.value();

    EXPECT_EQ(response.size(), hdrSize + PLDM_GET_VERSION_RESP_BYTES);

    auto* responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responsePtr->hdr.instance_id, instanceID);

    uint8_t completionCode{};
    uint32_t outTransferHandle{};
    uint8_t outTransferFlag{};
    ver32_t version{};

    rc = decode_get_version_resp(responsePtr, response.size() - hdrSize,
                                 &completionCode, &outTransferHandle,
                                 &outTransferFlag, &version);

    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_SUCCESS);
    EXPECT_EQ(outTransferHandle, 0);
    EXPECT_EQ(outTransferFlag, PLDM_START_AND_END);

    EXPECT_EQ(version.major, 0xF1);
    EXPECT_EQ(version.minor, 0xF0);
    EXPECT_EQ(version.update, 0xF0);
    EXPECT_EQ(version.alpha, 0x00);
}

TEST_F(MockupResponderTest, testBadTypeGetPLDMVersion)
{
    uint8_t instanceID = 0;

    Request request(hdrSize + PLDM_GET_VERSION_REQ_BYTES, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    uint32_t transferHandle = 0;
    uint8_t transferFlag = PLDM_START_AND_END;
    Type type = 7;

    auto rc = encode_get_version_req(instanceID, transferHandle, transferFlag,
                                     type, requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    auto response =
        mockupResponder->getPLDMVersion(requestMsg, request.size() - hdrSize);

    ASSERT_FALSE(response.empty());
    EXPECT_EQ(response.size(), hdrSize + PLDM_CC_ONLY_RESP_BYTES);

    auto* responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    EXPECT_EQ(responsePtr->hdr.instance_id, instanceID);

    uint8_t completionCode{};
    uint32_t outTransferHandle{};
    uint8_t outTransferFlag{};
    ver32_t version{};

    rc = decode_get_version_resp(responsePtr, response.size() - hdrSize,
                                 &completionCode, &outTransferHandle,
                                 &outTransferFlag, &version);
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_ERROR_INVALID_PLDM_TYPE);
}

TEST_F(MockupResponderTest, testBadLengthGetPLDMVersion)
{
    uint8_t instanceID = 0;

    Request request(hdrSize + PLDM_GET_VERSION_REQ_BYTES, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    uint32_t transferHandle = 0;
    uint8_t transferFlag = PLDM_START_AND_END;
    Type type = PLDM_BASE;

    auto rc = encode_get_version_req(instanceID, transferHandle, transferFlag,
                                     type, requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    auto response = mockupResponder->getPLDMVersion(requestMsg, request.size());

    ASSERT_FALSE(response.empty());
    EXPECT_EQ(response.size(), hdrSize + PLDM_CC_ONLY_RESP_BYTES);

    auto* responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    EXPECT_EQ(responsePtr->hdr.instance_id, instanceID);

    uint8_t completionCode{};
    uint32_t outTransferHandle{};
    uint8_t outTransferFlag{};
    ver32_t version{};

    rc = decode_get_version_resp(responsePtr, response.size() - hdrSize,
                                 &completionCode, &outTransferHandle,
                                 &outTransferFlag, &version);
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_ERROR_INVALID_LENGTH);
}

TEST_F(MockupResponderTest, testGoodSetTID)
{
    uint8_t instanceID = 0;
    uint8_t eid = 31;
    uint8_t newTID = 31;

    Request request(hdrSize + PLDM_SET_TID_REQ_BYTES, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    auto rc = encode_set_tid_req(instanceID, newTID, requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    uint8_t hdr[] = {MCTP_MSG_TAG_REQ, eid, MCTP_MSG_TYPE_PLDM};

    std::vector<uint8_t> fullMessage;
    fullMessage.insert(fullMessage.end(), std::begin(hdr), std::end(hdr));
    fullMessage.insert(fullMessage.end(), request.begin(), request.end());

    auto responseOpt = mockupResponder->processRxMsg(fullMessage);
    ASSERT_TRUE(responseOpt.has_value());

    const auto& response = responseOpt.value();

    EXPECT_EQ(response.size(), hdrSize + PLDM_SET_TID_RESP_BYTES);

    auto* responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responsePtr->hdr.instance_id, instanceID);

    uint8_t completionCode{};
    rc = decode_cc_only_resp(responsePtr, response.size() - hdrSize,
                             &completionCode);

    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_SUCCESS);

    EXPECT_EQ(mockupResponder->getTid(), newTID);
}

TEST_F(MockupResponderTest, testBadPayloadLengthSetTID)
{
    uint8_t instanceID = 0;
    uint8_t newTID = 1;

    Request request(hdrSize + PLDM_SET_TID_REQ_BYTES, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    auto rc = encode_set_tid_req(instanceID, newTID, requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    auto response =
        mockupResponder->setTID(requestMsg, request.size(), *mockupResponder);

    ASSERT_FALSE(response.empty());
    EXPECT_EQ(response.size(), hdrSize + PLDM_CC_ONLY_RESP_BYTES);

    auto* responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    EXPECT_EQ(responsePtr->hdr.instance_id, instanceID);

    uint8_t completionCode{};
    rc = decode_cc_only_resp(responsePtr, response.size() - hdrSize,
                             &completionCode);
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_ERROR_INVALID_LENGTH);
}

TEST_F(MockupResponderTest, testGoodGetEventMessageBufferSize)
{
    uint8_t instanceID = 0;
    uint8_t eid = 31;
    uint16_t maxBufferSize = 256;
    size_t payloadLength = sizeof(uint16_t);

    Request request(hdrSize + payloadLength, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    auto rc = encode_event_message_buffer_size_req(instanceID, maxBufferSize,
                                                   requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    uint8_t hdr[] = {MCTP_MSG_TAG_REQ, eid, MCTP_MSG_TYPE_PLDM};

    std::vector<uint8_t> fullMessage;
    fullMessage.insert(fullMessage.end(), std::begin(hdr), std::end(hdr));
    fullMessage.insert(fullMessage.end(), request.begin(), request.end());

    auto responseOpt = mockupResponder->processRxMsg(fullMessage);
    ASSERT_TRUE(responseOpt.has_value());

    const auto& response = responseOpt.value();

    size_t expectedSize = hdrSize + sizeof(uint8_t) + sizeof(uint16_t);
    EXPECT_EQ(response.size(), expectedSize);

    auto* responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responsePtr->hdr.instance_id, instanceID);

    uint8_t completionCode{};
    uint16_t terminusBufferSize{};
    rc = decode_event_message_buffer_size_resp(
        responsePtr, response.size() - hdrSize, &completionCode,
        &terminusBufferSize);

    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_SUCCESS);
    EXPECT_EQ(terminusBufferSize, mockupResponder->getTerminusMaxBufferSize());
}

TEST_F(MockupResponderTest, testBadPayloadLengthGetEventMessageBufferSize)
{
    uint8_t instanceID = 0;
    uint16_t maxBufferSize = 256;
    size_t payloadLength = sizeof(uint16_t);
    Request request(hdrSize + payloadLength, 0);

    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    auto rc = encode_event_message_buffer_size_req(instanceID, maxBufferSize,
                                                   requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    auto response = mockupResponder->getEventMessageBufferSize(
        requestMsg, request.size(), *mockupResponder);

    ASSERT_FALSE(response.empty());
    size_t expectedSize = hdrSize + sizeof(uint8_t);
    EXPECT_EQ(response.size(), expectedSize);

    auto* responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    EXPECT_EQ(responsePtr->hdr.instance_id, instanceID);

    uint8_t completionCode{};
    uint16_t terminusBufferSize{};
    rc = decode_event_message_buffer_size_resp(
        responsePtr, response.size() - hdrSize, &completionCode,
        &terminusBufferSize);

    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_ERROR_INVALID_LENGTH);
}

TEST_F(MockupResponderTest, testGoodGetEventMessageSupported)
{
    uint8_t instanceID = 0;
    uint8_t eid = 31;
    uint8_t formatVersion = 0x01;

    Request request(hdrSize + PLDM_EVENT_MESSAGE_SUPPORTED_REQ_BYTES, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    auto rc = encode_event_message_supported_req(instanceID, formatVersion,
                                                 requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    uint8_t hdr[] = {MCTP_MSG_TAG_REQ, eid, MCTP_MSG_TYPE_PLDM};

    std::vector<uint8_t> fullMessage;
    fullMessage.insert(fullMessage.end(), std::begin(hdr), std::end(hdr));
    fullMessage.insert(fullMessage.end(), request.begin(), request.end());

    auto responseOpt = mockupResponder->processRxMsg(fullMessage);
    ASSERT_TRUE(responseOpt.has_value());

    const auto& response = responseOpt.value();

    size_t expectedSize = hdrSize + sizeof(pldm_event_message_supported_resp);
    EXPECT_EQ(response.size(), expectedSize);

    auto* responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responsePtr->hdr.instance_id, instanceID);

    uint8_t retCompletionCode = 0;
    uint8_t retSynchronyConfiguration = 0;
    uint8_t retSynchronyConfigurationSupported = 0;
    uint8_t retNumberEventClassReturned = 0;
    uint8_t* retEventClasses = nullptr;

    rc = decode_event_message_supported_resp(
        responsePtr, response.size() - hdrSize, &retCompletionCode,
        &retSynchronyConfiguration, &retSynchronyConfigurationSupported,
        &retNumberEventClassReturned, &retEventClasses);

    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(retCompletionCode, PLDM_SUCCESS);
    EXPECT_EQ(retSynchronyConfiguration, 0x00);
    EXPECT_EQ(retSynchronyConfigurationSupported, 0x0B);
    EXPECT_EQ(retNumberEventClassReturned, 1);
    ASSERT_NE(retEventClasses, nullptr);
    EXPECT_EQ(retEventClasses[0], 0);
}

TEST_F(MockupResponderTest, testBadPayloadLengthGetEventMessageSupported)
{
    uint8_t instanceID = 0;

    Request request(hdrSize + PLDM_EVENT_MESSAGE_SUPPORTED_REQ_BYTES, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    uint8_t formatVersion = 0x01;
    auto rc = encode_event_message_supported_req(instanceID, formatVersion,
                                                 requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    auto response = mockupResponder->getEventMessageSupported(
        requestMsg, request.size() - hdrSize - 1);

    ASSERT_FALSE(response.empty());

    size_t expectedSize = hdrSize + 1;
    EXPECT_EQ(response.size(), expectedSize);

    auto* responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    EXPECT_EQ(responsePtr->hdr.instance_id, requestMsg->hdr.instance_id);

    uint8_t retCompletionCode = 0;
    uint8_t retSynchronyConfiguration = 0;
    uint8_t retSynchronyConfigurationSupported = 0;
    uint8_t retNumberEventClassReturned = 0;
    uint8_t* retEventClasses = nullptr;

    rc = decode_event_message_supported_resp(
        responsePtr, response.size() - hdrSize, &retCompletionCode,
        &retSynchronyConfiguration, &retSynchronyConfigurationSupported,
        &retNumberEventClassReturned, &retEventClasses);

    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(PLDM_ERROR_INVALID_LENGTH, retCompletionCode);
}

TEST_F(MockupResponderTest, testBadFormatGetEventMessageSupported)
{
    uint8_t instanceID = 0;

    Request request(hdrSize + PLDM_EVENT_MESSAGE_SUPPORTED_REQ_BYTES, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    uint8_t formatVersion = 0x02;
    auto rc = encode_event_message_supported_req(instanceID, formatVersion,
                                                 requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    auto response = mockupResponder->getEventMessageSupported(
        requestMsg, request.size() - hdrSize);

    ASSERT_FALSE(response.empty());

    size_t expectedSize = hdrSize + 1;
    EXPECT_EQ(response.size(), expectedSize);

    auto* responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    EXPECT_EQ(responsePtr->hdr.instance_id, requestMsg->hdr.instance_id);

    uint8_t retCompletionCode = 0;
    uint8_t retSynchronyConfiguration = 0;
    uint8_t retSynchronyConfigurationSupported = 0;
    uint8_t retNumberEventClassReturned = 0;
    uint8_t* retEventClasses = nullptr;

    rc = decode_event_message_supported_resp(
        responsePtr, response.size() - hdrSize, &retCompletionCode,
        &retSynchronyConfiguration, &retSynchronyConfigurationSupported,
        &retNumberEventClassReturned, &retEventClasses);

    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(PLDM_ERROR_INVALID_DATA, retCompletionCode);
}

TEST_F(MockupResponderTest, testGoodSetEventReceiver)
{
    uint8_t instanceID = 0;
    uint8_t eid = 31;
    uint8_t eventMessageGlobalEnable =
        PLDM_EVENT_MESSAGE_GLOBAL_ENABLE_ASYNC_KEEP_ALIVE;
    uint8_t transportProtocolType = PLDM_TRANSPORT_PROTOCOL_TYPE_MCTP;
    uint8_t eventReceiverAddressInfo = 0x05;
    uint16_t heartbeatTimer = 100;

    Request request(hdrSize + PLDM_SET_EVENT_RECEIVER_REQ_BYTES, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    auto rc = encode_set_event_receiver_req(
        instanceID, eventMessageGlobalEnable, transportProtocolType,
        eventReceiverAddressInfo, heartbeatTimer, requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    uint8_t hdr[] = {MCTP_MSG_TAG_REQ, eid, MCTP_MSG_TYPE_PLDM};

    std::vector<uint8_t> fullMessage;
    fullMessage.insert(fullMessage.end(), std::begin(hdr), std::end(hdr));
    fullMessage.insert(fullMessage.end(), request.begin(), request.end());

    auto responseOpt = mockupResponder->processRxMsg(fullMessage);
    ASSERT_TRUE(responseOpt.has_value());

    const auto& response = responseOpt.value();

    size_t expectedSize = hdrSize + PLDM_SET_EVENT_RECEIVER_RESP_BYTES;
    EXPECT_EQ(response.size(), expectedSize);

    auto* responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responsePtr->hdr.instance_id, instanceID);

    uint8_t completionCode;
    rc = decode_set_event_receiver_resp(responsePtr, response.size() - hdrSize,
                                        &completionCode);

    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_SUCCESS);
    EXPECT_EQ(mockupResponder->getEventReceiverEid(), eventReceiverAddressInfo);
}

TEST_F(MockupResponderTest, testBadEventMessageSetEventReceiver)
{
    uint8_t instanceID = 0;
    uint8_t eventMessageGlobalEnable = 10;
    uint8_t transportProtocolType = PLDM_TRANSPORT_PROTOCOL_TYPE_MCTP;
    uint8_t eventReceiverAddressInfo = 0x05;
    uint16_t heartbeatTimer = 100;

    Request request(hdrSize + PLDM_SET_EVENT_RECEIVER_REQ_BYTES, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    auto rc = encode_set_event_receiver_req(
        instanceID, eventMessageGlobalEnable, transportProtocolType,
        eventReceiverAddressInfo, heartbeatTimer, requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    auto response = mockupResponder->setEventReceiver(
        requestMsg, request.size() - hdrSize, *mockupResponder);

    ASSERT_FALSE(response.empty());
    size_t expectedSize = hdrSize + 1;
    EXPECT_EQ(response.size(), expectedSize);

    auto* responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    EXPECT_EQ(responsePtr->hdr.instance_id, requestMsg->hdr.instance_id);

    uint8_t completionCode;
    rc = decode_set_event_receiver_resp(responsePtr, response.size() - hdrSize,
                                        &completionCode);

    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_PLATFORM_ENABLE_METHOD_NOT_SUPPORTED);
}

TEST_F(MockupResponderTest, testBadPayloadMessageSetEventReceiver)
{
    uint8_t instanceID = 0;
    uint8_t eventMessageGlobalEnable =
        PLDM_EVENT_MESSAGE_GLOBAL_ENABLE_ASYNC_KEEP_ALIVE;
    uint8_t transportProtocolType = PLDM_TRANSPORT_PROTOCOL_TYPE_MCTP;
    uint8_t eventReceiverAddressInfo = 0x05;
    uint16_t heartbeatTimer = 100;

    Request request(hdrSize + PLDM_SET_EVENT_RECEIVER_REQ_BYTES, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    auto rc = encode_set_event_receiver_req(
        instanceID, eventMessageGlobalEnable, transportProtocolType,
        eventReceiverAddressInfo, heartbeatTimer, requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    auto response = mockupResponder->setEventReceiver(
        requestMsg, request.size(), *mockupResponder);

    ASSERT_FALSE(response.empty());
    size_t expectedSize = hdrSize + 1;
    EXPECT_EQ(response.size(), expectedSize);

    auto* responsePtr = reinterpret_cast<pldm_msg*>(response.data());
    EXPECT_EQ(responsePtr->hdr.instance_id, requestMsg->hdr.instance_id);

    uint8_t completionCode;
    rc = decode_set_event_receiver_resp(responsePtr, response.size() - hdrSize,
                                        &completionCode);

    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_ERROR_INVALID_LENGTH);
}

TEST_F(MockupResponderTest, testGoodGetTerminusUID)
{
    uint8_t instanceID = 0;
    uint8_t eid = 31;

    Request request(hdrSize, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    struct pldm_header_info header;
    header.msg_type = PLDM_REQUEST;
    header.instance = instanceID;
    header.pldm_type = PLDM_PLATFORM;
    header.command = PLDM_GET_TERMINUS_UID;

    auto rc = pack_pldm_header(&header, &(requestMsg->hdr));
    EXPECT_EQ(rc, PLDM_SUCCESS);

    uint8_t hdr[] = {MCTP_MSG_TAG_REQ, eid, MCTP_MSG_TYPE_PLDM};

    std::vector<uint8_t> fullMessage;
    fullMessage.insert(fullMessage.end(), std::begin(hdr), std::end(hdr));
    fullMessage.insert(fullMessage.end(), request.begin(), request.end());

    auto responseOpt = mockupResponder->processRxMsg(fullMessage);
    ASSERT_TRUE(responseOpt.has_value());

    const auto& response = responseOpt.value();

    size_t expectedSize = hdrSize + PLDM_GET_TERMINUS_UID_RESP_BYTES;
    EXPECT_EQ(response.size(), expectedSize);

    auto* responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responsePtr->hdr.instance_id, instanceID);

    uint8_t completionCode{};
    uint8_t uuid[16]{};
    rc = decode_get_terminus_UID_resp(responsePtr, response.size() - hdrSize,
                                      &completionCode, uuid);

    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_SUCCESS);

    uint8_t expectedUuid[16] = {0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11};
    EXPECT_EQ(memcmp(uuid, expectedUuid, sizeof(expectedUuid)), 0);
}

TEST_F(MockupResponderTest, testGoodGetPdrRepositoryInfo)
{
    uint8_t instanceID = 0;
    uint8_t eid = 31;

    Request request(hdrSize, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    struct pldm_header_info header;
    header.msg_type = PLDM_REQUEST;
    header.instance = instanceID;
    header.pldm_type = PLDM_PLATFORM;
    header.command = PLDM_GET_PDR_REPOSITORY_INFO;

    auto rc = pack_pldm_header(&header, &(requestMsg->hdr));
    EXPECT_EQ(rc, PLDM_SUCCESS);

    uint8_t hdr[] = {MCTP_MSG_TAG_REQ, eid, MCTP_MSG_TYPE_PLDM};

    std::vector<uint8_t> fullMessage;
    fullMessage.insert(fullMessage.end(), std::begin(hdr), std::end(hdr));
    fullMessage.insert(fullMessage.end(), request.begin(), request.end());

    auto responseOpt = mockupResponder->processRxMsg(fullMessage);
    ASSERT_TRUE(responseOpt.has_value());

    const auto& response = responseOpt.value();

    size_t expectedSize = hdrSize + PLDM_GET_PDR_REPOSITORY_INFO_RESP_BYTES;
    EXPECT_EQ(response.size(), expectedSize);

    auto* responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responsePtr->hdr.instance_id, instanceID);

    uint8_t completionCode{};
    uint8_t repositoryState{};
    uint8_t updateTime[PLDM_TIMESTAMP104_SIZE]{};
    uint8_t oemUpdateTime[PLDM_TIMESTAMP104_SIZE]{};
    uint32_t recordCount{};
    uint32_t repositorySize{};
    uint32_t largestRecordSize{};
    uint8_t dataTransferHandleTimeout{};

    rc = decode_get_pdr_repository_info_resp(
        responsePtr, response.size() - hdrSize, &completionCode,
        &repositoryState, updateTime, oemUpdateTime, &recordCount,
        &repositorySize, &largestRecordSize, &dataTransferHandleTimeout);

    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_SUCCESS);
    EXPECT_EQ(repositoryState, PLDM_AVAILABLE);
    EXPECT_EQ(recordCount, 8);
    EXPECT_EQ(repositorySize, 1024);
    EXPECT_EQ(largestRecordSize, 128);
    EXPECT_EQ(dataTransferHandleTimeout, PLDM_NO_TIMEOUT);
    EXPECT_EQ(
        std::memcmp(updateTime,
                    std::vector<uint8_t>(PLDM_TIMESTAMP104_SIZE, 0).data(),
                    PLDM_TIMESTAMP104_SIZE),
        0);
    EXPECT_EQ(
        std::memcmp(oemUpdateTime,
                    std::vector<uint8_t>(PLDM_TIMESTAMP104_SIZE, 0).data(),
                    PLDM_TIMESTAMP104_SIZE),
        0);
}

TEST_F(MockupResponderTest, testGoodGetPdr)
{
    uint8_t instanceID = 0;
    uint8_t eid = 31;

    Request request(hdrSize + PLDM_GET_PDR_REQ_BYTES, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    auto rc = encode_get_pdr_req(instanceID, 0x00, 0x00, PLDM_START, 0xFFFF, 0,
                                 requestMsg, PLDM_GET_PDR_REQ_BYTES);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    uint8_t hdr[] = {MCTP_MSG_TAG_REQ, eid, MCTP_MSG_TYPE_PLDM};

    std::vector<uint8_t> fullMessage;
    fullMessage.insert(fullMessage.end(), std::begin(hdr), std::end(hdr));
    fullMessage.insert(fullMessage.end(), request.begin(), request.end());

    auto responseOpt = mockupResponder->processRxMsg(fullMessage);
    ASSERT_TRUE(responseOpt.has_value());

    const auto& response = responseOpt.value();

    auto* responseMsg = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responseMsg->hdr.instance_id, instanceID);

    uint8_t completionCode{};
    uint32_t nextRecordHandle{};
    uint32_t dataTransferHandle{};
    uint8_t transferFlag{};
    uint16_t responseCount{};
    uint8_t* recordData = nullptr;
    uint8_t transferCrc{};

    rc = decode_get_pdr_resp(responseMsg, response.size() - hdrSize,
                             &completionCode, &nextRecordHandle,
                             &dataTransferHandle, &transferFlag, &responseCount,
                             nullptr, 0, &transferCrc);
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_SUCCESS);

    if (responseCount > 0)
    {
        recordData = new uint8_t[responseCount];
        ASSERT_NE(recordData, nullptr);

        rc = decode_get_pdr_resp(
            responseMsg, response.size() - hdrSize, &completionCode,
            &nextRecordHandle, &dataTransferHandle, &transferFlag,
            &responseCount, recordData, responseCount, &transferCrc);
        EXPECT_EQ(rc, PLDM_SUCCESS);
    }

    EXPECT_EQ(completionCode, PLDM_SUCCESS);

    delete[] recordData;
}

TEST_F(MockupResponderTest, testBadPayloadLengthGetPdr)
{
    uint8_t instanceID = 0;
    Request request(hdrSize + PLDM_GET_PDR_REQ_BYTES);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    auto rc = encode_get_pdr_req(instanceID, 0x00, 0x00, PLDM_START, 0xFFFF, 0,
                                 requestMsg, PLDM_GET_PDR_REQ_BYTES);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    auto response = mockupResponder->getPdr(
        requestMsg, PLDM_GET_PDR_REQ_BYTES + 1, mockupResponder->getPdrRepo());
    ASSERT_FALSE(response.empty());

    auto* responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    uint8_t completionCode{};
    uint32_t nextRecordHandle{};
    uint32_t dataTransferHandle{};
    uint8_t transferFlag{};
    uint16_t responseCount{};
    uint8_t transferCrc{};

    rc = decode_get_pdr_resp(responseMsg, response.size() - hdrSize,
                             &completionCode, &nextRecordHandle,
                             &dataTransferHandle, &transferFlag, &responseCount,
                             nullptr, 0, &transferCrc);
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_ERROR_INVALID_LENGTH);
}

TEST_F(MockupResponderTest, testGoodGetStateSensorReadings)
{
    uint8_t instanceID = 0;
    uint8_t eid = 31;

    Request request(hdrSize + PLDM_GET_STATE_SENSOR_READINGS_REQ_BYTES, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    uint16_t sensorId = 528;
    bitfield8_t rearm = {0x1};
    uint8_t reserved = 0;

    auto rc = encode_get_state_sensor_readings_req(instanceID, sensorId, rearm,
                                                   reserved, requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    uint8_t hdr[] = {MCTP_MSG_TAG_REQ, eid, MCTP_MSG_TYPE_PLDM};

    std::vector<uint8_t> fullMessage;
    fullMessage.insert(fullMessage.end(), std::begin(hdr), std::end(hdr));
    fullMessage.insert(fullMessage.end(), request.begin(), request.end());

    auto responseOpt = mockupResponder->processRxMsg(fullMessage);
    ASSERT_TRUE(responseOpt.has_value());

    const auto& response = responseOpt.value();

    auto* responseMsg = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responseMsg->hdr.instance_id, instanceID);

    uint8_t completionCode{};
    uint8_t compositeSensorCount{};
    std::vector<get_sensor_state_field> stateFields(8);

    rc = decode_get_state_sensor_readings_resp(
        responseMsg, response.size() - hdrSize, &completionCode,
        &compositeSensorCount, stateFields.data());
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_SUCCESS);

    ASSERT_EQ(compositeSensorCount, 1);

    for (uint8_t i = 0; i < compositeSensorCount; ++i)
    {
        const auto& field = stateFields[i];
        EXPECT_EQ(field.sensor_op_state, PLDM_SENSOR_ENABLED);
        EXPECT_EQ(field.previous_state, 0);
        EXPECT_EQ(field.present_state, 0);
        EXPECT_EQ(field.event_state, 0);
    }
}

TEST_F(MockupResponderTest, testBadSensorIDGetStateSensorReadings)
{
    uint8_t instanceID = 0;
    uint8_t eid = 31;

    Request request(hdrSize + PLDM_GET_STATE_SENSOR_READINGS_REQ_BYTES, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    uint16_t sensorId = 529;
    bitfield8_t rearm = {0x1};
    uint8_t reserved = 0;

    auto rc = encode_get_state_sensor_readings_req(instanceID, sensorId, rearm,
                                                   reserved, requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    uint8_t hdr[] = {MCTP_MSG_TAG_REQ, eid, MCTP_MSG_TYPE_PLDM};

    std::vector<uint8_t> fullMessage;
    fullMessage.insert(fullMessage.end(), std::begin(hdr), std::end(hdr));
    fullMessage.insert(fullMessage.end(), request.begin(), request.end());

    auto responseOpt = mockupResponder->processRxMsg(fullMessage);
    ASSERT_TRUE(responseOpt.has_value());

    const auto& response = responseOpt.value();

    auto* responseMsg = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responseMsg->hdr.instance_id, instanceID);

    uint8_t completionCode{};
    uint8_t compositeSensorCount{};
    std::vector<get_sensor_state_field> stateFields(8);

    rc = decode_get_state_sensor_readings_resp(
        responseMsg, response.size() - hdrSize, &completionCode,
        &compositeSensorCount, stateFields.data());
    EXPECT_EQ(rc, PLDM_SUCCESS);

    EXPECT_EQ(completionCode, PLDM_PLATFORM_INVALID_SENSOR_ID);
}

TEST_F(MockupResponderTest, testBadPayloadLengthGetStateSensorReadings)
{
    uint8_t instanceID = 0;
    Request request(hdrSize + PLDM_GET_STATE_SENSOR_READINGS_REQ_BYTES);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    uint16_t sensorId = 528;
    bitfield8_t rearm = {0x1};
    uint8_t reserved = 0;

    auto rc = encode_get_state_sensor_readings_req(instanceID, sensorId, rearm,
                                                   reserved, requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    auto response = mockupResponder->getStateSensorReadings(
        requestMsg, PLDM_GET_STATE_SENSOR_READINGS_REQ_BYTES + 1);
    ASSERT_FALSE(response.empty());

    auto* responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    uint8_t completionCode{};
    uint8_t compositeSensorCount{};
    std::vector<get_sensor_state_field> stateFields(8);

    rc = decode_get_state_sensor_readings_resp(
        responseMsg, response.size() - hdrSize, &completionCode,
        &compositeSensorCount, stateFields.data());
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_ERROR_INVALID_LENGTH);
}

TEST_F(MockupResponderTest, testGoodGetNumericEffecterValue)
{
    uint8_t instanceID = 0;
    uint8_t eid = 31;

    Request request(hdrSize + PLDM_GET_NUMERIC_EFFECTER_VALUE_REQ_BYTES, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    uint16_t effecterId = 0x0800;

    auto rc = encode_get_numeric_effecter_value_req(instanceID, effecterId,
                                                    requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    uint8_t hdr[] = {MCTP_MSG_TAG_REQ, eid, MCTP_MSG_TYPE_PLDM};

    std::vector<uint8_t> fullMessage;
    fullMessage.insert(fullMessage.end(), std::begin(hdr), std::end(hdr));
    fullMessage.insert(fullMessage.end(), request.begin(), request.end());

    auto responseOpt = mockupResponder->processRxMsg(fullMessage);
    ASSERT_TRUE(responseOpt.has_value());

    const auto& response = responseOpt.value();

    auto* responseMsg = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responseMsg->hdr.instance_id, instanceID);

    uint8_t completionCode{};
    uint8_t effecterDataSize{};
    uint8_t effecterOperationalState{};
    uint32_t pendingValue{};
    uint32_t presentValue{};

    rc = decode_get_numeric_effecter_value_resp(
        responseMsg, response.size() - hdrSize, &completionCode,
        &effecterDataSize, &effecterOperationalState,
        reinterpret_cast<uint8_t*>(&pendingValue),
        reinterpret_cast<uint8_t*>(&presentValue));
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_SUCCESS);

    EXPECT_EQ(effecterDataSize, 4);
    EXPECT_EQ(effecterOperationalState, 1);
}

TEST_F(MockupResponderTest, testBadEffecterIDGetNumericEffecterValue)
{
    uint8_t instanceID = 0;
    uint8_t eid = 31;

    Request request(hdrSize + PLDM_GET_NUMERIC_EFFECTER_VALUE_REQ_BYTES, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    uint16_t effecterId = 0x0801;

    auto rc = encode_get_numeric_effecter_value_req(instanceID, effecterId,
                                                    requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    uint8_t hdr[] = {MCTP_MSG_TAG_REQ, eid, MCTP_MSG_TYPE_PLDM};

    std::vector<uint8_t> fullMessage;
    fullMessage.insert(fullMessage.end(), std::begin(hdr), std::end(hdr));
    fullMessage.insert(fullMessage.end(), request.begin(), request.end());

    auto responseOpt = mockupResponder->processRxMsg(fullMessage);
    ASSERT_TRUE(responseOpt.has_value());

    const auto& response = responseOpt.value();

    auto* responseMsg = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responseMsg->hdr.instance_id, instanceID);

    uint8_t completionCode{};
    uint8_t effecterDataSize{};
    uint8_t effecterOperationalState{};
    uint32_t pendingValue{};
    uint32_t presentValue{};

    rc = decode_get_numeric_effecter_value_resp(
        responseMsg, response.size() - hdrSize, &completionCode,
        &effecterDataSize, &effecterOperationalState,
        reinterpret_cast<uint8_t*>(&pendingValue),
        reinterpret_cast<uint8_t*>(&presentValue));
    EXPECT_EQ(rc, PLDM_SUCCESS);

    EXPECT_EQ(completionCode, PLDM_PLATFORM_INVALID_EFFECTER_ID);
}

TEST_F(MockupResponderTest, testBadPayloadLengthGetNumericEffecterValue)
{
    uint8_t instanceID = 0;
    Request request(hdrSize + PLDM_GET_NUMERIC_EFFECTER_VALUE_REQ_BYTES);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    uint16_t effecterId = 0x0800;

    auto rc = encode_get_numeric_effecter_value_req(instanceID, effecterId,
                                                    requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    auto response = mockupResponder->getNumericEffecterValue(
        requestMsg, PLDM_GET_NUMERIC_EFFECTER_VALUE_REQ_BYTES + 1);
    ASSERT_FALSE(response.empty());

    auto* responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    uint8_t completionCode{};
    uint8_t effecterDataSize{};
    uint8_t effecterOperationalState{};
    uint32_t pendingValue{};
    uint32_t presentValue{};

    rc = decode_get_numeric_effecter_value_resp(
        responseMsg, response.size() - hdrSize, &completionCode,
        &effecterDataSize, &effecterOperationalState,
        reinterpret_cast<uint8_t*>(&pendingValue),
        reinterpret_cast<uint8_t*>(&presentValue));
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_ERROR_INVALID_LENGTH);
}

TEST_F(MockupResponderTest, testGoodGetStateEffecterStates)
{
    uint8_t instanceID = 0;
    uint8_t eid = 31;

    Request request(hdrSize + PLDM_GET_STATE_EFFECTER_STATES_REQ_BYTES, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    uint16_t effecterId = 2028;
    auto rc = encode_get_state_effecter_states_req(instanceID, effecterId,
                                                   requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    uint8_t hdr[] = {MCTP_MSG_TAG_REQ, eid, MCTP_MSG_TYPE_PLDM};

    std::vector<uint8_t> fullMessage;
    fullMessage.insert(fullMessage.end(), std::begin(hdr), std::end(hdr));
    fullMessage.insert(fullMessage.end(), request.begin(), request.end());

    auto responseOpt = mockupResponder->processRxMsg(fullMessage);
    ASSERT_TRUE(responseOpt.has_value());

    const auto& response = responseOpt.value();

    auto* responseMsg = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responseMsg->hdr.instance_id, instanceID);

    uint8_t completionCode{};
    uint8_t compEffecterCount{};
    get_effecter_state_field stateFields[8];

    rc = decode_get_state_effecter_states_resp(
        responseMsg, response.size() - hdrSize, &completionCode,
        &compEffecterCount, stateFields);
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_SUCCESS);

    EXPECT_EQ(compEffecterCount, 1);
    EXPECT_EQ(stateFields[0].effecter_op_state,
              EFFECTER_OPER_STATE_ENABLED_NOUPDATEPENDING);
    EXPECT_EQ(stateFields[0].pending_state, 0);
    EXPECT_EQ(stateFields[0].present_state, 0);
}

TEST_F(MockupResponderTest, testBadEffecterIDGetStateEffecterStates)
{
    uint8_t instanceID = 0;
    uint8_t eid = 31;

    Request request(hdrSize + PLDM_GET_STATE_EFFECTER_STATES_REQ_BYTES, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    uint16_t effecterId = 2029;
    auto rc = encode_get_state_effecter_states_req(instanceID, effecterId,
                                                   requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    uint8_t hdr[] = {MCTP_MSG_TAG_REQ, eid, MCTP_MSG_TYPE_PLDM};

    std::vector<uint8_t> fullMessage;
    fullMessage.insert(fullMessage.end(), std::begin(hdr), std::end(hdr));
    fullMessage.insert(fullMessage.end(), request.begin(), request.end());

    auto responseOpt = mockupResponder->processRxMsg(fullMessage);
    ASSERT_TRUE(responseOpt.has_value());

    const auto& response = responseOpt.value();

    auto* responseMsg = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responseMsg->hdr.instance_id, instanceID);

    uint8_t completionCode{};
    uint8_t compEffecterCount{};
    get_effecter_state_field stateFields[8];

    rc = decode_get_state_effecter_states_resp(
        responseMsg, response.size() - hdrSize, &completionCode,
        &compEffecterCount, stateFields);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    EXPECT_EQ(completionCode, PLDM_PLATFORM_INVALID_EFFECTER_ID);
}

TEST_F(MockupResponderTest, testBadPayloadLengthGetStateEffecterStates)
{
    uint8_t instanceID = 0;
    Request request(hdrSize + PLDM_GET_STATE_EFFECTER_STATES_REQ_BYTES);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    uint16_t effecterId = 2028;
    auto rc = encode_get_state_effecter_states_req(instanceID, effecterId,
                                                   requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    auto response = mockupResponder->getStateEffecterStates(
        requestMsg, PLDM_GET_STATE_EFFECTER_STATES_REQ_BYTES + 1);
    ASSERT_FALSE(response.empty());

    auto* responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    uint8_t completionCode{};
    uint8_t compEffecterCount{};
    get_effecter_state_field stateFields[8];

    rc = decode_get_state_effecter_states_resp(
        responseMsg, response.size() - hdrSize, &completionCode,
        &compEffecterCount, stateFields);
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_ERROR_INVALID_LENGTH);
}

TEST_F(MockupResponderTest, testGoodGetSensorReading)
{
    uint8_t instanceID = 0;
    uint8_t eid = 31;

    Request request(hdrSize + PLDM_GET_SENSOR_READING_REQ_BYTES, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    uint16_t sensorId = 20;
    bool8_t rearm = false;

    auto rc =
        encode_get_sensor_reading_req(instanceID, sensorId, rearm, requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    uint8_t hdr[] = {MCTP_MSG_TAG_REQ, eid, MCTP_MSG_TYPE_PLDM};

    std::vector<uint8_t> fullMessage;
    fullMessage.insert(fullMessage.end(), std::begin(hdr), std::end(hdr));
    fullMessage.insert(fullMessage.end(), request.begin(), request.end());

    auto responseOpt = mockupResponder->processRxMsg(fullMessage);
    ASSERT_TRUE(responseOpt.has_value());

    const auto& response = responseOpt.value();

    auto* responseMsg = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responseMsg->hdr.instance_id, instanceID);

    uint8_t completionCode{};
    uint8_t sensorDataSize = PLDM_SENSOR_DATA_SIZE_UINT32;
    uint8_t sensorOperationalState{};
    uint8_t sensorEventMessageEnable{};
    uint8_t presentState{};
    uint8_t previousState{};
    uint8_t eventState{};
    uint8_t presentReading[4]{};

    rc = decode_get_sensor_reading_resp(
        responseMsg, response.size() - hdrSize, &completionCode,
        &sensorDataSize, &sensorOperationalState, &sensorEventMessageEnable,
        &presentState, &previousState, &eventState, presentReading);
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_SUCCESS);

    EXPECT_EQ(sensorDataSize, PLDM_EFFECTER_DATA_SIZE_UINT32);
    EXPECT_EQ(sensorOperationalState, PLDM_SENSOR_ENABLED);
    EXPECT_EQ(sensorEventMessageEnable, PLDM_NO_EVENT_GENERATION);
    EXPECT_EQ(presentState, PLDM_SENSOR_NORMAL);
    EXPECT_EQ(previousState, PLDM_SENSOR_NORMAL);
    EXPECT_EQ(eventState, PLDM_SENSOR_NORMAL);

    uint32_t decodedPresentReading =
        *reinterpret_cast<const uint32_t*>(presentReading);
    EXPECT_EQ(decodedPresentReading, 0);
}

TEST_F(MockupResponderTest, testBadSensorIDGetSensorReading)
{
    uint8_t instanceID = 0;
    uint8_t eid = 31;

    Request request(hdrSize + PLDM_GET_SENSOR_READING_REQ_BYTES, 0);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    uint16_t sensorId = 200;
    bool8_t rearm = false;

    auto rc =
        encode_get_sensor_reading_req(instanceID, sensorId, rearm, requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    uint8_t hdr[] = {MCTP_MSG_TAG_REQ, eid, MCTP_MSG_TYPE_PLDM};

    std::vector<uint8_t> fullMessage;
    fullMessage.insert(fullMessage.end(), std::begin(hdr), std::end(hdr));
    fullMessage.insert(fullMessage.end(), request.begin(), request.end());

    auto responseOpt = mockupResponder->processRxMsg(fullMessage);
    ASSERT_TRUE(responseOpt.has_value());

    const auto& response = responseOpt.value();

    auto* responseMsg = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responseMsg->hdr.instance_id, instanceID);

    uint8_t completionCode{};
    uint8_t sensorDataSize{};
    uint8_t sensorOperationalState{};
    uint8_t sensorEventMessageEnable{};
    uint8_t presentState{};
    uint8_t previousState{};
    uint8_t eventState{};
    uint8_t presentReading[4]{};

    rc = decode_get_sensor_reading_resp(
        responseMsg, response.size() - hdrSize, &completionCode,
        &sensorDataSize, &sensorOperationalState, &sensorEventMessageEnable,
        &presentState, &previousState, &eventState, presentReading);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    EXPECT_EQ(completionCode, PLDM_PLATFORM_INVALID_SENSOR_ID);
}

TEST_F(MockupResponderTest, testBadPayloadLengthGetSensorReading)
{
    uint8_t instanceID = 0;
    Request request(hdrSize + PLDM_GET_SENSOR_READING_REQ_BYTES);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    uint16_t sensorId = 20;
    bool8_t rearm = false;

    auto rc =
        encode_get_sensor_reading_req(instanceID, sensorId, rearm, requestMsg);
    EXPECT_EQ(rc, PLDM_SUCCESS);

    auto response = mockupResponder->getSensorReading(
        requestMsg, PLDM_GET_SENSOR_READING_REQ_BYTES + 1, nullptr);
    ASSERT_FALSE(response.empty());

    auto* responseMsg = reinterpret_cast<pldm_msg*>(response.data());
    uint8_t completionCode{};
    uint8_t sensorDataSize{};
    uint8_t sensorOperationalState{};
    uint8_t sensorEventMessageEnable{};
    uint8_t presentState{};
    uint8_t previousState{};
    uint8_t eventState{};
    uint8_t presentReading[4]{};

    rc = decode_get_sensor_reading_resp(
        responseMsg, response.size() - hdrSize, &completionCode,
        &sensorDataSize, &sensorOperationalState, &sensorEventMessageEnable,
        &presentState, &previousState, &eventState, presentReading);
    EXPECT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_ERROR_INVALID_LENGTH);
}

TEST_F(MockupResponderTest, testGoodProcessRxMsg)
{
    uint8_t instanceId = 0;
    uint8_t eid = 31;
    Request request(hdrSize);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    auto rc = encode_get_types_req(instanceId, requestMsg);
    ASSERT_EQ(rc, PLDM_SUCCESS);

    uint8_t hdr[] = {MCTP_MSG_TAG_REQ, eid, MCTP_MSG_TYPE_PLDM};

    std::vector<uint8_t> fullMessage;
    fullMessage.insert(fullMessage.end(), std::begin(hdr), std::end(hdr));
    fullMessage.insert(fullMessage.end(), request.begin(), request.end());

    auto responseOpt = mockupResponder->processRxMsg(fullMessage);
    ASSERT_TRUE(responseOpt.has_value());

    const auto& response = responseOpt.value();

    EXPECT_EQ(response.size(), hdrSize + PLDM_GET_TYPES_RESP_BYTES);

    auto* responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responsePtr->hdr.instance_id, instanceId);

    uint8_t completionCode{};
    std::array<bitfield8_t, 8> types{};
    rc = decode_get_types_resp(responsePtr, response.size() - hdrSize,
                               &completionCode, types.data());
    ASSERT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_SUCCESS);

    for (const auto& type : capabilities)
    {
        auto index = type.first / 8;
        auto bit = type.first % 8;
        EXPECT_TRUE(types[index].byte & (1 << bit));
    }
}

TEST_F(MockupResponderTest, testGoodUnsupportedCommandHandler)
{
    uint8_t instanceId = 0;
    uint8_t eid = 31;
    Request request(hdrSize);
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());

    struct pldm_header_info header;
    header.instance = instanceId;
    header.msg_type = PLDM_REQUEST;

    // GET_EVENT_RECEIVER = 0x05,
    header.command = 0x05;
    header.pldm_type = PLDM_PLATFORM;

    auto rc = pack_pldm_header(&header, &(requestMsg->hdr));
    ASSERT_EQ(rc, PLDM_SUCCESS);

    uint8_t hdr[] = {MCTP_MSG_TAG_REQ, eid, MCTP_MSG_TYPE_PLDM};

    std::vector<uint8_t> fullMessage;
    fullMessage.insert(fullMessage.end(), std::begin(hdr), std::end(hdr));
    fullMessage.insert(fullMessage.end(), request.begin(), request.end());

    auto responseOpt = mockupResponder->processRxMsg(fullMessage);
    ASSERT_TRUE(responseOpt.has_value());

    const auto& response = responseOpt.value();

    EXPECT_EQ(response.size(), hdrSize + 1);

    auto* responsePtr = reinterpret_cast<const pldm_msg*>(response.data());
    EXPECT_EQ(responsePtr->hdr.instance_id, instanceId);

    uint8_t completionCode{};
    rc = decode_cc_only_resp(responsePtr, response.size() - hdrSize,
                             &completionCode);
    ASSERT_EQ(rc, PLDM_SUCCESS);
    EXPECT_EQ(completionCode, PLDM_ERROR_UNSUPPORTED_PLDM_CMD);
}

TEST_F(MockupResponderTest, testGoodHandlingResponse)
{
    uint8_t instanceId = 0;
    uint8_t endpointId = 31;
    Request responseMessage(hdrSize + 1);
    auto responseMsgPtr = reinterpret_cast<pldm_msg*>(responseMessage.data());
    uint8_t completionCode = PLDM_SUCCESS;

    auto encodeRc = encode_set_numeric_effecter_value_resp(
        instanceId, completionCode, responseMsgPtr,
        PLDM_SET_NUMERIC_EFFECTER_VALUE_RESP_BYTES);
    ASSERT_EQ(encodeRc, PLDM_SUCCESS);

    uint8_t mctpHeader[] = {MCTP_MSG_TAG_RESP, endpointId, MCTP_MSG_TYPE_PLDM};

    std::vector<uint8_t> fullResponseMessage;
    fullResponseMessage.insert(fullResponseMessage.end(),
                               std::begin(mctpHeader), std::end(mctpHeader));
    fullResponseMessage.insert(fullResponseMessage.end(),
                               responseMessage.begin(), responseMessage.end());

    auto responseOpt = mockupResponder->processRxMsg(fullResponseMessage);
    ASSERT_TRUE(responseOpt.has_value());

    const auto& processedResponse = responseOpt.value();

    EXPECT_EQ(processedResponse.size(), hdrSize + 1);

    auto* processedResponsePtr =
        reinterpret_cast<const pldm_msg*>(processedResponse.data());
    EXPECT_EQ(processedResponsePtr->hdr.instance_id, instanceId);

    uint8_t decodedCompletionCode{};
    auto decodeRc = decode_cc_only_resp(processedResponsePtr,
                                        processedResponse.size() - hdrSize,
                                        &decodedCompletionCode);

    ASSERT_EQ(decodeRc, PLDM_SUCCESS);
    EXPECT_EQ(decodedCompletionCode, PLDM_ERROR_UNSUPPORTED_PLDM_CMD);
}
