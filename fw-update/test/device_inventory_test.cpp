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

#include "common/test/mocked_utils.hpp"
#include "common/types.hpp"
#include "fw-update/device_inventory.hpp"

#include <sdbusplus/bus.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldm;
using namespace pldm::fw_update;
using namespace pldm::fw_update::device_inventory;

using ::testing::IsNull;
using ::testing::StrEq;

TEST(Entry, Basic)
{
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};
    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const Associations assocs{};
    const std::string sku{};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(1);

    Entry entry(busMock, objPath, uuid, assocs, sku);
}

TEST(Manager, SingleMatchForECSKU)
{
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};
    DeviceInventoryInfo deviceInventoryInfo({
        {{"xyz.openbmc_project.Common.UUID", {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130003"}}},
         {{objPath,
           {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"}}},
          {}}}});
    const EID eid = 1;
    const DescriptorMap descriptorMap{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("ECSKU",
                           std::vector<uint8_t>{0x49, 0x35, 0x36, 0x81})}}}};
    dbus::MctpInterfaces mctpInterfaces{{uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130003"}}}}}};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(1);
    MockdBusHandler dbusHandler;
    Manager manager(busMock, deviceInventoryInfo, descriptorMap, &dbusHandler);
    EXPECT_EQ(manager.createEntry(eid, uuid, mctpInterfaces), objPath);
}

TEST(Manager, SingleMatchForAPSKU)
{
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};
    DeviceInventoryInfo deviceInventoryInfo({
        {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}},
         {{objPath,
           {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"}}},
          {}}}});
    const EID eid = 1;
    const DescriptorMap descriptorMap{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("APSKU",
                           std::vector<uint8_t>{0x49, 0x35, 0x36, 0x81})}}}};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(1);
    MockdBusHandler dbusHandler;
    Manager manager(busMock, deviceInventoryInfo, descriptorMap, &dbusHandler);
    dbus::MctpInterfaces mctpInterfaces{{uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};
    EXPECT_EQ(manager.createEntry(eid, uuid, mctpInterfaces), objPath);
}

TEST(Manager, SingleMatchForAPSKUwithUpdateSecond)
{
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};
    DeviceInventoryInfo deviceInventoryInfo({
        {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}},
         {{objPath,
           {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"}}},
          {objPath}}}});
    const EID eid = 1;
    const DescriptorMap descriptorMap{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("SKU", std::vector<uint8_t>{0x12, 0x34, 0x56})},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("APSKU",
                           std::vector<uint8_t>{0x49, 0x35, 0x36, 0x81})}}}};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(1);
    MockdBusHandler dbusHandler;
    Manager manager(busMock, deviceInventoryInfo, descriptorMap, &dbusHandler);
    dbus::MctpInterfaces mctpInterfaces{{uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};
    EXPECT_EQ(manager.createEntry(eid, uuid, mctpInterfaces), objPath);
}

TEST(Manager, SingleMatchForAPSKUwithUpdate)
{
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};
    DeviceInventoryInfo deviceInventoryInfo({
        {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}},
         {{objPath,
           {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"}}},
          {objPath}}}});
    const EID eid = 1;
    const DescriptorMap descriptorMap{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("APSKU",
                           std::vector<uint8_t>{0x49, 0x35, 0x36, 0x81})}}}};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(1);
    MockdBusHandler dbusHandler;
    Manager manager(busMock, deviceInventoryInfo, descriptorMap, &dbusHandler);
    dbus::MctpInterfaces mctpInterfaces{{uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};
    EXPECT_EQ(manager.createEntry(eid, uuid, mctpInterfaces), objPath);
}

TEST(Manager, MultipleMatch)
{
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const UUID uuid1{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const UUID uuid2{"ad4c8360-c54c-11eb-8529-0242ac130004"};
    const std::string objPath1{"/xyz/openbmc_project/inventory/chassis/bmc1"};
    const std::string objPath2{"/xyz/openbmc_project/inventory/chassis/bmc2"};
    DeviceInventoryInfo deviceInventoryInfo({
        {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid1}}},
         {{objPath1,
           {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"}}},
          {}}},
        {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid2}}},
         {{objPath2,
           {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"}}},
          {}}}});
    const EID eid1 = 1;
    const EID eid2 = 1;
    const DescriptorMap descriptorMap{
        {eid1,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}}}},
        {eid2,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x01}}}}};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath1)))
        .Times(1);
    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath2)))
        .Times(1);

    MockdBusHandler dbusHandler;
    Manager manager(busMock, deviceInventoryInfo, descriptorMap, &dbusHandler);
    dbus::MctpInterfaces mctpInterfaces1{{uuid1, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid1}}}}}};
    dbus::MctpInterfaces mctpInterfaces2{{uuid2, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid2}}}}}};

    EXPECT_EQ(manager.createEntry(eid1, uuid1, mctpInterfaces1), objPath1);
    EXPECT_EQ(manager.createEntry(eid2, uuid2, mctpInterfaces2), objPath2);
}

TEST(Manager, MultiPropertyMatch)
{
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc1"};
    DeviceInventoryInfo deviceInventoryInfo({
        {{"xyz.openbmc_project.Inventory.Decorator.I2CDevice", {{"Address", uint32_t(0)}, {"Bus", uint32_t(16)}}},
         {{objPath,
           {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"}}},
          {}}}});
    const EID eid = 1;
    const DescriptorMap descriptorMap{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}}}}};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(1);

    MockdBusHandler dbusHandler;
    Manager manager(busMock, deviceInventoryInfo, descriptorMap, &dbusHandler);
    dbus::MctpInterfaces mctpInterfaces{{uuid, {{"xyz.openbmc_project.Inventory.Decorator.I2CDevice", {{"Address", uint32_t(0)}, {"Bus", uint32_t(16)}}}}}};
    EXPECT_EQ(manager.createEntry(eid, uuid, mctpInterfaces), objPath);
}

TEST(Manager, NoMatch)
{
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const UUID uuid1{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};
    DeviceInventoryInfo deviceInventoryInfo({
        {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid1}}},
         {{objPath,
           {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"}}},
          {}}}});
    const EID eid1 = 1;
    const DescriptorMap descriptorMap{
        {eid1,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}}}}};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(0);

    MockdBusHandler dbusHandler;
    Manager manager(busMock, deviceInventoryInfo, descriptorMap, &dbusHandler);
    dbus::MctpInterfaces mctpInterfaces;
    const UUID uuid2{"ad4c8360-c54c-11eb-8529-0242ac130004"};
    // Non-matching MCTP UUID, not present in the mctp end point interface entry
    EXPECT_FALSE(manager.createEntry(eid1, uuid2, mctpInterfaces).has_value());
    // Non-matching for "match" condition in device inventory info
    mctpInterfaces = {{uuid1, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid2}}}}}};
    EXPECT_FALSE(manager.createEntry(eid1, uuid1, mctpInterfaces).has_value());

}

TEST(Manager, MultiPropertyNoMatch)
{
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc1"};
    DeviceInventoryInfo deviceInventoryInfo({
        {{"xyz.openbmc_project.Inventory.Decorator.I2CDevice", {{"Address", uint32_t(0)}, {"Bus", uint32_t(16)}}},
         {{objPath,
           {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"}}},
          {}}}});
    const EID eid = 1;
    const DescriptorMap descriptorMap{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}}}}};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(0);

    MockdBusHandler dbusHandler;
    Manager manager(busMock, deviceInventoryInfo, descriptorMap, &dbusHandler);
    //No match for wrong property value
    dbus::MctpInterfaces mctpInterfaces{{uuid, {{"xyz.openbmc_project.Inventory.Decorator.I2CDevice", {{"Address", uint32_t(1)}, {"Bus", uint32_t(17)}}}}}};
    EXPECT_FALSE(manager.createEntry(eid, uuid, mctpInterfaces).has_value());
}