/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "fw-update/dbusutil.hpp"

#include <systemd/sd-bus.h>

#include <sdbusplus/test/sdbus_mock.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

std::atomic<int> mockedSetSkuCallCount{0};
std::atomic<bool> mockedSetSkuShouldThrow{true};

void setDBusPropertyMock(const pldm::utils::DBusMapping&, const std::string&)
{
    ++mockedSetSkuCallCount;
    if (mockedSetSkuShouldThrow)
    {
        throw std::runtime_error("mock set property failure");
    }
}

template <typename T>
void setDBusPropertyAsyncSkuMock(
    const std::string& /*objectPath*/, const std::string& /*interface*/,
    const std::string& propertyName, const T& /*value*/,
    std::function<void(bool)> onComplete = nullptr)
{
    if (propertyName != "SKU")
    {
        return;
    }
    ++mockedSetSkuCallCount;
    if (onComplete)
    {
        onComplete(!mockedSetSkuShouldThrow.load());
    }
}

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#define setDBusProperty setDBusPropertyMock
#define setDBusPropertyAsync setDBusPropertyAsyncSkuMock
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "fw-update/device_inventory.cpp" // NOLINT(bugprone-suspicious-include)
#undef setDBusPropertyAsync
#undef setDBusProperty
#undef private

using namespace pldm;
using namespace pldm::fw_update;
using namespace pldm::fw_update::device_inventory;
using ::testing::IsNull;
using ::testing::StrEq;

namespace
{

void waitForSkuCallCount(int expectedCount)
{
    for (int i = 0; i < 100 && mockedSetSkuCallCount.load() < expectedCount;
         ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

TEST(DeviceInventoryInternalThreadTest, updateSKUOnMatchRunsDetachedLambda)
{
    mockedSetSkuCallCount = 0;
    mockedSetSkuShouldThrow = true;

    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};

    DeviceInventoryInfo deviceInventoryInfo(
        {{{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}},
          {{objPath,
            {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"}}},
           {objPath}}}});

    const eid eid1 = 1;
    const DescriptorMap descriptorMap{
        {eid1,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("ECSKU",
                           std::vector<uint8_t>{0x49, 0x35, 0x36, 0x81})},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("APSKU",
                           std::vector<uint8_t>{0x11, 0x22, 0x33, 0x44})}}}};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(1);

    Manager manager(busMock, deviceInventoryInfo, descriptorMap);
    dbus::MctpInterfaces mctpInterfaces{
        {uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};

    ASSERT_EQ(manager.createEntry(eid1, uuid, mctpInterfaces), objPath);
    ASSERT_TRUE(manager.skuLookup.contains(objPath));

    sdbusplus::object_path signalPath{objPath};
    dbus::InterfaceMap validInterfaces{
        {"xyz.openbmc_project.Inventory.Decorator.SKU",
         {{"SKU", std::string("0x11223344")}}}};
    auto rawBus = sdbusplus::bus::new_default();
    auto validSignal = rawBus.new_method_call("org.test", objPath.c_str(),
                                              "org.test.Interface", "Method");
    validSignal.append(signalPath, validInterfaces);
    sd_bus_message_seal(validSignal.get(), 0, 0);
    sd_bus_message_rewind(validSignal.get(), true);
    EXPECT_NO_THROW({ manager.updateSKUOnMatch(validSignal); });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    waitForSkuCallCount(2);
    EXPECT_GE(mockedSetSkuCallCount.load(), 1);
}

TEST(DeviceInventoryInternalThreadTest,
     updateSKUOnMatchReturnsWhenSkuInterfaceIsMissing)
{
    mockedSetSkuCallCount = 0;
    mockedSetSkuShouldThrow = true;
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    DeviceInventoryInfo deviceInventoryInfo{};
    DescriptorMap descriptorMap{};
    Manager manager(busMock, deviceInventoryInfo, descriptorMap);

    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};
    manager.skuLookup.emplace(objPath, "0x11223344");

    sdbusplus::object_path signalPath{objPath};
    dbus::InterfaceMap interfaces{
        {"xyz.openbmc_project.Inventory.Decorator.Asset",
         {{"Model", std::string("X")}}}};
    auto rawBus = sdbusplus::bus::new_default();
    auto signal = rawBus.new_method_call("org.test", objPath.c_str(),
                                         "org.test.Interface", "Method");
    signal.append(signalPath, interfaces);
    sd_bus_message_seal(signal.get(), 0, 0);
    sd_bus_message_rewind(signal.get(), true);

    EXPECT_NO_THROW({ manager.updateSKUOnMatch(signal); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(mockedSetSkuCallCount.load(), 0);
}

TEST(DeviceInventoryInternalThreadTest,
     updateSKUOnMatchReturnsWhenLookupEntryDoesNotExist)
{
    mockedSetSkuCallCount = 0;
    mockedSetSkuShouldThrow = true;
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    DeviceInventoryInfo deviceInventoryInfo{};
    DescriptorMap descriptorMap{};
    Manager manager(busMock, deviceInventoryInfo, descriptorMap);

    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};
    sdbusplus::object_path signalPath{objPath};
    dbus::InterfaceMap interfaces{
        {"xyz.openbmc_project.Inventory.Decorator.SKU",
         {{"SKU", std::string("0x11223344")}}}};
    auto rawBus = sdbusplus::bus::new_default();
    auto signal = rawBus.new_method_call("org.test", objPath.c_str(),
                                         "org.test.Interface", "Method");
    signal.append(signalPath, interfaces);
    sd_bus_message_seal(signal.get(), 0, 0);
    sd_bus_message_rewind(signal.get(), true);

    EXPECT_NO_THROW({ manager.updateSKUOnMatch(signal); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(mockedSetSkuCallCount.load(), 0);
}

TEST(DeviceInventoryInternalThreadTest, updateEntryAndUpdateSKUEarlyReturnPaths)
{
    mockedSetSkuShouldThrow = true;
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};

    DeviceInventoryInfo deviceInventoryInfo(
        {{{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}},
          {{objPath,
            {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"}}},
           {objPath}}}});

    const eid eid1 = 1;
    const eid eidMissing = 2;
    const DescriptorMap descriptorMap{
        {eid1,
         {{PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("ECSKU",
                           std::vector<uint8_t>{0x49, 0x35, 0x36, 0x81})}}}};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(1);

    Manager manager(busMock, deviceInventoryInfo, descriptorMap);
    dbus::MctpInterfaces mctpInterfaces{
        {uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};

    ASSERT_TRUE(manager.createEntry(eid1, uuid, mctpInterfaces).has_value());
    EXPECT_EQ(manager.updateEntry(eidMissing, uuid, mctpInterfaces),
              std::nullopt);
    EXPECT_NO_THROW({ manager.updateSKU("", "0xAABBCCDD"); });
}

TEST(DeviceInventoryInternalThreadTest,
     updateEntryExistingDeviceRunsVendorSkuUpdatePath)
{
    mockedSetSkuCallCount = 0;
    mockedSetSkuShouldThrow = true;
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};

    DeviceInventoryInfo deviceInventoryInfo(
        {{{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}},
          {{objPath,
            {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"}}},
           {"/xyz/openbmc_project/software/entry0"}}}});

    const eid eid1 = 1;
    const DescriptorMap descriptorMap{
        {eid1,
         {{PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("ECSKU",
                           std::vector<uint8_t>{0x49, 0x35, 0x36, 0x81})},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("APSKU",
                           std::vector<uint8_t>{0x11, 0x22, 0x33, 0x44})}}}};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(1);

    Manager manager(busMock, deviceInventoryInfo, descriptorMap);
    dbus::MctpInterfaces mctpInterfaces{
        {uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};

    ASSERT_TRUE(manager.createEntry(eid1, uuid, mctpInterfaces).has_value());
    auto updated = manager.updateEntry(eid1, uuid, mctpInterfaces);
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(*updated, objPath);

    waitForSkuCallCount(1);
    EXPECT_GE(mockedSetSkuCallCount.load(), 1);
}

TEST(DeviceInventoryInternalThreadTest,
     updateSKUOnMatchHandlesNonThrowingSetPropertyPath)
{
    mockedSetSkuCallCount = 0;
    mockedSetSkuShouldThrow = false;

    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};

    DeviceInventoryInfo deviceInventoryInfo(
        {{{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}},
          {{objPath,
            {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"}}},
           {objPath}}}});

    const eid eid1 = 1;
    const DescriptorMap descriptorMap{
        {eid1,
         {{PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("APSKU",
                           std::vector<uint8_t>{0x11, 0x22, 0x33, 0x44})}}}};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(1);

    Manager manager(busMock, deviceInventoryInfo, descriptorMap);
    dbus::MctpInterfaces mctpInterfaces{
        {uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};

    ASSERT_TRUE(manager.createEntry(eid1, uuid, mctpInterfaces).has_value());
    waitForSkuCallCount(1);

    sdbusplus::object_path signalPath{objPath};
    dbus::InterfaceMap validInterfaces{
        {"xyz.openbmc_project.Inventory.Decorator.SKU",
         {{"SKU", std::string("0x11223344")}}}};
    auto rawBus = sdbusplus::bus::new_default();
    auto validSignal = rawBus.new_method_call("org.test", objPath.c_str(),
                                              "org.test.Interface", "Method");
    validSignal.append(signalPath, validInterfaces);
    sd_bus_message_seal(validSignal.get(), 0, 0);
    sd_bus_message_rewind(validSignal.get(), true);
    EXPECT_NO_THROW({ manager.updateSKUOnMatch(validSignal); });

    waitForSkuCallCount(2);
    EXPECT_GE(mockedSetSkuCallCount.load(), 2);
    mockedSetSkuShouldThrow = true;
}

TEST(DeviceInventoryInternalThreadTest,
     createEntrySkipsObjectCreationForEmptyPathAndShortVendorData)
{
    mockedSetSkuCallCount = 0;
    mockedSetSkuShouldThrow = true;

    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    DeviceInventoryInfo deviceInventoryInfo(
        {{{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}},
          {{"", {}}, {"/xyz/openbmc_project/software/entry0"}}}});

    const eid eid1 = 1;
    const DescriptorMap descriptorMap{
        {eid1,
         {{PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("ECSKU", std::vector<uint8_t>{0x11, 0x22, 0x33})},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("APSKU", std::vector<uint8_t>{0x44, 0x55, 0x66})}}}};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), testing::_))
        .Times(0);

    Manager manager(busMock, deviceInventoryInfo, descriptorMap);
    dbus::MctpInterfaces mctpInterfaces{
        {uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};

    auto created = manager.createEntry(eid1, uuid, mctpInterfaces);
    EXPECT_FALSE(created.has_value());
    EXPECT_TRUE(manager.skuLookup.empty());
}

TEST(DeviceInventoryInternalThreadTest,
     updateEntryReturnsNulloptWhenMctpInterfacesDoNotContainUuid)
{
    mockedSetSkuShouldThrow = true;

    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};
    DeviceInventoryInfo deviceInventoryInfo(
        {{{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}},
          {{objPath, {}}, {}}}});

    const eid eid1 = 1;
    const DescriptorMap descriptorMap{
        {eid1,
         {{PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("ECSKU",
                           std::vector<uint8_t>{0x49, 0x35, 0x36, 0x81})}}}};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(1);

    Manager manager(busMock, deviceInventoryInfo, descriptorMap);
    dbus::MctpInterfaces matchInterfaces{
        {uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};
    ASSERT_TRUE(manager.createEntry(eid1, uuid, matchInterfaces).has_value());

    dbus::MctpInterfaces noInterfaces{};
    EXPECT_EQ(manager.updateEntry(eid1, uuid, noInterfaces), std::nullopt);
}

TEST(DeviceInventoryInternalThreadTest,
     createEntrySkipsShortVendorPayloadsAndApskuUpdate)
{
    mockedSetSkuCallCount = 0;
    mockedSetSkuShouldThrow = true;

    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};
    const std::string updateObjPath{"/xyz/openbmc_project/software/entry0"};
    DeviceInventoryInfo deviceInventoryInfo(
        {{{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}},
          {{objPath,
            {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"}}},
           {updateObjPath}}}});

    const DescriptorMap descriptorMap{
        {1,
         {{PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("ECSKU", std::vector<uint8_t>{0x11, 0x22, 0x33})},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("APSKU", std::vector<uint8_t>{0x44, 0x55, 0x66})}}}};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(1);

    Manager manager(busMock, deviceInventoryInfo, descriptorMap);
    dbus::MctpInterfaces mctpInterfaces{
        {uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};

    auto created = manager.createEntry(1, uuid, mctpInterfaces);
    ASSERT_TRUE(created.has_value());
    EXPECT_EQ(*created, objPath);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(mockedSetSkuCallCount.load(), 0);
    ASSERT_TRUE(manager.deviceEntryMap.contains(uuid));
    EXPECT_TRUE(manager.deviceEntryMap.at(uuid)->sku().empty());
}

TEST(DeviceInventoryInternalThreadTest,
     updateEntryReturnsNulloptWhenInventoryMatchFailsDespiteUuidPresence)
{
    mockedSetSkuCallCount = 0;
    mockedSetSkuShouldThrow = true;

    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const UUID mismatchUuid{"ad4c8360-c54c-11eb-8529-0242ac130099"};
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};
    const Associations assocs{
        {"parent", "child", "/xyz/openbmc_project/inventory/chassis"}};
    DeviceInventoryInfo deviceInventoryInfo(
        {{{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}},
          {{objPath, assocs}, {}}}});

    const DescriptorMap descriptorMap{
        {1,
         {{PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("ECSKU",
                           std::vector<uint8_t>{0x49, 0x35, 0x36, 0x81})}}}};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(1);

    Manager manager(busMock, deviceInventoryInfo, descriptorMap);
    manager.deviceEntryMap.emplace(
        uuid, std::make_unique<Entry>(busMock, objPath, uuid, assocs, ""));

    dbus::MctpInterfaces mismatchInterfaces{
        {uuid,
         {{"xyz.openbmc_project.Common.UUID", {{"UUID", mismatchUuid}}}}}};
    EXPECT_EQ(manager.updateEntry(1, uuid, mismatchInterfaces), std::nullopt);
    EXPECT_EQ(mockedSetSkuCallCount.load(), 0);
}

TEST(DeviceInventoryInternalThreadTest,
     updateEntrySkipsNonVendorDefinedDescriptorsAndLeavesSkuUnchanged)
{
    mockedSetSkuCallCount = 0;
    mockedSetSkuShouldThrow = true;

    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};
    const Associations assocs{
        {"parent", "child", "/xyz/openbmc_project/inventory/chassis"}};
    DeviceInventoryInfo deviceInventoryInfo(
        {{{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}},
          {{objPath, assocs}, {}}}});

    const DescriptorMap descriptorMap{
        {1,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}}}}};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(1);

    Manager manager(busMock, deviceInventoryInfo, descriptorMap);
    manager.deviceEntryMap.emplace(
        uuid,
        std::make_unique<Entry>(busMock, objPath, uuid, assocs, "0x01020304"));

    dbus::MctpInterfaces mctpInterfaces{
        {uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};
    auto updated = manager.updateEntry(1, uuid, mctpInterfaces);
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(*updated, objPath);
    EXPECT_EQ(manager.deviceEntryMap.at(uuid)->sku(), "0x01020304");
    EXPECT_EQ(mockedSetSkuCallCount.load(), 0);
}

TEST(DeviceInventoryInternalThreadTest,
     updateEntrySkipsShortVendorPayloadsAndEmptyUpdatePath)
{
    mockedSetSkuCallCount = 0;
    mockedSetSkuShouldThrow = true;

    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};
    const Associations assocs{
        {"parent", "child", "/xyz/openbmc_project/inventory/chassis"}};
    DeviceInventoryInfo deviceInventoryInfo(
        {{{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}},
          {{objPath, assocs}, {""}}}});

    const DescriptorMap descriptorMap{
        {1,
         {{PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("ECSKU", std::vector<uint8_t>{0x11, 0x22, 0x33})},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("APSKU", std::vector<uint8_t>{0x44, 0x55, 0x66})}}}};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(1);

    Manager manager(busMock, deviceInventoryInfo, descriptorMap);
    manager.deviceEntryMap.emplace(
        uuid,
        std::make_unique<Entry>(busMock, objPath, uuid, assocs, "0x01020304"));

    dbus::MctpInterfaces mctpInterfaces{
        {uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};
    auto updated = manager.updateEntry(1, uuid, mctpInterfaces);
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(*updated, objPath);
    EXPECT_EQ(manager.deviceEntryMap.at(uuid)->sku(), "0x01020304");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(mockedSetSkuCallCount.load(), 0);
    EXPECT_TRUE(manager.skuLookup.empty());
    EXPECT_TRUE(manager.updateSKUMatch.empty());
}

TEST(DeviceInventoryInternalThreadTest,
     updateSKUReusesExistingRegistrationsForDuplicateObjectPath)
{
    mockedSetSkuCallCount = 0;
    mockedSetSkuShouldThrow = false;

    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    Manager manager(busMock, DeviceInventoryInfo{}, DescriptorMap{});
    const std::string objPath{"/xyz/openbmc_project/software/entry0"};

    EXPECT_NO_THROW({ manager.updateSKU(objPath, "0x11223344"); });
    EXPECT_NO_THROW({ manager.updateSKU(objPath, "0x55667788"); });

    waitForSkuCallCount(2);
    EXPECT_EQ(manager.skuLookup.size(), 1U);
    EXPECT_EQ(manager.updateSKUMatch.size(), 1U);
    EXPECT_EQ(manager.skuLookup.at(objPath), "0x11223344");
    mockedSetSkuShouldThrow = true;
}
