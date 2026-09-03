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
#include "common/types.hpp"

#include <systemd/sd-bus-protocol.h>

#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "fw-update/firmware_inventory.hpp"

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

    Entry entry(busMock, objPath, version, swId, "NVIDIA");
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
            [=](sd_bus*, const char*, const char*, const char** names) {
                EXPECT_STREQ("Associations", names[0]);
                return 0;
            });

    Entry entry(busMock, objPath, version, swId, "NVIDIA");
    entry.createAssociation(fwdAssociation, revAssociation, swObjectPath1);
    entry.createUpdateableAssociation(swObjectPath2);
}

TEST(Manager, SingleMatch)
{
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    eid eid = 1;
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
           std::make_tuple(compClassificationIndex1, activeCompVersion1,
                           static_cast<uint16_t>(0))},
          {std::make_pair(compClassification2, compIdentifier2),
           std::make_tuple(compClassificationIndex2, activeCompVersion2,
                           static_cast<uint16_t>(0))}}}};

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string compName1{"CompName1"};
    const Associations associations = {
        {"inventory", "activation", "/xyz/openbmc_project/software/CompName1"}};
    const ComponentObject componentObject = {compName1, associations, "NVIDIA",
                                             false};

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
            [=](sd_bus*, const char*, const char*, const char** names) {
                EXPECT_STREQ("Associations", names[0]);
                return 0;
            });

    ComponentNameMap componentNameMap;
    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);
    dbus::MctpInterfaces mctpInterfaces = {
        {uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};

    manager.createEntry(eid, uuid, mctpInterfaces);
}

TEST(Manager, SingleMatchTwoComponents)
{
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    eid eid = 1;
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
           std::make_tuple(compClassificationIndex1, activeCompVersion1,
                           static_cast<uint16_t>(0))},
          {std::make_pair(compClassification2, compIdentifier2),
           std::make_tuple(compClassificationIndex2, activeCompVersion2,
                           static_cast<uint16_t>(0))}}}};

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string compName1{"CompName1"};
    const Associations associations1 = {
        {"inventory", "activation", "/xyz/openbmc_project/software/CompName1"}};
    const ComponentObject componentObject1 = {compName1, associations1,
                                              "NVIDIA", false};

    const std::string compName2{"CompName2"};
    const Associations associations2 = {
        {"inventory", "activation", "/xyz/openbmc_project/software/CompName2"}};
    const ComponentObject componentObject2 = {compName2, associations2,
                                              "NVIDIA", false};
    DBusIntfMatch m;

    FirmwareInventoryInfo fwInventoryInfo(
        {{m,
          {{{compIdentifier1, componentObject1},
            {compIdentifier2, componentObject2}},
           {}}}});

    ComponentNameMap componentNameMap;
    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);
    dbus::MctpInterfaces mctpInterfaces;

    manager.createEntry(eid, uuid, mctpInterfaces);
}

TEST(Manager, MulipleMatch)
{
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    // ComponentInfoMap
    eid eid1 = 1;
    eid eid2 = 2;
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
           std::make_tuple(compClassificationIndex1, activeCompVersion1,
                           static_cast<uint16_t>(0))},
          {std::make_pair(compClassification2, compIdentifier2),
           std::make_tuple(compClassificationIndex2, activeCompVersion2,
                           static_cast<uint16_t>(0))}}},
        {eid2,
         {{std::make_pair(compClassification3, compIdentifier3),
           std::make_tuple(compClassificationIndex3, activeCompVersion3,
                           static_cast<uint16_t>(0))}}}};

    // FirmwareInventoryInfo
    const UUID uuid1{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const UUID uuid2{"ad4c8360-c54c-11eb-8529-0242ac130004"};

    const std::string compName1{"CompName1"};
    const Associations associations1 = {
        {"inventory", "activation", "/xyz/openbmc_project/software/CompName1"}};
    const ComponentObject componentObject1 = {compName1, associations1,
                                              "NVIDIA", false};

    const std::string compName2{"CompName2"};
    const Associations associations2 = {
        {"inventory", "activation", "/xyz/openbmc_project/software/CompName2"}};
    const ComponentObject componentObject2 = {compName2, associations2,
                                              "NVIDIA", false};

    const std::string compName3{"CompName3"};
    const Associations associations3 = {
        {"inventory", "activation", "/xyz/openbmc_project/software/CompName3"}};
    const ComponentObject componentObject3 = {compName3, associations3,
                                              "NVIDIA", false};

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
            [=](sd_bus*, const char*, const char*, const char** names) {
                EXPECT_STREQ("Associations", names[0]);
                return 0;
            });
    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath2)))
        .Times(1);
    EXPECT_CALL(sdbusMock,
                sd_bus_emit_properties_changed_strv(
                    IsNull(), StrEq(objPath2),
                    StrEq("xyz.openbmc_project.Association.Definitions"),
                    NotNull()))
        .Times(2)
        .WillRepeatedly(
            [=](sd_bus*, const char*, const char*, const char** names) {
                EXPECT_STREQ("Associations", names[0]);
                return 0;
            });
    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath3)))
        .Times(1);
    EXPECT_CALL(sdbusMock,
                sd_bus_emit_properties_changed_strv(
                    IsNull(), StrEq(objPath3),
                    StrEq("xyz.openbmc_project.Association.Definitions"),
                    NotNull()))
        .Times(2)
        .WillRepeatedly(
            [=](sd_bus*, const char*, const char*, const char** names) {
                EXPECT_STREQ("Associations", names[0]);
                return 0;
            });

    ComponentNameMap componentNameMap;
    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);
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

    eid eid = 1;
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
           std::make_tuple(compClassificationIndex1, activeCompVersion1,
                           static_cast<uint16_t>(0))},
          {std::make_pair(compClassification2, compIdentifier2),
           std::make_tuple(compClassificationIndex2, activeCompVersion2,
                           static_cast<uint16_t>(0))}}}};

    const std::string compName1{"CompName1"};
    const Associations associations1 = {
        {"inventory", "activation", "/xyz/openbmc_project/software/CompName1"}};
    const ComponentObject componentObject1 = {compName1, associations1,
                                              "NVIDIA", false};

    const std::string compName2{"CompName2"};
    const Associations associations2 = {
        {"inventory", "activation", "/xyz/openbmc_project/software/CompName2"}};
    const ComponentObject componentObject2 = {compName2, associations2,
                                              "NVIDIA", false};

    FirmwareInventoryInfo fwInventoryInfo(
        {{{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}},
          {{{compIdentifier1, componentObject1},
            {compIdentifier2, componentObject2}},
           {}}}});
    const std::string objPath = "/xyz/openbmc_project/software/" + compName1;

    ComponentNameMap componentNameMap;
    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);

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

    ComponentNameMap componentNameMap;
    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);

    EXPECT_NO_THROW({ manager.updateSwId(emptyObjPath, compName1); });
}

class FirmwareInventoryObjectTest : public pldm::fw_update::FirmwareInventory
{
  public:
    using pldm::fw_update::FirmwareInventory::FirmwareInventory;
    const std::string& getSoftwarePath() const
    {
        return softwarePath;
    }
    const SoftwareAssociationDefinitions& getAssociation() const
    {
        return association;
    }
    const SoftwareVersion& getVersion() const
    {
        return version;
    }
};

TEST(FirmwareInventoryObjectTest, ConstructorSetsProperties)
{
    SoftwareIdentifier softwareIdentifier{1, 100};
    const std::string softwareBasePath =
        "/xyz/openbmc_project/software/PLDM_Device_TestDevice";
    const std::string generatedId = "1234";
    const std::string expectedSoftwarePath =
        softwareBasePath + "_" + generatedId;
    const std::string expectedSoftwareVersion = "2.3.4";
    const std::string expectedEndpointPath =
        "/xyz/openbmc_project/inventory/system/board/PLDM_Device";
    SoftwareVersionPurpose expectedPurpose = SoftwareVersionPurpose::Unknown;

    FirmwareInventoryObjectTest inventory(
        softwareIdentifier, softwareBasePath, generatedId,
        expectedSoftwareVersion, expectedEndpointPath, expectedPurpose);

    EXPECT_EQ(inventory.getSoftwarePath(), expectedSoftwarePath);
    auto associationTuples = inventory.getAssociation().associations();
    ASSERT_FALSE(associationTuples.empty());
    EXPECT_EQ(std::get<2>(associationTuples[0]), expectedEndpointPath);
    EXPECT_EQ(inventory.getVersion().version(), expectedSoftwareVersion);
    EXPECT_EQ(inventory.getVersion().purpose(), expectedPurpose);
}

namespace
{

// Shared component identifiers/versions used by the additive Manager coverage
// tests below.
constexpr eid testEid = 1;
constexpr uint16_t testCompClassification1 = 10;
constexpr uint16_t testCompIdentifier1 = 300;
constexpr uint8_t testCompClassificationIndex1 = 20;
constexpr uint16_t testCompClassification2 = 16;
constexpr uint16_t testCompIdentifier2 = 301;
constexpr uint8_t testCompClassificationIndex2 = 30;

// Build a ComponentInfoMap for testEid carrying two components.
ComponentInfoMap makeTwoComponentInfoMap(const std::string& version1 = "v1.0",
                                         const std::string& version2 = "v2.0")
{
    return ComponentInfoMap{
        {testEid,
         {{std::make_pair(testCompClassification1, testCompIdentifier1),
           std::make_tuple(testCompClassificationIndex1, version1,
                           static_cast<uint16_t>(0))},
          {std::make_pair(testCompClassification2, testCompIdentifier2),
           std::make_tuple(testCompClassificationIndex2, version2,
                           static_cast<uint16_t>(0))}}}};
}

ComponentObject makeComponentObject(const std::string& name,
                                    bool updateOnly = false)
{
    Associations assocs = {
        {"inventory", "activation",
         "/xyz/openbmc_project/software/" + name + "_assoc"}};
    return ComponentObject{name, assocs, "NVIDIA", updateOnly};
}

} // namespace

// createEntry() must early-return when the endpoint has no component info.
TEST(ManagerCoverage, CreateEntrySkippedWhenNoComponentInfo)
{
    ::testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    ComponentInfoMap componentInfoMap{};
    ComponentNameMap componentNameMap{};
    FirmwareInventoryInfo fwInventoryInfo{};
    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    dbus::MctpInterfaces mctpInterfaces{};
    EXPECT_NO_THROW(manager.createEntry(testEid, uuid, mctpInterfaces));
    EXPECT_TRUE(manager.firmwareInventoryMap.empty());
}

// No config JSON match: createEntry() falls back to componentNameMap generated
// names (the else branch).
TEST(ManagerCoverage, CreateEntryUsesComponentNameMapWhenNoConfigMatch)
{
    ::testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    auto componentInfoMap = makeTwoComponentInfoMap();
    ComponentNameMap componentNameMap{{testEid,
                                       {{testCompIdentifier1, "GenName1"},
                                        {testCompIdentifier2, "GenName2"}}}};
    // Empty inventory info => matchInventoryEntry() returns false.
    FirmwareInventoryInfo fwInventoryInfo{};
    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    dbus::MctpInterfaces mctpInterfaces{
        {uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};

    manager.createEntry(testEid, uuid, mctpInterfaces);
    EXPECT_EQ(manager.firmwareInventoryMap.size(), 2U);

    // Second call refreshes the preserved objects in place (setVersion path).
    auto refreshedInfoMap = makeTwoComponentInfoMap("v9.0", "v9.1");
    Manager refreshManager(busMock, fwInventoryInfo, refreshedInfoMap,
                           componentNameMap);
    refreshManager.createEntry(testEid, uuid, mctpInterfaces);
    refreshManager.createEntry(testEid, uuid, mctpInterfaces);
    EXPECT_EQ(refreshManager.firmwareInventoryMap.size(), 2U);
}

// Config JSON match with an update-only component: no competing object is
// created, only updateSwId() runs.
TEST(ManagerCoverage, CreateEntryUpdateOnlyComponentStampsSwId)
{
    ::testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    auto componentInfoMap = makeTwoComponentInfoMap();
    ComponentNameMap componentNameMap{};

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    ComponentObject updateOnlyObj = makeComponentObject("UpdateOnlyComp", true);
    FirmwareInventoryInfo fwInventoryInfo(
        {{{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}},
          {{{testCompIdentifier1, updateOnlyObj}}, {}}}});
    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);
    dbus::MctpInterfaces mctpInterfaces{
        {uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};

    manager.createEntry(testEid, uuid, mctpInterfaces);
    // Update-only components are not added to the inventory map.
    EXPECT_TRUE(manager.firmwareInventoryMap.empty());
    EXPECT_FALSE(manager.compIdentifierLookup.empty());
}

// Config JSON match that carries an UpdateComponentIdNameMap entry: the
// SoftwareId is stamped onto an existing object for that component.
TEST(ManagerCoverage, CreateEntryStampsSwIdForUpdateComponentNames)
{
    ::testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    auto componentInfoMap = makeTwoComponentInfoMap();
    ComponentNameMap componentNameMap{};

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    ComponentObject compObj1 = makeComponentObject("CreateComp");
    // Second element of FirmwareInfo is the UpdateComponentIdNameMap.
    FirmwareInventoryInfo fwInventoryInfo(
        {{{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}},
          {{{testCompIdentifier1, compObj1}},
           {{testCompIdentifier2, "UpdateName2"}}}}});
    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);
    dbus::MctpInterfaces mctpInterfaces{
        {uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};

    manager.createEntry(testEid, uuid, mctpInterfaces);
    // Component 1 created a real object; component 2 only stamped a SwId.
    EXPECT_EQ(manager.firmwareInventoryMap.size(), 1U);
    EXPECT_FALSE(manager.compIdentifierLookup.empty());
}

// EM-config path: setEmComponentObjects() metadata is preferred and drives
// object creation, fallback naming, update-only stamping and skips.
TEST(ManagerCoverage, CreateEntryEmConfigCreatesObjects)
{
    ::testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    auto componentInfoMap = makeTwoComponentInfoMap();
    ComponentNameMap componentNameMap{};
    FirmwareInventoryInfo fwInventoryInfo{};
    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);

    CreateComponentIdNameMap emComponents{
        {testCompIdentifier1, makeComponentObject("EmComp1")},
        {testCompIdentifier2, makeComponentObject("EmComp2")}};
    manager.setEmComponentObjects(testEid, emComponents);

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    dbus::MctpInterfaces mctpInterfaces{};
    manager.createEntry(testEid, uuid, mctpInterfaces);
    EXPECT_EQ(manager.firmwareInventoryMap.size(), 2U);
}

TEST(ManagerCoverage, CreateEntryEmConfigUpdateOnlyAndFallbackAndSkip)
{
    ::testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    // Three components: one update-only (EM), one fallback-named
    // (componentNameMap), one skipped (declared nowhere).
    constexpr uint16_t compIdentifier3 = 302;
    ComponentInfoMap componentInfoMap{
        {testEid,
         {{std::make_pair(testCompClassification1, testCompIdentifier1),
           std::make_tuple(testCompClassificationIndex1, std::string{"v1"},
                           static_cast<uint16_t>(0))},
          {std::make_pair(testCompClassification2, testCompIdentifier2),
           std::make_tuple(testCompClassificationIndex2, std::string{"v2"},
                           static_cast<uint16_t>(0))},
          {std::make_pair(testCompClassification1, compIdentifier3),
           std::make_tuple(testCompClassificationIndex1, std::string{"v3"},
                           static_cast<uint16_t>(0))}}}};
    ComponentNameMap componentNameMap{
        {testEid, {{testCompIdentifier2, "FallbackComp2"}}}};
    FirmwareInventoryInfo fwInventoryInfo{};
    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);

    CreateComponentIdNameMap emComponents{
        {testCompIdentifier1, makeComponentObject("EmUpdateOnly", true)}};
    manager.setEmComponentObjects(testEid, emComponents);

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    dbus::MctpInterfaces mctpInterfaces{};
    manager.createEntry(testEid, uuid, mctpInterfaces);

    // comp1 is update-only (no object), comp2 uses the fallback name (object),
    // comp3 is skipped entirely.
    EXPECT_EQ(manager.firmwareInventoryMap.size(), 1U);
    EXPECT_FALSE(manager.compIdentifierLookup.empty());
}

// updateEntry() exercises: missing EID, refresh of an existing entry, and the
// "no entries updated" path.
TEST(ManagerCoverage, UpdateEntryPaths)
{
    ::testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    dbus::MctpInterfaces mctpInterfaces{
        {uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};

    // Missing EID -> early return.
    {
        ComponentInfoMap emptyInfo{};
        ComponentNameMap componentNameMap{};
        FirmwareInventoryInfo fwInventoryInfo{};
        Manager manager(busMock, fwInventoryInfo, emptyInfo, componentNameMap);
        EXPECT_NO_THROW(manager.updateEntry(testEid, uuid, mctpInterfaces));
    }

    // EID present but no inventory objects -> "no entries updated" path.
    {
        auto componentInfoMap = makeTwoComponentInfoMap();
        ComponentNameMap componentNameMap{};
        FirmwareInventoryInfo fwInventoryInfo{};
        Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                        componentNameMap);
        EXPECT_NO_THROW(manager.updateEntry(testEid, uuid, mctpInterfaces));
    }

    // EID present with existing inventory objects -> refresh each entry.
    {
        auto componentInfoMap = makeTwoComponentInfoMap();
        ComponentNameMap componentNameMap{
            {testEid,
             {{testCompIdentifier1, "GenName1"},
              {testCompIdentifier2, "GenName2"}}}};
        FirmwareInventoryInfo fwInventoryInfo{};
        Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                        componentNameMap);
        manager.createEntry(testEid, uuid, mctpInterfaces);
        ASSERT_EQ(manager.firmwareInventoryMap.size(), 2U);
        EXPECT_NO_THROW(manager.updateEntry(testEid, uuid, mctpInterfaces));
    }
}

// updateFWVersion() exercises both the found and not-found branches.
TEST(ManagerCoverage, UpdateFWVersionPaths)
{
    ::testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    dbus::MctpInterfaces mctpInterfaces{
        {uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};

    // Unknown EID -> logs and returns.
    {
        ComponentInfoMap emptyInfo{};
        ComponentNameMap componentNameMap{};
        FirmwareInventoryInfo fwInventoryInfo{};
        Manager manager(busMock, fwInventoryInfo, emptyInfo, componentNameMap);
        EXPECT_NO_THROW(manager.updateFWVersion(testEid));
    }

    // Known EID with existing objects -> setVersion on each.
    {
        auto componentInfoMap = makeTwoComponentInfoMap();
        ComponentNameMap componentNameMap{
            {testEid,
             {{testCompIdentifier1, "GenName1"},
              {testCompIdentifier2, "GenName2"}}}};
        FirmwareInventoryInfo fwInventoryInfo{};
        Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                        componentNameMap);
        manager.createEntry(testEid, uuid, mctpInterfaces);
        ASSERT_EQ(manager.firmwareInventoryMap.size(), 2U);
        EXPECT_NO_THROW(manager.updateFWVersion(testEid));
    }
}

// A second createEntry() with a JSON-config match refreshes the preserved
// object in place (setVersion) rather than re-registering the path.
TEST(ManagerCoverage, CreateEntryJsonConfigRefreshInPlace)
{
    ::testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    ComponentObject compObj = makeComponentObject("RefreshComp");
    FirmwareInventoryInfo fwInventoryInfo(
        {{{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}},
          {{{testCompIdentifier1, compObj}}, {}}}});
    dbus::MctpInterfaces mctpInterfaces{
        {uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};
    ComponentNameMap componentNameMap{};

    auto componentInfoMap = makeTwoComponentInfoMap("orig1", "orig2");
    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);

    manager.createEntry(testEid, uuid, mctpInterfaces);
    ASSERT_EQ(manager.firmwareInventoryMap.size(), 1U);

    // Second call finds the existing entry and refreshes its version in place.
    manager.createEntry(testEid, uuid, mctpInterfaces);
    EXPECT_EQ(manager.firmwareInventoryMap.size(), 1U);
}
