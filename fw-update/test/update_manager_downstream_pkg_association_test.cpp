// SPDX-FileCopyrightText: Copyright OpenBMC Authors
// SPDX-License-Identifier: Apache-2.0
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

namespace
{

Descriptors makeDescriptors(uint8_t vendorByte)
{
    return {{PLDM_FWUP_VENDOR_DEFINED,
             std::make_tuple("VendorPN", std::vector<uint8_t>{vendorByte})}};
}

} // namespace

/** @brief Fixture with one FDP (eid 61) proxying two downstream devices.
 *         Device index 0 and 1 share identical descriptors (same
 *         transceiver part number twice), matching the vendor-provided
 *         transceiver-populated rig scenario in nvbug 6727528 where
 *         a broadcast (0xFF) job is expected when nothing targets a single
 *         device explicitly.
 */
class DownstreamPackageAssociation : public testing::Test
{
  protected:
    DownstreamPackageAssociation() :
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                      componentInfoMap, componentNameMap, false, nullptr,
                      downstreamDescriptorMap, downstreamComponentInfoMap,
                      downstreamDeviceNameMap)
    {}

    static constexpr eid fdpEid = 61;
    static constexpr DownstreamDeviceIndex device0 = 0;
    static constexpr DownstreamDeviceIndex device1 = 1;

    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    requester::Handler<requester::Request> reqHandler;
    const DescriptorMap descriptorMap;
    const ComponentInfoMap componentInfoMap;
    ComponentNameMap componentNameMap;

    const DownstreamDescriptorMap downstreamDescriptorMap{
        {fdpEid,
         {{device0, makeDescriptors(0x01)}, {device1, makeDescriptors(0x01)}}},
    };
    const DownstreamComponentInfoMap downstreamComponentInfoMap{
        {fdpEid,
         {{device0, {"0.9", 1 << PLDM_ACTIVATION_SELF_CONTAINED}},
          {device1, {"0.9", 1 << PLDM_ACTIVATION_SELF_CONTAINED}}}},
    };
    const DownstreamDeviceFwInvNameMap downstreamDeviceNameMap{
        {fdpEid,
         {{device0, "FDP_DownstreamDevice_0"},
          {device1, "FDP_DownstreamDevice_1"}}},
    };

    UpdateManager updateManager;

    const ComponentImageInfos compImageInfos{
        {10, 100, 0xFFFFFFFF, 0, 0, 200, 27, "1.0"},
    };

    // A single downstream package record whose descriptors match both
    // device0 and device1.
    const DownstreamDeviceIDRecords downstreamDeviceIDRecords{
        {0, {0}, makeDescriptors(0x01), {}},
    };

    std::vector<sdbusplus::object_path> noTargets;
    std::unordered_set<mctp_eid_t> noMatchedFDPEids;
};

TEST_F(DownstreamPackageAssociation, NoTargetingIdenticalDescriptorsBroadcasts)
{
    FirmwareDeviceIDRecords outFwDeviceIDRecords;
    std::unordered_map<DeviceIDRecordOffset, DownstreamUpdateJob>
        outDownstreamJobByRecordOffset;
    TotalComponentUpdates totalNumComponentUpdates = 0;

    DownstreamTargetList downstreamTargetList =
        updateManager.getDownstreamTargetList(downstreamDeviceNameMap,
                                              noTargets);
    EXPECT_TRUE(downstreamTargetList.empty());

    auto deviceUpdaterInfos = updateManager.associatePkgToDownstreamDevices(
        downstreamDeviceIDRecords, downstreamDescriptorMap, compImageInfos,
        downstreamTargetList, noTargets, noMatchedFDPEids, outFwDeviceIDRecords,
        outDownstreamJobByRecordOffset, totalNumComponentUpdates);

    ASSERT_EQ(deviceUpdaterInfos.size(), 1u);
    EXPECT_EQ(deviceUpdaterInfos[0].first, fdpEid);
    EXPECT_EQ(totalNumComponentUpdates, 1u);

    ASSERT_TRUE(
        outDownstreamJobByRecordOffset.contains(deviceUpdaterInfos[0].second));
    const auto& job =
        outDownstreamJobByRecordOffset.at(deviceUpdaterInfos[0].second);
    EXPECT_EQ(job.mode, DownstreamUpdateAddressMode::AllMatchingDevices);
    EXPECT_THAT(job.affectedDeviceIndices,
                testing::UnorderedElementsAre(device0, device1));
}

TEST_F(DownstreamPackageAssociation,
       TargetingBothDevicesUnderSameFDPForcesSeparateJobs)
{
    FirmwareDeviceIDRecords outFwDeviceIDRecords;
    std::unordered_map<DeviceIDRecordOffset, DownstreamUpdateJob>
        outDownstreamJobByRecordOffset;
    TotalComponentUpdates totalNumComponentUpdates = 0;

    std::vector<sdbusplus::object_path> targets{
        sdbusplus::object_path(
            "/xyz/openbmc_project/software/FDP_DownstreamDevice_0"),
        sdbusplus::object_path(
            "/xyz/openbmc_project/software/FDP_DownstreamDevice_1")};

    DownstreamTargetList downstreamTargetList =
        updateManager.getDownstreamTargetList(downstreamDeviceNameMap, targets);
    ASSERT_TRUE(downstreamTargetList.contains(fdpEid));
    EXPECT_THAT(downstreamTargetList.at(fdpEid),
                testing::UnorderedElementsAre(device0, device1));

    auto deviceUpdaterInfos = updateManager.associatePkgToDownstreamDevices(
        downstreamDeviceIDRecords, downstreamDescriptorMap, compImageInfos,
        downstreamTargetList, targets, noMatchedFDPEids, outFwDeviceIDRecords,
        outDownstreamJobByRecordOffset, totalNumComponentUpdates);

    // Even though both targeted devices share identical descriptors, an
    // explicit Redfish target selection must never collapse to a single
    // 0xFF broadcast job -- that would also update any other device behind
    // the FDP sharing the same descriptors that the user did not select.
    ASSERT_EQ(deviceUpdaterInfos.size(), 2u);
    EXPECT_EQ(totalNumComponentUpdates, 2u);
    std::set<DownstreamDeviceIndex> seenDeviceIndices;
    for (const auto& [eid, offset] : deviceUpdaterInfos)
    {
        EXPECT_EQ(eid, fdpEid);
        ASSERT_TRUE(outDownstreamJobByRecordOffset.contains(offset));
        const auto& job = outDownstreamJobByRecordOffset.at(offset);
        EXPECT_EQ(job.mode, DownstreamUpdateAddressMode::SingleDevice);
        EXPECT_EQ(job.affectedDeviceIndices.size(), 1u);
        seenDeviceIndices.insert(job.deviceIndex);
    }
    EXPECT_THAT(seenDeviceIndices,
                testing::UnorderedElementsAre(device0, device1));
}

TEST_F(DownstreamPackageAssociation, TargetingSingleDeviceYieldsOneJob)
{
    FirmwareDeviceIDRecords outFwDeviceIDRecords;
    std::unordered_map<DeviceIDRecordOffset, DownstreamUpdateJob>
        outDownstreamJobByRecordOffset;
    TotalComponentUpdates totalNumComponentUpdates = 0;

    std::vector<sdbusplus::object_path> targets{sdbusplus::object_path(
        "/xyz/openbmc_project/software/FDP_DownstreamDevice_0")};

    DownstreamTargetList downstreamTargetList =
        updateManager.getDownstreamTargetList(downstreamDeviceNameMap, targets);

    auto deviceUpdaterInfos = updateManager.associatePkgToDownstreamDevices(
        downstreamDeviceIDRecords, downstreamDescriptorMap, compImageInfos,
        downstreamTargetList, targets, noMatchedFDPEids, outFwDeviceIDRecords,
        outDownstreamJobByRecordOffset, totalNumComponentUpdates);

    ASSERT_EQ(deviceUpdaterInfos.size(), 1u);
    const auto& job =
        outDownstreamJobByRecordOffset.at(deviceUpdaterInfos[0].second);
    EXPECT_EQ(job.mode, DownstreamUpdateAddressMode::SingleDevice);
    EXPECT_EQ(job.deviceIndex, device0);
}

TEST_F(DownstreamPackageAssociation, FDPMatchedForOwnFDUpdateIsExcluded)
{
    FirmwareDeviceIDRecords outFwDeviceIDRecords;
    std::unordered_map<DeviceIDRecordOffset, DownstreamUpdateJob>
        outDownstreamJobByRecordOffset;
    TotalComponentUpdates totalNumComponentUpdates = 0;

    std::unordered_set<mctp_eid_t> matchedFDPEids{fdpEid};

    DownstreamTargetList downstreamTargetList =
        updateManager.getDownstreamTargetList(downstreamDeviceNameMap,
                                              noTargets);
    auto deviceUpdaterInfos = updateManager.associatePkgToDownstreamDevices(
        downstreamDeviceIDRecords, downstreamDescriptorMap, compImageInfos,
        downstreamTargetList, noTargets, matchedFDPEids, outFwDeviceIDRecords,
        outDownstreamJobByRecordOffset, totalNumComponentUpdates);

    // DSP0267 §12.17: an FDP already in update mode for its own firmware
    // cannot simultaneously be in update mode for its downstream devices.
    EXPECT_TRUE(deviceUpdaterInfos.empty());
    EXPECT_EQ(totalNumComponentUpdates, 0u);
}

TEST(DownstreamPackageAssociationNoComponentInfo,
     DeviceWithoutParameterTableEntryIsSkipped)
{
    // A standalone UpdateManager (rather than DownstreamPackageAssociation's
    // shared fixture instance) since two UpdateManager/Update D-Bus objects
    // can't coexist on the same object path within one test process.
    sdeventplus::Event event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    requester::Handler<requester::Request> reqHandler(
        nullptr, event, instanceIdDb, false, seconds(1), 2, milliseconds(100));
    const DescriptorMap descriptorMap;
    const ComponentInfoMap componentInfoMap;
    ComponentNameMap componentNameMap;

    constexpr eid fdpEid = 61;
    constexpr DownstreamDeviceIndex device0 = 0;
    constexpr DownstreamDeviceIndex device1 = 1;
    const DownstreamDescriptorMap downstreamDescriptorMap{
        {fdpEid,
         {{device0, makeDescriptors(0x01)}, {device1, makeDescriptors(0x01)}}},
    };
    const DownstreamComponentInfoMap noComponentInfo;
    const DownstreamDeviceFwInvNameMap downstreamDeviceNameMap{
        {fdpEid,
         {{device0, "FDP_DownstreamDevice_0"},
          {device1, "FDP_DownstreamDevice_1"}}},
    };
    const ComponentImageInfos compImageInfos{
        {10, 100, 0xFFFFFFFF, 0, 0, 200, 27, "1.0"},
    };
    const DownstreamDeviceIDRecords downstreamDeviceIDRecords{
        {0, {0}, makeDescriptors(0x01), {}},
    };
    std::vector<sdbusplus::object_path> noTargets;
    std::unordered_set<mctp_eid_t> noMatchedFDPEids;

    UpdateManager um(event, reqHandler, instanceIdDb, descriptorMap,
                     componentInfoMap, componentNameMap, false, nullptr,
                     downstreamDescriptorMap, noComponentInfo,
                     downstreamDeviceNameMap);

    FirmwareDeviceIDRecords outFwDeviceIDRecords;
    std::unordered_map<DeviceIDRecordOffset, DownstreamUpdateJob>
        outDownstreamJobByRecordOffset;
    TotalComponentUpdates totalNumComponentUpdates = 0;

    DownstreamTargetList downstreamTargetList =
        um.getDownstreamTargetList(downstreamDeviceNameMap, noTargets);
    auto deviceUpdaterInfos = um.associatePkgToDownstreamDevices(
        downstreamDeviceIDRecords, downstreamDescriptorMap, compImageInfos,
        downstreamTargetList, noTargets, noMatchedFDPEids, outFwDeviceIDRecords,
        outDownstreamJobByRecordOffset, totalNumComponentUpdates);

    // Neither device has a parameter-table entry (GetDownstreamFirmware
    // Parameters never completed for this FDP), so there's no version or
    // activation-method info to drive an update -- nothing should match.
    EXPECT_TRUE(deviceUpdaterInfos.empty());
}
