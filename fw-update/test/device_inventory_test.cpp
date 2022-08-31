#include "libpldm/firmware_update.h"

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

TEST(Manager, SingleMatch)
{
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const UUID uuid{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};
    DeviceInventoryInfo deviceInventoryInfo{
        {uuid,
         {objPath,
          {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"}}}}};
    const EID eid = 1;
    const DescriptorMap descriptorMap{
        {eid,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("ECSKU",
                           std::vector<uint8_t>{0x49, 0x35, 0x36, 0x81})}}}};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(1);

    Manager manager(busMock, deviceInventoryInfo, descriptorMap);
    EXPECT_EQ(manager.createEntry(eid, uuid), objPath);
}

TEST(Manager, MultipleMatch)
{
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const UUID uuid1{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const UUID uuid2{"ad4c8360-c54c-11eb-8529-0242ac130004"};
    const std::string objPath1{"/xyz/openbmc_project/inventory/chassis/bmc1"};
    const std::string objPath2{"/xyz/openbmc_project/inventory/chassis/bmc2"};
    DeviceInventoryInfo deviceInventoryInfo{
        {uuid1,
         {objPath1,
          {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"}}}},
        {uuid2,
         {objPath2,
          {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"}}}}};
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

    Manager manager(busMock, deviceInventoryInfo, descriptorMap);
    EXPECT_EQ(manager.createEntry(eid1, uuid1), objPath1);
    EXPECT_EQ(manager.createEntry(eid2, uuid2), objPath2);
}

TEST(Manager, NoMatch)
{
    sdbusplus::SdBusMock sdbusMock;
    auto busMock = sdbusplus::get_mocked_new(&sdbusMock);
    const UUID uuid1{"ad4c8360-c54c-11eb-8529-0242ac130003"};
    const std::string objPath{"/xyz/openbmc_project/inventory/chassis/bmc"};
    DeviceInventoryInfo deviceInventoryInfo{
        {uuid1,
         {objPath,
          {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"}}}}};
    const EID eid1 = 1;
    const DescriptorMap descriptorMap{
        {eid1,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}}}}};

    EXPECT_CALL(sdbusMock, sd_bus_emit_object_added(IsNull(), StrEq(objPath)))
        .Times(0);

    Manager manager(busMock, deviceInventoryInfo, descriptorMap);
    // Non-matching MCTP UUID, not present in the config entry
    const UUID uuid2{"ad4c8360-c54c-11eb-8529-0242ac130004"};
    const std::string emptyString{};
    EXPECT_EQ(manager.createEntry(eid1, uuid2), emptyString);
}