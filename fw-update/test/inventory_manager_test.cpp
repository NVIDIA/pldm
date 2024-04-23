/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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

#include "common/utils.hpp"
#include "fw-update/inventory_manager.hpp"
#include "requester/test/mock_request.hpp"

#include <gtest/gtest.h>

using namespace pldm;
using namespace std::chrono;
using namespace pldm::fw_update;

class InventoryManagerTest : public testing::Test
{
  protected:
    InventoryManagerTest() :
        event(sdeventplus::Event::get_default()),
        dbusImplRequester(pldm::utils::DBusHandler::getBus(),
                          "/xyz/openbmc_project/pldm"),
        reqHandler(event, dbusImplRequester, sockManager, false, seconds(1), 2,
                   milliseconds(100)),
        inventoryManager(reqHandler, dbusImplRequester, nullptr,
                         outDescriptorMap, outComponentInfoMap,
                         deviceInventoryInfo)
    {}

    sdeventplus::Event event;
    pldm::dbus_api::Requester dbusImplRequester;
    pldm::mctp_socket::Manager sockManager;
    requester::Handler<requester::Request> reqHandler;
    InventoryManager inventoryManager;
    DescriptorMap outDescriptorMap{};
    ComponentInfoMap outComponentInfoMap{};
    DeviceInventoryInfo deviceInventoryInfo{};
    std::string messageError;
    std::string resolution;
};

TEST_F(InventoryManagerTest, handleQueryDeviceIdentifiersResponse)
{
    constexpr size_t respPayloadLength1 = 49;
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + respPayloadLength1>
        queryDeviceIdentifiersResp1{
            0x00, 0x00, 0x00, 0x00, 0x2b, 0x00, 0x00, 0x00, 0x03, 0x01, 0x00,
            0x04, 0x00, 0x0a, 0x0b, 0x0c, 0x0d, 0x02, 0x00, 0x10, 0x00, 0x12,
            0x44, 0xd2, 0x64, 0x8d, 0x7d, 0x47, 0x18, 0xa0, 0x30, 0xfc, 0x8a,
            0x56, 0x58, 0x7d, 0x5b, 0xFF, 0xFF, 0x0B, 0x00, 0x01, 0x07, 0x4f,
            0x70, 0x65, 0x6e, 0x42, 0x4d, 0x43, 0x01, 0x02};
    auto responseMsg1 =
        reinterpret_cast<const pldm_msg*>(queryDeviceIdentifiersResp1.data());
    inventoryManager.parseQueryDeviceIdentifiersResponse(
        1, responseMsg1, respPayloadLength1, messageError, resolution);

    DescriptorMap descriptorMap1{
        {0x01,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x12, 0x44, 0xd2, 0x64, 0x8d, 0x7d, 0x47, 0x18,
                                0xa0, 0x30, 0xfc, 0x8a, 0x56, 0x58, 0x7d,
                                0x5b}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x01, 0x02})}}}};

    EXPECT_EQ(outDescriptorMap.size(), descriptorMap1.size());
    EXPECT_EQ(outDescriptorMap, descriptorMap1);

    constexpr size_t respPayloadLength2 = 26;
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + respPayloadLength2>
        queryDeviceIdentifiersResp2{
            0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x01, 0x02,
            0x00, 0x10, 0x00, 0xF0, 0x18, 0x87, 0x8C, 0xCB, 0x7D, 0x49,
            0x43, 0x98, 0x00, 0xA0, 0x2F, 0x59, 0x9A, 0xCA, 0x02};
    auto responseMsg2 =
        reinterpret_cast<const pldm_msg*>(queryDeviceIdentifiersResp2.data());
    inventoryManager.parseQueryDeviceIdentifiersResponse(
        2, responseMsg2, respPayloadLength2, messageError, resolution);
    DescriptorMap descriptorMap2{
        {0x01,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x12, 0x44, 0xd2, 0x64, 0x8d, 0x7d, 0x47, 0x18,
                                0xa0, 0x30, 0xfc, 0x8a, 0x56, 0x58, 0x7d,
                                0x5b}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x01, 0x02})}}},
        {0x02,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0xF0, 0x18, 0x87, 0x8C, 0xCB, 0x7D, 0x49, 0x43,
                                0x98, 0x00, 0xA0, 0x2F, 0x59, 0x9A, 0xCA,
                                0x02}}}}};
    EXPECT_EQ(outDescriptorMap.size(), descriptorMap2.size());
    EXPECT_EQ(outDescriptorMap, descriptorMap2);
}

TEST_F(InventoryManagerTest, handleQueryDeviceIdentifiersResponseErrorCC)
{
    constexpr size_t respPayloadLength = 1;
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + respPayloadLength>
        queryDeviceIdentifiersResp{0x00, 0x00, 0x00, 0x01};
    auto responseMsg =
        reinterpret_cast<const pldm_msg*>(queryDeviceIdentifiersResp.data());
    inventoryManager.parseQueryDeviceIdentifiersResponse(
        1, responseMsg, respPayloadLength, messageError, resolution);
    EXPECT_EQ(outDescriptorMap.size(), 0);
}

TEST_F(InventoryManagerTest, getFirmwareParametersResponse)
{
    // constexpr uint16_t compCount = 2;
    // constexpr std::string_view activeCompImageSetVersion{"DeviceVer1.0"};
    const std::string activeCompVersion1{"Comp1v2.0"};
    const std::string activeCompVersion2{"Comp2v3.0"};
    constexpr uint16_t compClassification1 = 10;
    constexpr uint16_t compIdentifier1 = 300;
    constexpr uint8_t compClassificationIndex1 = 20;
    constexpr uint16_t compClassification2 = 16;
    constexpr uint16_t compIdentifier2 = 301;
    constexpr uint8_t compClassificationIndex2 = 30;

    constexpr size_t respPayloadLength1 = 119;
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + respPayloadLength1>
        getFirmwareParametersResp1{
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01,
            0x0c, 0x00, 0x00, 0x44, 0x65, 0x76, 0x69, 0x63, 0x65, 0x56, 0x65,
            0x72, 0x31, 0x2e, 0x30, 0x0a, 0x00, 0x2c, 0x01, 0x14, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x43,
            0x6f, 0x6d, 0x70, 0x31, 0x76, 0x32, 0x2e, 0x30, 0x10, 0x00, 0x2d,
            0x01, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x01, 0x09, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x43, 0x6f, 0x6d, 0x70, 0x32, 0x76, 0x33, 0x2e,
            0x30};
    auto responseMsg1 =
        reinterpret_cast<const pldm_msg*>(getFirmwareParametersResp1.data());
    dbus::MctpInterfaces mctpInterfaces;
    inventoryManager.parseGetFWParametersResponse(
        1, responseMsg1, respPayloadLength1, messageError, resolution, mctpInterfaces);

    ComponentInfoMap componentInfoMap1{
        {1,
         {{std::make_pair(compClassification1, compIdentifier1),
           std::make_tuple(compClassificationIndex1, activeCompVersion1)},
          {std::make_pair(compClassification2, compIdentifier2),
           std::make_tuple(compClassificationIndex2, activeCompVersion2)}}}};
    EXPECT_EQ(outComponentInfoMap.size(), componentInfoMap1.size());
    EXPECT_EQ(outComponentInfoMap, componentInfoMap1);

    // constexpr uint16_t compCount = 1;
    constexpr std::string_view activeCompImageSetVersion{"DeviceVer2.0"};
    const std::string activeCompVersion3{"Comp3v4.0"};
    constexpr uint16_t compClassification3 = 2;
    constexpr uint16_t compIdentifier3 = 302;
    constexpr uint8_t compClassificationIndex3 = 40;

    constexpr size_t respPayloadLength2 = 119;
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + respPayloadLength2>
        getFirmwareParametersResp2{
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01,
            0x0c, 0x00, 0x00, 0x44, 0x65, 0x76, 0x69, 0x63, 0x65, 0x56, 0x65,
            0x72, 0x32, 0x2e, 0x30, 0x02, 0x00, 0x2e, 0x01, 0x28, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x43,
            0x6f, 0x6d, 0x70, 0x33, 0x76, 0x34, 0x2e, 0x30};
    auto responseMsg2 =
        reinterpret_cast<const pldm_msg*>(getFirmwareParametersResp2.data());
    inventoryManager.parseGetFWParametersResponse(
        2, responseMsg2, respPayloadLength2, messageError, resolution, mctpInterfaces);

    ComponentInfoMap componentInfoMap2{
        {1,
         {{std::make_pair(compClassification1, compIdentifier1),
           std::make_tuple(compClassificationIndex1, activeCompVersion1)},
          {std::make_pair(compClassification2, compIdentifier2),
           std::make_tuple(compClassificationIndex2, activeCompVersion2)}}},
        {2,
         {{std::make_pair(compClassification3, compIdentifier3),
           std::make_tuple(compClassificationIndex3, activeCompVersion3)}}}};
    EXPECT_EQ(outComponentInfoMap.size(), componentInfoMap2.size());
    EXPECT_EQ(outComponentInfoMap, componentInfoMap2);
}

TEST_F(InventoryManagerTest, getFirmwareParametersResponseErrorCC)
{
    constexpr size_t respPayloadLength = 1;
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + respPayloadLength>
        getFirmwareParametersResp{0x00, 0x00, 0x00, 0x01};
    auto responseMsg =
        reinterpret_cast<const pldm_msg*>(getFirmwareParametersResp.data());
    dbus::MctpInterfaces mctpInterfaces;
    inventoryManager.parseGetFWParametersResponse(
        1, responseMsg, respPayloadLength, messageError, resolution, mctpInterfaces);
    EXPECT_EQ(outComponentInfoMap.size(), 0);
}

TEST_F(InventoryManagerTest, MultipleIdSameTypeIdentifiers)
{
    constexpr size_t respPayloadLength1 = 68;
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + respPayloadLength1>
        queryDeviceIdentifiersResp1{
            0x00, 0x00, 0x00, 0x00, 0x3E, 0x00, 0x00, 0x00, 0x05, 0x01, 0x00,
            0x04, 0x00, 0x0a, 0x0b, 0x0c, 0x0d, 0x01, 0x00, 0x04, 0x00, 0x0a,
            0x0b, 0x0c, 0x0e, 0x02, 0x00, 0x10, 0x00, 0x12, 0x44, 0xd2, 0x64,
            0x8d, 0x7d, 0x47, 0x18, 0xa0, 0x30, 0xfc, 0x8a, 0x56, 0x58, 0x7d,
            0x5b, 0xFF, 0xFF, 0x0B, 0x00, 0x01, 0x07, 0x4f, 0x70, 0x65, 0x6e,
            0x42, 0x4d, 0x43, 0x01, 0x02, 0xFF, 0xFF, 0x07, 0x00, 0x01, 0x03,
            0x53, 0x4B, 0x55, 0x01, 0x03};
    auto responseMsg1 =
        reinterpret_cast<const pldm_msg*>(queryDeviceIdentifiersResp1.data());
    inventoryManager.parseQueryDeviceIdentifiersResponse(1, responseMsg1,
                                            respPayloadLength1, messageError, resolution);

    DescriptorMap descriptorMap1{
        {0x01,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0x0d}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0x0e}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x12, 0x44, 0xd2, 0x64, 0x8d, 0x7d, 0x47, 0x18,
                                0xa0, 0x30, 0xfc, 0x8a, 0x56, 0x58, 0x7d,
                                0x5b}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x01, 0x02})},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("SKU", std::vector<uint8_t>{0x01, 0x03})}}}};

    EXPECT_EQ(outDescriptorMap.size(), descriptorMap1.size());
    EXPECT_EQ(outDescriptorMap, descriptorMap1);
}

TEST_F(InventoryManagerTest, MultipleIdSameTypeInvalidIdentifiers)
{
    constexpr size_t respPayloadLength1 = 49;
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + respPayloadLength1>
        queryDeviceIdentifiersResp1{
            0x00, 0x00, 0x00, 0x00, 0x2b, 0x00, 0x00, 0x00, 0x03, 0x01, 0x00,
            0x04, 0x00, 0x0a, 0x0b, 0x0c, 0x0d, 0x02, 0x00, 0x10, 0x00, 0x12,
            0x44, 0xd2, 0x64, 0x8d, 0x7d, 0x47, 0x18, 0xa0, 0x30, 0xfc, 0x8a,
            0x56, 0x58, 0x7d, 0x5b, 0xFF, 0xFF, 0x0B, 0x00, 0x01, 0x07, 0x4f,
            0x70, 0x65, 0x6e, 0x42, 0x4d, 0x43, 0x01, 0x02};
    auto responseMsg1 =
        reinterpret_cast<const pldm_msg*>(queryDeviceIdentifiersResp1.data());
    inventoryManager.parseQueryDeviceIdentifiersResponse(1, responseMsg1,
                                            respPayloadLength1, messageError, resolution);

    DescriptorMap descriptorMap1{
        {0x01,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x12, 0x44, 0xd2, 0x64, 0x8d, 0x7d, 0x47, 0x18,
                                0xa0, 0x30, 0xfc, 0x8a, 0x56, 0x58, 0x7d,
                                0x5b}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x12, 0x44, 0xd2, 0x64, 0x8d, 0x7d}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x01, 0x02})}}}};

    EXPECT_EQ(outDescriptorMap.size(), descriptorMap1.size());
    EXPECT_NE(outDescriptorMap, descriptorMap1);

    constexpr size_t respPayloadLength2 = 26;
    constexpr std::array<uint8_t, sizeof(pldm_msg_hdr) + respPayloadLength2>
        queryDeviceIdentifiersResp2{
            0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x01, 0x02,
            0x00, 0x10, 0x00, 0xF0, 0x18, 0x87, 0x8C, 0xCB, 0x7D, 0x49,
            0x43, 0x98, 0x00, 0xA0, 0x2F, 0x59, 0x9A, 0xCA, 0x02};
    auto responseMsg2 =
        reinterpret_cast<const pldm_msg*>(queryDeviceIdentifiersResp2.data());
    inventoryManager.parseQueryDeviceIdentifiersResponse(2, responseMsg2,
                                            respPayloadLength2, messageError, resolution);
    DescriptorMap descriptorMap2{
        {0x01,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x0a, 0x0b, 0x0c, 0xd}},
          {PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x12, 0x44, 0xd2, 0x64, 0x8d, 0x7d, 0x47, 0x18,
                                0xa0, 0x30, 0xfc, 0x8a, 0x56, 0x58, 0x7d,
                                0x5b}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("SKU", std::vector<uint8_t>{0x12, 0x34, 0x56})},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("OpenBMC", std::vector<uint8_t>{0x01, 0x02})}}},
        {0x02,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0xF0, 0x18, 0x87, 0x8C, 0xCB, 0x7D, 0x49, 0x43,
                                0x98, 0x00, 0xA0, 0x2F, 0x59, 0x9A, 0xCA,
                                0x02}}}}};
    EXPECT_EQ(outDescriptorMap.size(), descriptorMap2.size());
    EXPECT_NE(outDescriptorMap, descriptorMap2);
}