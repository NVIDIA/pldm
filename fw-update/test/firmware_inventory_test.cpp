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
#include "common/test/mocked_utils.hpp"
#define private public
#include "fw-update/firmware_inventory.hpp"

#include <sdbusplus/bus.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldm;
using namespace pldm::fw_update;
using namespace pldm::fw_update::fw_inventory;

using ::testing::_;
using ::testing::DoAll;
using ::testing::Invoke;
using ::testing::IsNull;
using ::testing::NotNull;
using ::testing::Return;
using ::testing::StrEq;

TEST(Entry, Basic)
{
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const std::string objPath{"/xyz/openbmc_project/software/bmc"};
    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string version{"MAJOR.MINOR.PATCH"};
    const std::string swId{"0x0001"};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(1);

    Entry entry(busMock, objPath, version, swId);
}

TEST(Entry, BasicEntryCreateAssociation)
{
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const std::string objPath{"/xyz/openbmc_project/software/bmc"};
    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string version{"MAJOR.MINOR.PATCH"};
    const std::string swId{"0x0001"};

    const std::string fwdAssociation{"inventory"};
    const std::string revAssociation{"activation"};
    const std::string swObjectPath1{
        "/xyz/openbmc_project/software/ComponentName1"};
    const std::string swObjectPath2{"/xyz/openbmc_project/software"};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(1);

    EXPECT_CALL(sdbusMock,
                sd_bus_emit_properties_changed_strv(
                    IsNull(), StrEq(objPath),
                    StrEq("xyz.openbmc_project.Association.Definitions"),
                    NotNull()))
        .Times(2)
        .WillRepeatedly(
            Invoke([=](sd_bus*, const char*, const char*, const char** names) {
                EXPECT_STREQ("Associations", names[0]);
                return 0;
            }));

    Entry entry(busMock, objPath, version, swId);
    entry.createAssociation(fwdAssociation, revAssociation, swObjectPath1);
    entry.createUpdateableAssociation(swObjectPath2);
}

TEST(Manager, SingleMatch)
{
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    EID eid = 1;
    const std::string activeCompVersion1{"Comp1v2.0"};
    const std::string activeCompVersion2{"Comp2v3.0"};
    constexpr uint16_t compClassification1 = 10;
    constexpr uint16_t compIdentifier1 = 300;
    constexpr uint8_t compClassificationIndex1 = 20;
    constexpr uint16_t compClassification2 = 16;
    constexpr uint16_t compIdentifier2 = 301;
    constexpr uint8_t compClassificationIndex2 = 30;
    ComponentInfoMap componentInfoMap{
        {eid,
         {{std::make_pair(compClassification1, compIdentifier1),
           std::make_tuple(compClassificationIndex1, activeCompVersion1)},
          {std::make_pair(compClassification2, compIdentifier2),
           std::make_tuple(compClassificationIndex2, activeCompVersion2)}}}};

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string compName1{"CompName1"};
    const Associations associations = {
        {"inventory", "activation", "/xyz/openbmc_project/software/CompName1"}};
    const ComponentObject componentObject = {compName1, associations};

    FirmwareInventoryInfo fwInventoryInfo(
        {{{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}},
          {{{compIdentifier1, componentObject}}, {}}}});
    const std::string objPath = "/xyz/openbmc_project/software/" + compName1;

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(1);
    EXPECT_CALL(sdbusMock,
                sd_bus_emit_properties_changed_strv(
                    IsNull(), StrEq(objPath),
                    StrEq("xyz.openbmc_project.Association.Definitions"),
                    NotNull()))
        .Times(2)
        .WillRepeatedly(
            Invoke([=](sd_bus*, const char*, const char*, const char** names) {
                EXPECT_STREQ("Associations", names[0]);
                return 0;
            }));

    MockdBusHandler dbusHandler;
    Manager manager(busMock, fwInventoryInfo, componentInfoMap, &dbusHandler);
    dbus::MctpInterfaces mctpInterfaces = {
        {uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};

    manager.createEntry(eid, uuid, mctpInterfaces);
}

TEST(Manager, SingleMatchTwoComponents)
{
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    EID eid = 1;
    const std::string activeCompVersion1{"Comp1v2.0"};
    const std::string activeCompVersion2{"Comp2v3.0"};
    constexpr uint16_t compClassification1 = 10;
    constexpr uint16_t compIdentifier1 = 300;
    constexpr uint8_t compClassificationIndex1 = 20;
    constexpr uint16_t compClassification2 = 16;
    constexpr uint16_t compIdentifier2 = 301;
    constexpr uint8_t compClassificationIndex2 = 30;
    ComponentInfoMap componentInfoMap{
        {eid,
         {{std::make_pair(compClassification1, compIdentifier1),
           std::make_tuple(compClassificationIndex1, activeCompVersion1)},
          {std::make_pair(compClassification2, compIdentifier2),
           std::make_tuple(compClassificationIndex2, activeCompVersion2)}}}};

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string compName1{"CompName1"};
    const Associations associations1 = {
        {"inventory", "activation", "/xyz/openbmc_project/software/CompName1"}};
    const ComponentObject componentObject1 = {compName1, associations1};

    const std::string compName2{"CompName2"};
    const Associations associations2 = {
        {"inventory", "activation", "/xyz/openbmc_project/software/CompName2"}};
    const ComponentObject componentObject2 = {compName2, associations2};
    DBusIntfMatch m;

    FirmwareInventoryInfo fwInventoryInfo(
        {{m,
          {{{compIdentifier1, componentObject1},
            {compIdentifier2, componentObject2}},
           {}}}});

    MockdBusHandler dbusHandler;
    Manager manager(busMock, fwInventoryInfo, componentInfoMap, &dbusHandler);
    dbus::MctpInterfaces mctpInterfaces;

    manager.createEntry(eid, uuid, mctpInterfaces);
}

TEST(Manager, MulipleMatch)
{
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    // ComponentInfoMap
    EID eid1 = 1;
    EID eid2 = 2;
    const std::string activeCompVersion1{"Comp1v2.0"};
    const std::string activeCompVersion2{"Comp2v3.0"};
    const std::string activeCompVersion3{"Comp2v4.0"};
    constexpr uint16_t compClassification1 = 10;
    constexpr uint16_t compIdentifier1 = 300;
    constexpr uint8_t compClassificationIndex1 = 20;
    constexpr uint16_t compClassification2 = 16;
    constexpr uint16_t compIdentifier2 = 301;
    constexpr uint8_t compClassificationIndex2 = 30;
    constexpr uint16_t compClassification3 = 10;
    constexpr uint16_t compIdentifier3 = 302;
    constexpr uint8_t compClassificationIndex3 = 40;
    ComponentInfoMap componentInfoMap{
        {eid1,
         {{std::make_pair(compClassification1, compIdentifier1),
           std::make_tuple(compClassificationIndex1, activeCompVersion1)},
          {std::make_pair(compClassification2, compIdentifier2),
           std::make_tuple(compClassificationIndex2, activeCompVersion2)}}},
        {eid2,
         {{std::make_pair(compClassification3, compIdentifier3),
           std::make_tuple(compClassificationIndex3, activeCompVersion3)}}}};

    // FirmwareInventoryInfo
    const UUID uuid1{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const UUID uuid2{"ad4c8360-c54c-11eb-8529-0242ac130004"};

    const std::string compName1{"CompName1"};
    const Associations associations1 = {
        {"inventory", "activation", "/xyz/openbmc_project/software/CompName1"}};
    const ComponentObject componentObject1 = {compName1, associations1};

    const std::string compName2{"CompName2"};
    const Associations associations2 = {
        {"inventory", "activation", "/xyz/openbmc_project/software/CompName2"}};
    const ComponentObject componentObject2 = {compName2, associations2};

    const std::string compName3{"CompName3"};
    const Associations associations3 = {
        {"inventory", "activation", "/xyz/openbmc_project/software/CompName3"}};
    const ComponentObject componentObject3 = {compName3, associations3};

    FirmwareInventoryInfo fwInventoryInfo(
        {{{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid1}}},
          {{{compIdentifier1, componentObject1},
            {compIdentifier2, componentObject2}},
           {}}},
         {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid2}}},
          {{{compIdentifier3, componentObject3}}, {}}}});
    const std::string objPath1 = "/xyz/openbmc_project/software/" + compName1;
    const std::string objPath2 = "/xyz/openbmc_project/software/" + compName2;
    const std::string objPath3 = "/xyz/openbmc_project/software/" + compName3;

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath1)))
        .Times(1);
    EXPECT_CALL(sdbusMock,
                sd_bus_emit_properties_changed_strv(
                    IsNull(), StrEq(objPath1),
                    StrEq("xyz.openbmc_project.Association.Definitions"),
                    NotNull()))
        .Times(2)
        .WillRepeatedly(
            Invoke([=](sd_bus*, const char*, const char*, const char** names) {
                EXPECT_STREQ("Associations", names[0]);
                return 0;
            }));
    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath2)))
        .Times(1);
    EXPECT_CALL(sdbusMock,
                sd_bus_emit_properties_changed_strv(
                    IsNull(), StrEq(objPath2),
                    StrEq("xyz.openbmc_project.Association.Definitions"),
                    NotNull()))
        .Times(2)
        .WillRepeatedly(
            Invoke([=](sd_bus*, const char*, const char*, const char** names) {
                EXPECT_STREQ("Associations", names[0]);
                return 0;
            }));
    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath3)))
        .Times(1);
    EXPECT_CALL(sdbusMock,
                sd_bus_emit_properties_changed_strv(
                    IsNull(), StrEq(objPath3),
                    StrEq("xyz.openbmc_project.Association.Definitions"),
                    NotNull()))
        .Times(2)
        .WillRepeatedly(
            Invoke([=](sd_bus*, const char*, const char*, const char** names) {
                EXPECT_STREQ("Associations", names[0]);
                return 0;
            }));

    MockdBusHandler dbusHandler;
    Manager manager(busMock, fwInventoryInfo, componentInfoMap, &dbusHandler);
    dbus::MctpInterfaces mctpInterfaces{
        {uuid1, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid1}}}}},
        {uuid2, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid2}}}}}};

    manager.createEntry(eid1, uuid1, mctpInterfaces);
    manager.createEntry(eid2, uuid2, mctpInterfaces);
}

TEST(Manager, test_private_method_updateSwId)
{
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};

    EID eid = 1;
    const std::string activeCompVersion1{"Comp1v2.0"};
    const std::string activeCompVersion2{"Comp2v3.0"};
    constexpr uint16_t compClassification1 = 10;
    constexpr uint16_t compIdentifier1 = 300;
    constexpr uint8_t compClassificationIndex1 = 20;
    constexpr uint16_t compClassification2 = 16;
    constexpr uint16_t compIdentifier2 = 301;
    constexpr uint8_t compClassificationIndex2 = 30;
    ComponentInfoMap componentInfoMap{
        {eid,
         {{std::make_pair(compClassification1, compIdentifier1),
           std::make_tuple(compClassificationIndex1, activeCompVersion1)},
          {std::make_pair(compClassification2, compIdentifier2),
           std::make_tuple(compClassificationIndex2, activeCompVersion2)}}}};

    const std::string compName1{"CompName1"};
    const Associations associations1 = {
        {"inventory", "activation", "/xyz/openbmc_project/software/CompName1"}};
    const ComponentObject componentObject1 = {compName1, associations1};

    const std::string compName2{"CompName2"};
    const Associations associations2 = {
        {"inventory", "activation", "/xyz/openbmc_project/software/CompName2"}};
    const ComponentObject componentObject2 = {compName2, associations2};

    FirmwareInventoryInfo fwInventoryInfo(
        {{{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}},
          {{{compIdentifier1, componentObject1},
            {compIdentifier2, componentObject2}},
           {}}}});
    const std::string objPath = "/xyz/openbmc_project/software/" + compName1;

    MockdBusHandler dbusHandler;
    Manager manager(busMock, fwInventoryInfo, componentInfoMap, &dbusHandler);

    EXPECT_NO_THROW({ manager.updateSwId(objPath, compName1); });
}

TEST(Manager, test_private_method_updateSwId_emptyObjPath)
{
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    ComponentInfoMap componentInfoMap{};

    const std::string compName1{"CompName1"};
    FirmwareInventoryInfo fwInventoryInfo{};

    const std::string emptyObjPath;

    MockdBusHandler dbusHandler;
    Manager manager(busMock, fwInventoryInfo, componentInfoMap, &dbusHandler);

    EXPECT_NO_THROW({ manager.updateSwId(emptyObjPath, compName1); });
}