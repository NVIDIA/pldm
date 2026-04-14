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
#include "libpldm/firmware_update.h"

#include "common/utils.hpp"
#include "fw-update/update_manager.hpp"
#include "requester/handler.hpp"
#include "requester/test/mock_request.hpp"
#include "test/test_instance_id.hpp"

#include <sdeventplus/test/sdevent.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldm::fw_update;
using namespace std::chrono;

class PackageAssociationEmptyTargetFiltering : public testing::Test
{
  protected:
    PackageAssociationEmptyTargetFiltering() :
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                      componentInfoMap, componentNameMap, false, nullptr)
    {}

    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    requester::Handler<requester::Request> reqHandler;
    const DescriptorMap descriptorMap;
    const ComponentInfoMap componentInfoMap;
    ComponentNameMap componentNameMap;
    UpdateManager updateManager;

    // Package to firmware device associations, the FD identifer records via
    // QueryIdentifiers command match to Package device identifer records.
    // No target filtering.

    // Device1 - ApplicableComponents{compIdentifer1, compIdentifer2}
    // Device2 - ApplicableComponents{compIdentifer1, compIdentifer3}
    const FirmwareDeviceIDRecords inFwDeviceIDRecords{
        {1,
         {0, 1},
         "VersionString1",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x50})}},
         {}},
        {1,
         {0, 2},
         "VersionString2",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x10})}},
         {}},
    };

    const CompIdentifier compIdentifer1 = 65280;
    const CompIdentifier compIdentifer2 = 80;
    const CompIdentifier compIdentifer3 = 16;

    // Component Idenitifier field is relevant for the tests
    const ComponentImageInfos compImageInfos{
        {10, compIdentifer1, 0xFFFFFFFF, 0, 0, 326, 27, "VersionString3"},
        {10, compIdentifer2, 0xFFFFFFFF, 0, 1, 353, 27, "VersionString4"},
        {10, compIdentifer3, 0xFFFFFFFF, 1, 12, 380, 27, "VersionString5"}};

    std::vector<sdbusplus::message::object_path> targets;
};

TEST_F(PackageAssociationEmptyTargetFiltering, MatchingDescriptors)
{
    constexpr eid eid1 = 13;
    constexpr eid eid2 = 24;
    const DescriptorMap descriptorMap{
        {eid1,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x50})}}},
        {eid2,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x10})}}}};

    FirmwareDeviceIDRecords outFwDeviceIDRecords;
    TotalComponentUpdates totalNumComponentUpdates = 0;

    ComponentTargetList compTargetList =
        updateManager.getComponentTargetList(componentNameMap, targets);
    auto deviceUpdaterInfos = updateManager.associatePkgToDevices(
        inFwDeviceIDRecords, descriptorMap, compImageInfos, compTargetList,
        targets, outFwDeviceIDRecords, totalNumComponentUpdates);

    DeviceUpdaterInfos expectDeviceUpdaterInfos{{eid1, 0}, {eid2, 1}};
    const FirmwareDeviceIDRecords expectFwDeviceIDRecords{
        {1,
         {0, 1},
         "VersionString1",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x50})}},
         {}},
        {1,
         {0, 2},
         "VersionString2",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x10})}},
         {}},
    };
    // All the components match for all the devices
    constexpr TotalComponentUpdates expectTotalComponents = 4;

    EXPECT_EQ(deviceUpdaterInfos, expectDeviceUpdaterInfos);
    EXPECT_EQ(outFwDeviceIDRecords, expectFwDeviceIDRecords);
    EXPECT_EQ(totalNumComponentUpdates, expectTotalComponents);
}

TEST_F(PackageAssociationEmptyTargetFiltering,
       MatchingDescriptorsMultipleDevices)
{
    constexpr eid eid1 = 13;
    constexpr eid eid2 = 14;
    constexpr eid eid3 = 24;
    const DescriptorMap descriptorMap{
        {eid1,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x50})}}},
        {eid2,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x50})}}},
        {eid3,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x10})}}}};

    FirmwareDeviceIDRecords outFwDeviceIDRecords;
    TotalComponentUpdates totalNumComponentUpdates = 0;

    ComponentTargetList compTargetList =
        updateManager.getComponentTargetList(componentNameMap, targets);
    auto deviceUpdaterInfos = updateManager.associatePkgToDevices(
        inFwDeviceIDRecords, descriptorMap, compImageInfos, compTargetList,
        targets, outFwDeviceIDRecords, totalNumComponentUpdates);

    DeviceUpdaterInfos expectDeviceUpdaterInfos{
        {eid2, 0}, {eid1, 1}, {eid3, 2}};
    const FirmwareDeviceIDRecords expectFwDeviceIDRecords{
        {1,
         {0, 1},
         "VersionString1",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x50})}},
         {}},
        {1,
         {0, 1},
         "VersionString1",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x50})}},
         {}},
        {1,
         {0, 2},
         "VersionString2",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x10})}},
         {}},
    };
    // 3 device * 2 components
    constexpr TotalComponentUpdates expectTotalComponents = 6;

    EXPECT_EQ(deviceUpdaterInfos, expectDeviceUpdaterInfos);
    EXPECT_EQ(outFwDeviceIDRecords, expectFwDeviceIDRecords);
    EXPECT_EQ(totalNumComponentUpdates, expectTotalComponents);
}

class PackageAssociationTargetFiltering : public testing::Test
{
  protected:
    PackageAssociationTargetFiltering() :
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                      componentInfoMap, componentNameMap, false, nullptr)
    {}

    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    requester::Handler<requester::Request> reqHandler;
    const ComponentInfoMap componentInfoMap;
    UpdateManager updateManager;

    // Device1 - ApplicableComponents{compIdentifer1, compIdentifer2}
    // Device2 - ApplicableComponents{compIdentifer1, compIdentifer3}
    const FirmwareDeviceIDRecords inFwDeviceIDRecords{
        {1,
         {0, 1},
         "VersionString1",
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}}},
         {}},
        {1,
         {0, 2},
         "VersionString2",
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x01}}},
         {}},
    };

    // Discovered two endpoints that match with the Device 1 & Device2
    // descriptors.
    const eid eid1 = 1;
    const eid eid2 = 2;
    const DescriptorMap descriptorMap{
        {eid1,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}}}},
        {eid2,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x01}}}}};

    const CompIdentifier compIdentifer1 = 65280;
    const CompIdentifier compIdentifer2 = 80;
    const CompIdentifier compIdentifer3 = 16;

    // ComponentImageInformationArea from the package, what matter for the
    // test is the component identifers and order of the components.
    const ComponentImageInfos compImageInfos{
        {10, compIdentifer1, 0xFFFFFFFF, 0, 0, 326, 27, "VersionString3"},
        {10, compIdentifer2, 0xFFFFFFFF, 1, 12, 380, 27, "VersionString4"},
        {10, compIdentifer3, 0xFFFFFFFF, 0, 1, 353, 27, "VersionString5"},
    };

    // ComponentNameMap is needed for target filtering feature and maps the
    // firmware targets to the right PLDM device and components.
    ComponentNameMap componentNameMap{
        {eid1, {{65280, "ERoT_FPGA_Firmware"}, {80, "FPGAFirmware"}}},
        {eid2, {{65280, "ERoT_HMC_Firmware"}, {16, "HMCFirmware"}}}};
};

TEST_F(PackageAssociationTargetFiltering, MatchingTwoComponents)
{
    const std::string erotFPGAFirmware =
        "/xyz/openbmc_project/software/ERoT_FPGA_Firmware";
    const std::string erotHMCFirmware =
        "/xyz/openbmc_project/software/ERoT_HMC_Firmware";
    std::vector<sdbusplus::message::object_path> targets{erotFPGAFirmware,
                                                         erotHMCFirmware};
    FirmwareDeviceIDRecords outFwDeviceIDRecords{};
    TotalComponentUpdates totalNumComponentUpdates = 0;

    ComponentTargetList compTargetList =
        updateManager.getComponentTargetList(componentNameMap, targets);
    auto deviceUpdaterInfos = updateManager.associatePkgToDevices(
        inFwDeviceIDRecords, descriptorMap, compImageInfos, compTargetList,
        targets, outFwDeviceIDRecords, totalNumComponentUpdates);

    const FirmwareDeviceIDRecords expectFwDeviceIDRecords{
        {1,
         {0},
         "VersionString1",
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}}},
         {}},
        {1,
         {0},
         "VersionString2",
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x01}}},
         {}},
    };
    DeviceUpdaterInfos expectDeviceUpdaterInfos{{eid1, 0}, {eid2, 1}};
    constexpr TotalComponentUpdates expectTotalComponents = 2;

    EXPECT_EQ(totalNumComponentUpdates, expectTotalComponents);
    EXPECT_EQ(outFwDeviceIDRecords, expectFwDeviceIDRecords);
    EXPECT_EQ(deviceUpdaterInfos, expectDeviceUpdaterInfos);
}

TEST_F(PackageAssociationTargetFiltering, MatchingOneComponent)
{
    const std::string erotHMCFirmware =
        "/xyz/openbmc_project/software/ERoT_HMC_Firmware";
    std::vector<sdbusplus::message::object_path> targets{erotHMCFirmware};
    FirmwareDeviceIDRecords outFwDeviceIDRecords{};
    TotalComponentUpdates totalNumComponentUpdates = 0;

    ComponentTargetList compTargetList =
        updateManager.getComponentTargetList(componentNameMap, targets);
    auto deviceUpdaterInfos = updateManager.associatePkgToDevices(
        inFwDeviceIDRecords, descriptorMap, compImageInfos, compTargetList,
        targets, outFwDeviceIDRecords, totalNumComponentUpdates);

    const FirmwareDeviceIDRecords expectFwDeviceIDRecords{
        {1,
         {0},
         "VersionString2",
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x01}}},
         {}},
    };
    DeviceUpdaterInfos expectDeviceUpdaterInfos{{eid2, 0}};
    constexpr TotalComponentUpdates expectTotalComponents = 1;

    EXPECT_EQ(totalNumComponentUpdates, expectTotalComponents);
    EXPECT_EQ(outFwDeviceIDRecords, expectFwDeviceIDRecords);
    EXPECT_EQ(deviceUpdaterInfos, expectDeviceUpdaterInfos);
}

class PackageAssociationMultipleDescSameType : public testing::Test
{
  protected:
    PackageAssociationMultipleDescSameType() :
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                      componentInfoMap, componentNameMap, false, nullptr)
    {}

    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    requester::Handler<requester::Request> reqHandler;
    const DescriptorMap descriptorMap;
    const ComponentInfoMap componentInfoMap;
    ComponentNameMap componentNameMap;
    UpdateManager updateManager;

    const FirmwareDeviceIDRecords inFwDeviceIDRecords{
        {1,
         {0, 1},
         "VersionString1",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x10, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x50})}},
         {}},
        {1,
         {0, 2},
         "VersionString2",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x10})}},
         {}}};

    const CompIdentifier compIdentifer1 = 65280;
    const CompIdentifier compIdentifer2 = 80;
    const CompIdentifier compIdentifer3 = 16;

    // Component Idenitifier field is relevant for the tests
    const ComponentImageInfos compImageInfos{
        {10, compIdentifer1, 0xFFFFFFFF, 0, 0, 326, 27, "VersionString3"},
        {10, compIdentifer2, 0xFFFFFFFF, 0, 1, 353, 27, "VersionString4"},
        {10, compIdentifer3, 0xFFFFFFFF, 1, 12, 380, 27, "VersionString5"}};

    std::vector<sdbusplus::message::object_path> targets;
};

TEST_F(PackageAssociationMultipleDescSameType, MultipleDescriptorsMatch)
{
    constexpr eid eid1 = 13;
    constexpr eid eid2 = 24;
    const DescriptorMap descriptorMap{
        {eid1,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x10, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x50})}}},
        {eid2,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x10})}}}};

    FirmwareDeviceIDRecords outFwDeviceIDRecords;
    TotalComponentUpdates totalNumComponentUpdates = 0;

    ComponentTargetList compTargetList =
        updateManager.getComponentTargetList(componentNameMap, targets);
    auto deviceUpdaterInfos = updateManager.associatePkgToDevices(
        inFwDeviceIDRecords, descriptorMap, compImageInfos, compTargetList,
        targets, outFwDeviceIDRecords, totalNumComponentUpdates);

    DeviceUpdaterInfos expectDeviceUpdaterInfos{{eid1, 0}, {eid2, 1}};
    const FirmwareDeviceIDRecords expectFwDeviceIDRecords{
        {1,
         {0, 1},
         "VersionString1",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x10, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x50})}},
         {}},
        {1,
         {0, 2},
         "VersionString2",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x10})}},
         {}},
    };
    // All the components match for all the devices
    constexpr TotalComponentUpdates expectTotalComponents = 4;

    EXPECT_EQ(deviceUpdaterInfos, expectDeviceUpdaterInfos);
    EXPECT_EQ(outFwDeviceIDRecords, expectFwDeviceIDRecords);
    EXPECT_EQ(totalNumComponentUpdates, expectTotalComponents);
}

TEST_F(PackageAssociationMultipleDescSameType, MultipleDescriptorsNoMatch)
{
    constexpr eid eid1 = 13;
    constexpr eid eid2 = 24;
    const DescriptorMap descriptorMap{
        {eid1,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x10, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x51})},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("SKU",
                           std::vector<uint8_t>{0x50, 0x51, 0x52, 0x53})}}},
        {eid2,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,

           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x10})}}}};

    FirmwareDeviceIDRecords outFwDeviceIDRecords;
    TotalComponentUpdates totalNumComponentUpdates = 0;

    ComponentTargetList compTargetList =
        updateManager.getComponentTargetList(componentNameMap, targets);
    auto deviceUpdaterInfos = updateManager.associatePkgToDevices(
        inFwDeviceIDRecords, descriptorMap, compImageInfos, compTargetList,
        targets, outFwDeviceIDRecords, totalNumComponentUpdates);

    DeviceUpdaterInfos expectDeviceUpdaterInfos{{eid1, 0}, {eid2, 1}};
    const FirmwareDeviceIDRecords expectFwDeviceIDRecords{
        {1,
         {0, 1},
         "VersionString1",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x10, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x50})},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("SKU",
                           std::vector<uint8_t>{0x50, 0x51, 0x52, 0x53})}},
         {}},
        {1,
         {0, 2},
         "VersionString2",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x10})}},
         {}}};
    // All the components match for all the devices
    constexpr TotalComponentUpdates expectTotalComponents = 4;

    EXPECT_NE(deviceUpdaterInfos, expectDeviceUpdaterInfos);
    EXPECT_NE(outFwDeviceIDRecords, expectFwDeviceIDRecords);
    EXPECT_NE(totalNumComponentUpdates, expectTotalComponents);
}

// Test case for vendor-defined descriptors with different insertion order
// between package and device. Package has [PSID, APSKU] order while device
// reports [APSKU, PSID]. The match should succeed because the descriptorsMatch
// function performs order-independent matching.
TEST_F(PackageAssociationMultipleDescSameType,
       VendorDefinedDescriptorsDifferentOrder)
{
    // Package descriptors: PSID first, then APSKU
    const FirmwareDeviceIDRecords pkgFwDeviceIDRecords{
        {1,
         {0, 1},
         "VersionString1",
         {{PLDM_FWUP_PCI_VENDOR_ID, std::vector<uint8_t>{0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple(
               "PSID", std::vector<uint8_t>{0x4d, 0x54, 0x5f, 0x30, 0x30, 0x30,
                                            0x30, 0x30, 0x30, 0x31, 0x35, 0x35,
                                            0x30, 0x00, 0x00, 0x00})},
          {PLDM_FWUP_PCI_DEVICE_ID, std::vector<uint8_t>{0x00, 0x00}},
          {PLDM_FWUP_PCI_SUBSYSTEM_VENDOR_ID, std::vector<uint8_t>{0x00, 0x00}},
          {PLDM_FWUP_PCI_SUBSYSTEM_ID, std::vector<uint8_t>{0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("APSKU",
                           std::vector<uint8_t>{0x50, 0x15, 0x00, 0x00})}},
         {}}};

    constexpr eid eid1 = 53;
    // Device descriptors: APSKU first, then PSID (different order than package)
    const DescriptorMap deviceDescriptorMap{
        {eid1,
         {{PLDM_FWUP_PCI_VENDOR_ID, std::vector<uint8_t>{0x00, 0x00}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("APSKU",
                           std::vector<uint8_t>{0x50, 0x15, 0x00, 0x00})},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple(
               "PSID", std::vector<uint8_t>{0x4d, 0x54, 0x5f, 0x30, 0x30, 0x30,
                                            0x30, 0x30, 0x30, 0x31, 0x35, 0x35,
                                            0x30, 0x00, 0x00, 0x00})},
          {PLDM_FWUP_PCI_DEVICE_ID, std::vector<uint8_t>{0x00, 0x00}},
          {PLDM_FWUP_PCI_SUBSYSTEM_VENDOR_ID, std::vector<uint8_t>{0x00, 0x00}},
          {PLDM_FWUP_PCI_SUBSYSTEM_ID, std::vector<uint8_t>{0x00, 0x00}}}}};

    FirmwareDeviceIDRecords outFwDeviceIDRecords;
    TotalComponentUpdates totalNumComponentUpdates = 0;

    ComponentTargetList compTargetList =
        updateManager.getComponentTargetList(componentNameMap, targets);
    auto deviceUpdaterInfos = updateManager.associatePkgToDevices(
        pkgFwDeviceIDRecords, deviceDescriptorMap, compImageInfos,
        compTargetList, targets, outFwDeviceIDRecords,
        totalNumComponentUpdates);

    // Should match despite different vendor-defined descriptor order
    DeviceUpdaterInfos expectDeviceUpdaterInfos{{eid1, 0}};
    constexpr TotalComponentUpdates expectTotalComponents = 2;

    EXPECT_EQ(deviceUpdaterInfos, expectDeviceUpdaterInfos);
    EXPECT_EQ(totalNumComponentUpdates, expectTotalComponents);
}

// Per DSP0267 the first package record matching an FD wins; later matches for
// the same FD must be ignored so totalNumComponentUpdates stays consistent
// with the single DeviceUpdater created per EID.
class PackageAssociationDuplicateRecordMatch : public testing::Test
{
  protected:
    PackageAssociationDuplicateRecordMatch() :
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                      componentInfoMap, componentNameMap, false, nullptr)
    {}

    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    requester::Handler<requester::Request> reqHandler;
    const DescriptorMap descriptorMap;
    const ComponentInfoMap componentInfoMap;
    ComponentNameMap componentNameMap;
    UpdateManager updateManager;

    const CompIdentifier compIdentifier1 = 65280;
    const CompIdentifier compIdentifier2 = 80;
    const CompIdentifier compIdentifier3 = 16;

    const ComponentImageInfos compImageInfos{
        {10, compIdentifier1, 0xFFFFFFFF, 0, 0, 326, 27, "VersionString3"},
        {10, compIdentifier2, 0xFFFFFFFF, 0, 1, 353, 27, "VersionString4"},
        {10, compIdentifier3, 0xFFFFFFFF, 1, 12, 380, 27, "VersionString5"}};

    std::vector<sdbusplus::message::object_path> targets;
};

// Two package records share identical UUID+IANA descriptors so both match
// eid1. The first (record 0) is selected; the second (record 1) is skipped
// per DSP0267 first-match-wins.
TEST_F(PackageAssociationDuplicateRecordMatch, FirstMatchingRecordWins)
{
    const FirmwareDeviceIDRecords inFwDeviceIDRecords{
        {1,
         {0, 1},
         "VersionString1",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}}},
         {}},
        {1,
         {0, 2},
         "VersionString2",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}}},
         {}},
    };

    constexpr eid eid1 = 13;
    const DescriptorMap descriptorMap{
        {eid1,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}}}}};

    FirmwareDeviceIDRecords outFwDeviceIDRecords;
    TotalComponentUpdates totalNumComponentUpdates = 0;

    ComponentTargetList compTargetList =
        updateManager.getComponentTargetList(componentNameMap, targets);
    auto deviceUpdaterInfos = updateManager.associatePkgToDevices(
        inFwDeviceIDRecords, descriptorMap, compImageInfos, compTargetList,
        targets, outFwDeviceIDRecords, totalNumComponentUpdates);

    // Only the first record is associated; second match for same EID skipped.
    DeviceUpdaterInfos expectDeviceUpdaterInfos{{eid1, 0}};
    EXPECT_EQ(deviceUpdaterInfos, expectDeviceUpdaterInfos);
    ASSERT_EQ(outFwDeviceIDRecords.size(), 1);
    // First record's applicable components are {0, 1} -> 2 components.
    EXPECT_EQ(totalNumComponentUpdates, 2);
}

// eid1 matches records 0 and 1 (deduped to record 0); eid2 matches record 2.
// Confirms the dedup is per-EID and unrelated devices proceed normally.
TEST_F(PackageAssociationDuplicateRecordMatch,
       DuplicateForOneEidDoesNotAffectOthers)
{
    const FirmwareDeviceIDRecords inFwDeviceIDRecords{
        {1,
         {0, 1},
         "VersionString1",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}}},
         {}},
        {1,
         {0, 2},
         "VersionString2",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}}},
         {}},
        {1,
         {0, 2},
         "VersionString3",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0xAA, 0xBB, 0xCC, 0xDD, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x01}}},
         {}},
    };

    constexpr eid eid1 = 13;
    constexpr eid eid2 = 24;
    const DescriptorMap descriptorMap{
        {eid1,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}}}},
        {eid2,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0xAA, 0xBB, 0xCC, 0xDD, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x01}}}}};

    FirmwareDeviceIDRecords outFwDeviceIDRecords;
    TotalComponentUpdates totalNumComponentUpdates = 0;

    ComponentTargetList compTargetList =
        updateManager.getComponentTargetList(componentNameMap, targets);
    auto deviceUpdaterInfos = updateManager.associatePkgToDevices(
        inFwDeviceIDRecords, descriptorMap, compImageInfos, compTargetList,
        targets, outFwDeviceIDRecords, totalNumComponentUpdates);

    // eid1 -> first match (record 0, components {0,1}); eid2 -> record 2
    // (components {0,2}). Total = 4, not 6.
    DeviceUpdaterInfos expectDeviceUpdaterInfos{{eid1, 0}, {eid2, 1}};
    EXPECT_EQ(deviceUpdaterInfos, expectDeviceUpdaterInfos);
    ASSERT_EQ(outFwDeviceIDRecords.size(), 2);
    EXPECT_EQ(totalNumComponentUpdates, 4);
}

TEST_F(PackageAssociationTargetFiltering,
       NonMatchingTargetsSkipOtherwiseMatchingDescriptors)
{
    std::vector<sdbusplus::message::object_path> invalidTargets{
        sdbusplus::message::object_path(
            "/xyz/openbmc_project/software/NotAMappedComponent")};
    FirmwareDeviceIDRecords outFwDeviceIDRecords{};
    TotalComponentUpdates totalNumComponentUpdates = 0;

    ComponentTargetList compTargetList =
        updateManager.getComponentTargetList(componentNameMap, invalidTargets);
    EXPECT_TRUE(compTargetList.empty());

    auto deviceUpdaterInfos = updateManager.associatePkgToDevices(
        inFwDeviceIDRecords, descriptorMap, compImageInfos, compTargetList,
        invalidTargets, outFwDeviceIDRecords, totalNumComponentUpdates);

    EXPECT_TRUE(deviceUpdaterInfos.empty());
    EXPECT_TRUE(outFwDeviceIDRecords.empty());
    EXPECT_EQ(totalNumComponentUpdates, 0u);
}

TEST_F(PackageAssociationTargetFiltering,
       MatchingDescriptorWithNoSelectedComponentsIsDropped)
{
    const FirmwareDeviceIDRecords filteredRecords{
        {1,
         {0},
         "VersionString1",
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}}},
         {}},
    };
    std::vector<sdbusplus::message::object_path> targets{
        sdbusplus::message::object_path(
            "/xyz/openbmc_project/software/FPGAFirmware")};
    FirmwareDeviceIDRecords outFwDeviceIDRecords{};
    TotalComponentUpdates totalNumComponentUpdates = 0;

    ComponentTargetList compTargetList =
        updateManager.getComponentTargetList(componentNameMap, targets);
    ASSERT_TRUE(compTargetList.contains(eid1));
    ASSERT_EQ(compTargetList.at(eid1).size(), 1u);

    auto deviceUpdaterInfos = updateManager.associatePkgToDevices(
        filteredRecords, descriptorMap, compImageInfos, compTargetList, targets,
        outFwDeviceIDRecords, totalNumComponentUpdates);

    EXPECT_TRUE(deviceUpdaterInfos.empty());
    EXPECT_TRUE(outFwDeviceIDRecords.empty());
    EXPECT_EQ(totalNumComponentUpdates, 0u);
}
