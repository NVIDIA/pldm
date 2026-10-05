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
                      emptyExpectedComponentIds, {}, {}, {}, {},
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
        downstreamTargetList, noTargets, outFwDeviceIDRecords,
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
        downstreamTargetList, targets, outFwDeviceIDRecords,
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
        downstreamTargetList, targets, outFwDeviceIDRecords,
        outDownstreamJobByRecordOffset, totalNumComponentUpdates);

    ASSERT_EQ(deviceUpdaterInfos.size(), 1u);
    const auto& job =
        outDownstreamJobByRecordOffset.at(deviceUpdaterInfos[0].second);
    EXPECT_EQ(job.mode, DownstreamUpdateAddressMode::SingleDevice);
    EXPECT_EQ(job.deviceIndex, device0);
}

TEST_F(DownstreamPackageAssociation, FDPMatchedForOwnFDUpdateStillGetsJobs)
{
    FirmwareDeviceIDRecords outFwDeviceIDRecords;
    std::unordered_map<DeviceIDRecordOffset, DownstreamUpdateJob>
        outDownstreamJobByRecordOffset;
    TotalComponentUpdates totalNumComponentUpdates = 0;

    DownstreamTargetList downstreamTargetList =
        updateManager.getDownstreamTargetList(downstreamDeviceNameMap,
                                              noTargets);
    auto deviceUpdaterInfos = updateManager.associatePkgToDownstreamDevices(
        downstreamDeviceIDRecords, downstreamDescriptorMap, compImageInfos,
        downstreamTargetList, noTargets, outFwDeviceIDRecords,
        outDownstreamJobByRecordOffset, totalNumComponentUpdates);

    ASSERT_EQ(deviceUpdaterInfos.size(), 1u);
    EXPECT_EQ(deviceUpdaterInfos[0].first, fdpEid);
    EXPECT_EQ(totalNumComponentUpdates, 1u);
    EXPECT_EQ(outDownstreamJobByRecordOffset.at(deviceUpdaterInfos[0].second)
                  .sourceRecordIndex,
              0u);
}

/** @brief Fixture for sequenceDeviceUpdaterInfos(), which only looks at
 *         record offsets, applicable components and job source records. */
class JobSequencing : public testing::Test
{
  protected:
    JobSequencing() :
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        updateManager(event, reqHandler, instanceIdDb, descriptorMap,
                      componentInfoMap, componentNameMap, false, nullptr)
    {}

    static constexpr eid fdpEid = 61;
    static constexpr eid otherFdpEid = 71;

    DeviceIDRecordOffset addFdRecord(ApplicableComponents components)
    {
        records.emplace_back(DeviceUpdateOptionFlags{}, components,
                             ComponentImageSetVersion{}, Descriptors{},
                             FirmwareDevicePackageData{});
        return records.size() - 1;
    }

    DeviceIDRecordOffset addDownstreamJob(ApplicableComponents components,
                                          size_t sourceRecordIndex,
                                          DownstreamDeviceIndex deviceIndex = 0)
    {
        auto offset = addFdRecord(components);
        DownstreamUpdateJob job;
        job.deviceIndex = deviceIndex;
        job.mode = DownstreamUpdateAddressMode::SingleDevice;
        job.affectedDeviceIndices = {deviceIndex};
        job.sourceRecordIndex = sourceRecordIndex;
        jobs.emplace(offset, job);
        return offset;
    }

    DeviceUpdaterInfos sequence(const DeviceUpdaterInfos& infos)
    {
        return updateManager.sequenceDeviceUpdaterInfos(infos, records, jobs);
    }

    const ApplicableComponents& componentsAt(DeviceIDRecordOffset offset)
    {
        return std::get<ApplicableComponents>(records[offset]);
    }

    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    requester::Handler<requester::Request> reqHandler;
    const DescriptorMap descriptorMap;
    const ComponentInfoMap componentInfoMap;
    ComponentNameMap componentNameMap;
    UpdateManager updateManager;

    FirmwareDeviceIDRecords records;
    std::unordered_map<DeviceIDRecordOffset, DownstreamUpdateJob> jobs;
};

TEST_F(JobSequencing, DownstreamThenFdpThenDownstream)
{
    auto fd = addFdRecord({1});
    auto downstream1 = addDownstreamJob({0}, 0);
    auto downstream2 = addDownstreamJob({2}, 1);

    auto sequenced =
        sequence({{fdpEid, fd}, {fdpEid, downstream1}, {fdpEid, downstream2}});

    EXPECT_THAT(sequenced,
                testing::ElementsAre(testing::Pair(fdpEid, downstream1),
                                     testing::Pair(fdpEid, fd),
                                     testing::Pair(fdpEid, downstream2)));
    EXPECT_EQ(records.size(), 3u);
}

TEST_F(JobSequencing, FdpThenDownstreamKeepsRecordsUntouched)
{
    auto fd = addFdRecord({0, 1});
    auto downstream = addDownstreamJob({2}, 0);

    auto sequenced = sequence({{fdpEid, fd}, {fdpEid, downstream}});

    EXPECT_THAT(sequenced,
                testing::ElementsAre(testing::Pair(fdpEid, fd),
                                     testing::Pair(fdpEid, downstream)));
    EXPECT_EQ(records.size(), 2u);
}

TEST_F(JobSequencing, DownstreamThenFdp)
{
    auto fd = addFdRecord({1});
    auto downstream = addDownstreamJob({0}, 0);

    auto sequenced = sequence({{fdpEid, fd}, {fdpEid, downstream}});

    EXPECT_THAT(sequenced,
                testing::ElementsAre(testing::Pair(fdpEid, downstream),
                                     testing::Pair(fdpEid, fd)));
}

TEST_F(JobSequencing, DownstreamInsideFdRecordSplitsTheFdRecord)
{
    auto fd = addFdRecord({0, 2});
    auto downstream = addDownstreamJob({1}, 0);

    auto sequenced = sequence({{fdpEid, fd}, {fdpEid, downstream}});

    ASSERT_EQ(sequenced.size(), 3u);
    EXPECT_EQ(componentsAt(sequenced[0].second), ApplicableComponents({0}));
    EXPECT_EQ(sequenced[1], DeviceUpdaterInfo(fdpEid, downstream));
    EXPECT_EQ(componentsAt(sequenced[2].second), ApplicableComponents({2}));
    EXPECT_NE(sequenced[0].second, sequenced[2].second);
    EXPECT_FALSE(jobs.contains(sequenced[0].second));
    EXPECT_FALSE(jobs.contains(sequenced[2].second));
}

TEST_F(JobSequencing, FdInsideDownstreamRecordSplitsTheDownstreamJob)
{
    auto fd = addFdRecord({1});
    auto downstream = addDownstreamJob({0, 2}, 0, 5);

    auto sequenced = sequence({{fdpEid, fd}, {fdpEid, downstream}});

    ASSERT_EQ(sequenced.size(), 3u);
    EXPECT_EQ(componentsAt(sequenced[0].second), ApplicableComponents({0}));
    EXPECT_EQ(sequenced[1], DeviceUpdaterInfo(fdpEid, fd));
    EXPECT_EQ(componentsAt(sequenced[2].second), ApplicableComponents({2}));
    for (size_t i : {0u, 2u})
    {
        ASSERT_TRUE(jobs.contains(sequenced[i].second));
        EXPECT_EQ(jobs.at(sequenced[i].second).deviceIndex, 5);
    }
}

TEST_F(JobSequencing, DeviceGroupsOfOneRecordStayTogetherAroundAnFdImage)
{
    auto fd = addFdRecord({1});
    auto groupA = addDownstreamJob({0, 2}, 0, 0);
    auto groupB = addDownstreamJob({0, 2}, 0, 1);

    auto sequenced =
        sequence({{fdpEid, fd}, {fdpEid, groupA}, {fdpEid, groupB}});

    ASSERT_EQ(sequenced.size(), 5u);
    EXPECT_EQ(jobs.at(sequenced[0].second).deviceIndex, 0);
    EXPECT_EQ(jobs.at(sequenced[1].second).deviceIndex, 1);
    EXPECT_EQ(sequenced[2], DeviceUpdaterInfo(fdpEid, fd));
    EXPECT_EQ(jobs.at(sequenced[3].second).deviceIndex, 0);
    EXPECT_EQ(jobs.at(sequenced[4].second).deviceIndex, 1);
    for (size_t i : {0u, 1u})
    {
        EXPECT_EQ(componentsAt(sequenced[i].second), ApplicableComponents({0}));
    }
    for (size_t i : {3u, 4u})
    {
        EXPECT_EQ(componentsAt(sequenced[i].second), ApplicableComponents({2}));
    }
}

TEST_F(JobSequencing, SameComponentFdBeforeDownstream)
{
    auto fd = addFdRecord({0});
    auto downstream = addDownstreamJob({0}, 0);

    auto sequenced = sequence({{fdpEid, downstream}, {fdpEid, fd}});

    EXPECT_THAT(sequenced,
                testing::ElementsAre(testing::Pair(fdpEid, fd),
                                     testing::Pair(fdpEid, downstream)));
}

TEST_F(JobSequencing, DownstreamRecordsOrderedByComponentNotPackageRecord)
{
    auto later = addDownstreamJob({3}, 0);
    auto earlier = addDownstreamJob({1}, 1);

    auto sequenced = sequence({{fdpEid, later}, {fdpEid, earlier}});

    EXPECT_THAT(sequenced, testing::ElementsAre(testing::Pair(fdpEid, earlier),
                                                testing::Pair(fdpEid, later)));
}

TEST_F(JobSequencing, EidsAreSequencedIndependently)
{
    auto fd = addFdRecord({1});
    auto downstream1 = addDownstreamJob({0}, 0);
    auto otherFd = addFdRecord({0});
    auto otherDownstream = addDownstreamJob({1}, 1);

    auto sequenced = sequence({{fdpEid, fd},
                               {otherFdpEid, otherFd},
                               {fdpEid, downstream1},
                               {otherFdpEid, otherDownstream}});

    std::vector<DeviceIDRecordOffset> fdpOrder;
    std::vector<DeviceIDRecordOffset> otherOrder;
    for (const auto& [eid, offset] : sequenced)
    {
        (eid == fdpEid ? fdpOrder : otherOrder).push_back(offset);
    }
    EXPECT_THAT(fdpOrder, testing::ElementsAre(downstream1, fd));
    EXPECT_THAT(otherOrder, testing::ElementsAre(otherFd, otherDownstream));
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

    UpdateManager um(
        event, reqHandler, instanceIdDb, descriptorMap, componentInfoMap,
        componentNameMap, false, nullptr, emptyExpectedComponentIds, {}, {}, {},
        {}, downstreamDescriptorMap, noComponentInfo, downstreamDeviceNameMap);

    FirmwareDeviceIDRecords outFwDeviceIDRecords;
    std::unordered_map<DeviceIDRecordOffset, DownstreamUpdateJob>
        outDownstreamJobByRecordOffset;
    TotalComponentUpdates totalNumComponentUpdates = 0;

    DownstreamTargetList downstreamTargetList =
        um.getDownstreamTargetList(downstreamDeviceNameMap, noTargets);
    auto deviceUpdaterInfos = um.associatePkgToDownstreamDevices(
        downstreamDeviceIDRecords, downstreamDescriptorMap, compImageInfos,
        downstreamTargetList, noTargets, outFwDeviceIDRecords,
        outDownstreamJobByRecordOffset, totalNumComponentUpdates);

    // Neither device has a parameter-table entry (GetDownstreamFirmware
    // Parameters never completed for this FDP), so there's no version or
    // activation-method info to drive an update -- nothing should match.
    EXPECT_TRUE(deviceUpdaterInfos.empty());
}

TEST(UpdateManagerRefreshCallback, IsVisibleThroughUpdateManagerBase)
{
    sdeventplus::Event event = sdeventplus::Event::get_default();
    TestInstanceIdDb instanceIdDb;
    requester::Handler<requester::Request> reqHandler(
        nullptr, event, instanceIdDb, false, seconds(1), 2, milliseconds(100));
    const DescriptorMap descriptorMap;
    const ComponentInfoMap componentInfoMap;
    ComponentNameMap componentNameMap;

    UpdateManager um(event, reqHandler, instanceIdDb, descriptorMap,
                     componentInfoMap, componentNameMap, false,
                     [](mctp_eid_t, bool, bool) -> exec::task<int> {
                         co_return PLDM_SUCCESS;
                     });

    // DeviceUpdater reads the callback through an UpdateManagerBase*.
    const UpdateManagerBase& base = um;
    EXPECT_TRUE(static_cast<bool>(base.refreshSingleEndpointCallback));
}
