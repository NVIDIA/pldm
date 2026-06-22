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

std::atomic<int> mockedSetSwIdCallCount{0};
std::atomic<bool> mockedSetSwIdShouldThrow{true};

void setDBusPropertyMock(const pldm::utils::DBusMapping&, const std::string&)
{
    ++mockedSetSwIdCallCount;
    if (mockedSetSwIdShouldThrow)
    {
        throw std::runtime_error("mock set property failure");
    }
}

template <typename T>
void setDBusPropertyAsyncSwIdMock(
    const std::string& /*objectPath*/, const std::string& /*interface*/,
    const std::string& propertyName, const T& /*value*/,
    std::function<void(bool)> onComplete = nullptr)
{
    if (propertyName != "SoftwareId")
    {
        return;
    }
    ++mockedSetSwIdCallCount;
    if (onComplete)
    {
        onComplete(!mockedSetSwIdShouldThrow.load());
    }
}

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#define setDBusProperty setDBusPropertyMock
#define setDBusPropertyAsync setDBusPropertyAsyncSwIdMock
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "fw-update/firmware_inventory.cpp" // NOLINT(bugprone-suspicious-include)
#undef setDBusPropertyAsync
#undef setDBusProperty
#undef private

using namespace pldm;
using namespace pldm::fw_update;
using namespace pldm::fw_update::fw_inventory;
using ::testing::IsNull;
using ::testing::StrEq;

namespace
{

void waitForSwIdCallCount(int expectedCount)
{
    for (int i = 0; i < 100 && mockedSetSwIdCallCount.load() < expectedCount;
         ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

TEST(FirmwareInventoryInternalThreadTest, updateSwIdOnSignalRunsDetachedLambda)
{
    mockedSetSwIdCallCount = 0;
    mockedSetSwIdShouldThrow = true;

    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    ComponentInfoMap componentInfoMap{};
    FirmwareInventoryInfo fwInventoryInfo{};
    ComponentNameMap componentNameMap{};
    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);

    const std::string objPath{"/xyz/openbmc_project/software/CompName1"};
    manager.updateSwId(objPath, "0x0123");
    ASSERT_TRUE(manager.compIdentifierLookup.contains(objPath));

    sdbusplus::object_path signalPath{objPath};
    dbus::InterfaceMap validInterfaces{{"xyz.openbmc_project.Software.Version",
                                        {{"Version", std::string("1.0")}}}};
    auto rawBus = sdbusplus::bus::new_default();
    auto validSignal = rawBus.new_method_call("org.test", objPath.c_str(),
                                              "org.test.Interface", "Method");
    validSignal.append(signalPath, validInterfaces);
    sd_bus_message_seal(validSignal.get(), 0, 0);
    sd_bus_message_rewind(validSignal.get(), true);
    EXPECT_NO_THROW({ manager.updateSwIdOnSignal(validSignal); });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    waitForSwIdCallCount(2);
    EXPECT_GE(mockedSetSwIdCallCount.load(), 1);
}

TEST(FirmwareInventoryInternalThreadTest,
     updateSwIdOnSignalReturnsWhenVersionInterfaceIsMissing)
{
    mockedSetSwIdCallCount = 0;
    mockedSetSwIdShouldThrow = true;
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    ComponentInfoMap componentInfoMap{};
    FirmwareInventoryInfo fwInventoryInfo{};
    ComponentNameMap componentNameMap{};
    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);

    const std::string objPath{"/xyz/openbmc_project/software/CompName1"};
    manager.compIdentifierLookup.emplace(objPath, "0x0123");

    sdbusplus::object_path signalPath{objPath};
    dbus::InterfaceMap interfaces{
        {"xyz.openbmc_project.Inventory.Item", {{"Present", true}}}};
    auto rawBus = sdbusplus::bus::new_default();
    auto signal = rawBus.new_method_call("org.test", objPath.c_str(),
                                         "org.test.Interface", "Method");
    signal.append(signalPath, interfaces);
    sd_bus_message_seal(signal.get(), 0, 0);
    sd_bus_message_rewind(signal.get(), true);

    EXPECT_NO_THROW({ manager.updateSwIdOnSignal(signal); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(mockedSetSwIdCallCount.load(), 0);
}

TEST(FirmwareInventoryInternalThreadTest,
     updateSwIdOnSignalReturnsWhenLookupEntryIsMissing)
{
    mockedSetSwIdCallCount = 0;
    mockedSetSwIdShouldThrow = true;
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    ComponentInfoMap componentInfoMap{};
    FirmwareInventoryInfo fwInventoryInfo{};
    ComponentNameMap componentNameMap{};
    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);

    const std::string objPath{"/xyz/openbmc_project/software/CompName1"};
    sdbusplus::object_path signalPath{objPath};
    dbus::InterfaceMap interfaces{{"xyz.openbmc_project.Software.Version",
                                   {{"Version", std::string("1.0.0")}}}};
    auto rawBus = sdbusplus::bus::new_default();
    auto signal = rawBus.new_method_call("org.test", objPath.c_str(),
                                         "org.test.Interface", "Method");
    signal.append(signalPath, interfaces);
    sd_bus_message_seal(signal.get(), 0, 0);
    sd_bus_message_rewind(signal.get(), true);

    EXPECT_NO_THROW({ manager.updateSwIdOnSignal(signal); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(mockedSetSwIdCallCount.load(), 0);
}

TEST(FirmwareInventoryInternalThreadTest, createEntryAndUpdatePathsEarlyReturns)
{
    mockedSetSwIdShouldThrow = true;
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    FirmwareInventoryInfo fwInventoryInfo{};
    ComponentInfoMap componentInfoMap{};
    ComponentNameMap componentNameMap{};
    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);

    dbus::MctpInterfaces mctpInterfaces{};
    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const eid eid1 = 1;

    EXPECT_NO_THROW({ manager.createEntry(eid1, uuid, mctpInterfaces); });
    EXPECT_NO_THROW({ manager.updateEntry(eid1, uuid, mctpInterfaces); });
    EXPECT_NO_THROW({ manager.updateSwId("", "0x0123"); });
}

TEST(FirmwareInventoryInternalThreadTest, updateFWVersionUpdatesExistingEntry)
{
    mockedSetSwIdShouldThrow = true;
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const eid eid1 = 1;
    constexpr uint16_t compClassification = 10;
    constexpr uint16_t compIdentifier = 0x1234;
    ComponentInfoMap componentInfoMap{
        {eid1,
         {{std::make_pair(compClassification, compIdentifier),
           std::make_tuple(static_cast<uint8_t>(1), std::string("new-version"),
                           static_cast<uint16_t>(0))}}}};
    FirmwareInventoryInfo fwInventoryInfo{};
    ComponentNameMap componentNameMap{};
    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);

    const std::string objPath{"/xyz/openbmc_project/software/CompName1"};
    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(1);
    auto entry = std::make_unique<Entry>(busMock, objPath, "old-version",
                                         "0x1234", "NVIDIA");
    auto key = std::make_pair(eid1, compIdentifier);
    manager.firmwareInventoryMap.emplace(key, std::move(entry));

    manager.updateFWVersion(eid1);
    EXPECT_EQ(manager.firmwareInventoryMap[key]->version(), "new-version");

    EXPECT_NO_THROW({ manager.updateFWVersion(99); });
}

TEST(FirmwareInventoryInternalThreadTest, updateEntryUpdatesMappedInventory)
{
    mockedSetSwIdShouldThrow = true;
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const eid eid1 = 3;
    constexpr uint16_t compClassification = 20;
    constexpr uint16_t compIdentifier = 0x4321;
    ComponentInfoMap componentInfoMap{
        {eid1,
         {{std::make_pair(compClassification, compIdentifier),
           std::make_tuple(static_cast<uint8_t>(1), std::string("v3.2.1"),
                           static_cast<uint16_t>(0))}}}};
    FirmwareInventoryInfo fwInventoryInfo{};
    ComponentNameMap componentNameMap{};
    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);

    const std::string objPath{"/xyz/openbmc_project/software/CompName2"};
    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(1);
    auto entry =
        std::make_unique<Entry>(busMock, objPath, "v0.0.1", "0x4321", "NVIDIA");
    auto key = std::make_pair(eid1, compIdentifier);
    manager.firmwareInventoryMap.emplace(key, std::move(entry));

    dbus::MctpInterfaces interfaces;
    manager.updateEntry(eid1, "unused-uuid", interfaces);
    EXPECT_EQ(manager.firmwareInventoryMap[key]->version(), "v3.2.1");
}

TEST(FirmwareInventoryInternalThreadTest,
     updateSwIdOnSignalHandlesNonThrowingSetPropertyPath)
{
    mockedSetSwIdCallCount = 0;
    mockedSetSwIdShouldThrow = false;

    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    ComponentInfoMap componentInfoMap{};
    FirmwareInventoryInfo fwInventoryInfo{};
    ComponentNameMap componentNameMap{};
    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);

    const std::string objPath{"/xyz/openbmc_project/software/CompName1"};
    manager.updateSwId(objPath, "0x0456");
    waitForSwIdCallCount(1);
    ASSERT_GE(mockedSetSwIdCallCount.load(), 1);

    sdbusplus::object_path signalPath{objPath};
    dbus::InterfaceMap validInterfaces{{"xyz.openbmc_project.Software.Version",
                                        {{"Version", std::string("2.0.0")}}}};
    auto rawBus = sdbusplus::bus::new_default();
    auto validSignal = rawBus.new_method_call("org.test", objPath.c_str(),
                                              "org.test.Interface", "Method");
    validSignal.append(signalPath, validInterfaces);
    sd_bus_message_seal(validSignal.get(), 0, 0);
    sd_bus_message_rewind(validSignal.get(), true);
    EXPECT_NO_THROW({ manager.updateSwIdOnSignal(validSignal); });
    waitForSwIdCallCount(2);
    EXPECT_GE(mockedSetSwIdCallCount.load(), 2);
    mockedSetSwIdShouldThrow = true;
}

TEST(FirmwareInventoryInternalThreadTest,
     createEntryUsesUpdateMapWhenCreateMapDoesNotContainComponent)
{
    mockedSetSwIdCallCount = 0;
    mockedSetSwIdShouldThrow = false;

    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const eid eid1 = 11;
    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    constexpr uint16_t compClassification = 1;
    constexpr uint16_t compIdentifier = 0x1234;
    ComponentInfoMap componentInfoMap{
        {eid1,
         {{std::make_pair(compClassification, compIdentifier),
           std::make_tuple(static_cast<uint8_t>(1), std::string("v1"),
                           static_cast<uint16_t>(0))}}}};

    DBusIntfMatch match{"xyz.openbmc_project.Common.UUID",
                        {{"UUID", std::string(uuid)}}};
    CreateComponentIdNameMap createMap{};
    UpdateComponentIdNameMap updateMap{{compIdentifier, "CompNameUpdate"}};
    FirmwareInventoryInfo fwInventoryInfo{
        {std::make_tuple(match, std::make_tuple(createMap, updateMap))}};

    ComponentNameMap componentNameMap{};
    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);
    dbus::MctpInterfaces mctpInterfaces{
        {uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};

    manager.createEntry(eid1, uuid, mctpInterfaces);
    waitForSwIdCallCount(1);
    EXPECT_GE(mockedSetSwIdCallCount.load(), 1);
    mockedSetSwIdShouldThrow = true;
}

TEST(FirmwareInventoryInternalThreadTest,
     createEntryFallsBackToGeneratedComponentNamesWithoutConfigMatch)
{
    mockedSetSwIdShouldThrow = true;

    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const eid eid1 = 12;
    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    constexpr uint16_t compClassification = 2;
    constexpr uint16_t compIdentifier = 0x4321;
    const std::string generatedName{"GeneratedComp"};
    ComponentInfoMap componentInfoMap{
        {eid1,
         {{std::make_pair(compClassification, compIdentifier),
           std::make_tuple(static_cast<uint8_t>(1), std::string("v2"),
                           static_cast<uint16_t>(0))}}}};

    FirmwareInventoryInfo fwInventoryInfo{};
    ComponentNameMap componentNameMap{
        {eid1, {{compIdentifier, generatedName}}}};

    const std::string objPath =
        std::string("/xyz/openbmc_project/software/") + generatedName;
    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(1);

    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);
    dbus::MctpInterfaces mctpInterfaces{
        {uuid, {{"xyz.openbmc_project.Common.UUID", {{"UUID", uuid}}}}}};

    EXPECT_NO_THROW({ manager.createEntry(eid1, uuid, mctpInterfaces); });
}

TEST(FirmwareInventoryInternalThreadTest,
     updateFWVersionHandlesPresentEidWithNoMappedInventoryEntry)
{
    mockedSetSwIdShouldThrow = true;

    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const eid eid1 = 13;
    constexpr uint16_t compClassification = 3;
    constexpr uint16_t compIdentifier = 0x2222;
    ComponentInfoMap componentInfoMap{
        {eid1,
         {{std::make_pair(compClassification, compIdentifier),
           std::make_tuple(static_cast<uint8_t>(1), std::string("v3"),
                           static_cast<uint16_t>(0))}}}};

    FirmwareInventoryInfo fwInventoryInfo{};
    ComponentNameMap componentNameMap{};
    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);

    EXPECT_NO_THROW({ manager.updateFWVersion(eid1); });
}

TEST(FirmwareInventoryInternalThreadTest,
     updateFWVersionHandlesEidWithEmptyComponentMap)
{
    mockedSetSwIdShouldThrow = true;

    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);

    const eid eid1 = 14;
    ComponentInfoMap componentInfoMap{{eid1, {}}};
    FirmwareInventoryInfo fwInventoryInfo{};
    ComponentNameMap componentNameMap{};
    Manager manager(busMock, fwInventoryInfo, componentInfoMap,
                    componentNameMap);

    EXPECT_NO_THROW({ manager.updateFWVersion(eid1); });
}
