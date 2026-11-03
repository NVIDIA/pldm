/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "common/test/mocked_utils.hpp"
#include "oem/nvidia/common/em_device.hpp"

#include <limits>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldm::em_device;
using pldm::utils::PropertyMap;
using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;

namespace
{

constexpr auto emService = "xyz.openbmc_project.EntityManager";

pldm::utils::GetSubTreeResponse subtreeOf(const std::vector<std::string>& paths,
                                          const std::string& intf)
{
    pldm::utils::GetSubTreeResponse subtree;
    for (const auto& path : paths)
    {
        subtree.emplace_back(
            path, std::vector<std::pair<std::string, std::vector<std::string>>>{
                      {emService, {intf}}});
    }
    return subtree;
}

/** @brief A mock D-Bus with NvDevice records at @p paths, and records of
 *         other interfaces at @p others (interface -> paths), whose
 *         properties are looked up as "<path>|<interface>". */
class EmDeviceDevices : public ::testing::Test
{
  protected:
    void publish(const std::vector<std::string>& paths,
                 std::map<std::string, PropertyMap> properties,
                 std::map<std::string, std::vector<std::string>> others = {})
    {
        props = std::move(properties);
        others[nvDeviceIntf] = paths;
        EXPECT_CALL(dbus, getSubtree(_, _, _))
            .WillRepeatedly(Invoke([others](const std::string&, int,
                                            const std::vector<std::string>& i) {
                auto it = i.empty() ? others.end() : others.find(i.front());
                if (it == others.end())
                {
                    return pldm::utils::GetSubTreeResponse{};
                }
                return subtreeOf(it->second, it->first);
            }));
        EXPECT_CALL(dbus, getDbusPropertiesVariant(_, _, _))
            .WillRepeatedly(
                Invoke([this](const char*, const char* path, const char* intf) {
                    auto it = props.find(std::string(path) + "|" + intf);
                    if (it == props.end())
                    {
                        throw std::runtime_error("no such interface");
                    }
                    return it->second;
                }));
    }

    MockdBusHandler dbus;
    std::map<std::string, PropertyMap> props;
};

} // namespace

TEST_F(EmDeviceDevices, mergesTheTwoRecordsOfADevice)
{
    // records are visited in path order: the topology item comes first
    const std::string topo = "/inventory/platform/HGX_CPU_0";
    const std::string entity = "/inventory/system/board/HGX_IRoT_CPU_0";
    publish({topo, entity},
            {{topo + "|" + nvDeviceIntf,
              {{"Name", std::string("HGX_CPU_0")},
               {"Kind", std::string("cpu")},
               {"Instance", uint64_t{0}},
               {"EID", uint64_t{14}},
               {"ParentChassis", std::string("HGX_ProcessorModule_0")}}},
             {topo + "|" + transportIntf,
              {{"Medium", std::string("usb")}, {"PoolStartEID", uint64_t{40}}}},
             {entity + "|" + nvDeviceIntf,
              {{"Name", std::string("HGX_CPU_0")},
               {"Kind", std::string("cpu")},
               {"InstanceNumber", uint64_t{0}},
               {"CpuIndex", uint64_t{2}},
               {"EID", uint64_t{14}}}},
             {entity + "|" + transportIntf,
              {{"Medium", std::string("i2c")},
               {"PoolStartEID", uint64_t{50}},
               {"PoolEndEID", uint64_t{42}}}}});

    auto out = devices(dbus);
    ASSERT_EQ(1U, out.size());
    EXPECT_EQ("HGX_CPU_0", out[0].name);
    EXPECT_EQ("cpu", out[0].kind);
    EXPECT_EQ("HGX_ProcessorModule_0", out[0].parentChassis);
    EXPECT_EQ(std::optional<uint64_t>(14), out[0].eid);
    EXPECT_EQ(std::optional<uint64_t>(0), out[0].instance);
    // taken from the second record, the first lacks it
    EXPECT_EQ(std::optional<uint64_t>(2), out[0].cpuIndex);
    // the transport of the first record wins field by field; the pool end
    // only the second record has is taken from it
    EXPECT_EQ("usb", out[0].medium);
    EXPECT_EQ(std::optional<uint64_t>(40), out[0].poolStartEid);
    EXPECT_EQ(std::optional<uint64_t>(42), out[0].poolEndEid);

    auto byName = deviceByName("HGX_CPU_0", dbus);
    ASSERT_TRUE(byName);
    EXPECT_EQ(std::optional<uint64_t>(14), byName->eid);
    EXPECT_FALSE(deviceByName("HGX_CPU_1", dbus));
}

TEST(EmDevicePathSegment, sanitisesLikeEntityManager)
{
    EXPECT_EQ("HGX_CPU_0", dbusPathSegment("HGX_CPU_0"));
    EXPECT_EQ("HGX_CX_9_0", dbusPathSegment("HGX_CX-9_0"));
    EXPECT_EQ("NVSwitch_0_1", dbusPathSegment("NVSwitch 0.1"));
    EXPECT_EQ("", dbusPathSegment(""));
}

TEST_F(EmDeviceDevices, findsADeviceByItsPathSegment)
{
    const std::string cx = "/inventory/platform/HGX_CX_9_0";
    publish({cx}, {{cx + "|" + nvDeviceIntf,
                    {{"Name", std::string("HGX_CX-9_0")},
                     {"Kind", std::string("cx9_io")},
                     {"EID", uint64_t{30}}}}});

    auto dev = deviceByPathSegment("HGX_CX_9_0", dbus);
    ASSERT_TRUE(dev);
    // the record keeps the raw name, which the PLDMDevice record refers to
    EXPECT_EQ("HGX_CX-9_0", dev->name);
    EXPECT_EQ(std::optional<uint64_t>(30), dev->eid);
    EXPECT_FALSE(deviceByPathSegment("HGX_CX-9_0", dbus));
    EXPECT_FALSE(deviceByName("HGX_CX_9_0", dbus));
}

TEST_F(EmDeviceDevices, keepsTheFirstValueOnConflict)
{
    const std::string a = "/inventory/a/HGX_CPU_0";
    const std::string b = "/inventory/b/HGX_CPU_0";
    publish({a, b},
            {{a + "|" + nvDeviceIntf,
              {{"Name", std::string("HGX_CPU_0")},
               {"Kind", std::string("cpu")},
               {"Instance", uint64_t{0}},
               {"EID", uint64_t{14}},
               {"ParentChassis", std::string("HGX_ProcessorModule_0")}}},
             {b + "|" + nvDeviceIntf,
              {{"Name", std::string("HGX_CPU_0")},
               {"Kind", std::string("gpu")},
               {"Instance", uint64_t{1}},
               {"CpuIndex", uint64_t{1}},
               {"EID", uint64_t{24}},
               {"ParentChassis", std::string("HGX_ProcessorModule_1")}}}});

    auto out = devices(dbus);
    ASSERT_EQ(1U, out.size());
    EXPECT_EQ("cpu", out[0].kind);
    EXPECT_EQ("HGX_ProcessorModule_0", out[0].parentChassis);
    EXPECT_EQ(std::optional<uint64_t>(14), out[0].eid);
    EXPECT_EQ(std::optional<uint64_t>(0), out[0].instance);
    EXPECT_EQ(std::optional<uint64_t>(1), out[0].cpuIndex);
}

TEST_F(EmDeviceDevices, dropsAnEidOutOfRange)
{
    const std::string broadcast = "/inventory/platform/HGX_GPU_0";
    const std::string wide = "/inventory/platform/HGX_GPU_1";
    const std::string last = "/inventory/platform/HGX_GPU_2";
    const std::string unnamed = "/inventory/platform/Unnamed";
    publish({broadcast, wide, last, unnamed},
            {{broadcast + "|" + nvDeviceIntf,
              {{"Name", std::string("HGX_GPU_0")},
               {"Kind", std::string("gpu")},
               {"EID", uint64_t{0xFF}}}},
             {wide + "|" + nvDeviceIntf,
              {{"Name", std::string("HGX_GPU_1")},
               {"Kind", std::string("gpu")},
               {"EID", uint64_t{264}}}},
             {last + "|" + nvDeviceIntf,
              {{"Name", std::string("HGX_GPU_2")},
               {"Kind", std::string("gpu")},
               {"EID", uint64_t{0xFE}}}},
             {unnamed + "|" + nvDeviceIntf, {{"Kind", std::string("gpu")}}}});

    auto out = devices(dbus);
    ASSERT_EQ(3U, out.size());
    EXPECT_EQ("HGX_GPU_0", out[0].name);
    EXPECT_FALSE(out[0].eid);
    EXPECT_EQ("HGX_GPU_1", out[1].name);
    EXPECT_FALSE(out[1].eid);
    // the largest EID a device can have
    EXPECT_EQ("HGX_GPU_2", out[2].name);
    EXPECT_EQ(std::optional<uint64_t>(0xFE), out[2].eid);
}

TEST_F(EmDeviceDevices, dropsAnInvalidInstanceOrCpuIndex)
{
    const std::string negative = "/inventory/platform/HGX_CPU_0";
    const std::string fraction = "/inventory/platform/HGX_CPU_1";
    const std::string text = "/inventory/platform/HGX_CPU_2";
    const std::string array = "/inventory/platform/HGX_CPU_3";
    const std::string large = "/inventory/platform/HGX_CPU_4";
    publish(
        {negative, fraction, text, array, large},
        {{negative + "|" + nvDeviceIntf,
          {{"Name", std::string("HGX_CPU_0")},
           {"Kind", std::string("cpu")},
           {"EID", uint64_t{14}},
           {"Instance", int64_t{-1}},
           {"CpuIndex", int32_t{-2}}}},
         // an invalid Instance falls back to InstanceNumber
         {fraction + "|" + nvDeviceIntf,
          {{"Name", std::string("HGX_CPU_1")},
           {"Kind", std::string("cpu")},
           {"EID", uint64_t{15}},
           {"Instance", 1.5},
           {"InstanceNumber", uint64_t{1}},
           {"CpuIndex", 2.5}}},
         {text + "|" + nvDeviceIntf,
          {{"Name", std::string("HGX_CPU_2")},
           {"Kind", std::string("cpu")},
           {"EID", uint64_t{16}},
           {"InstanceNumber", std::string("two")},
           {"CpuIndex", std::string("99999999999999999999")}}},
         {array + "|" + nvDeviceIntf,
          {{"Name", std::string("HGX_CPU_3")},
           {"Kind", std::string("cpu")},
           {"EID", uint64_t{17}},
           {"Instance", std::vector<uint64_t>{3}},
           {"CpuIndex", std::string("0x3")}}},
         // Instance and CpuIndex are not bounded like an EID
         {large + "|" + nvDeviceIntf,
          {{"Name", std::string("HGX_CPU_4")},
           {"Kind", std::string("cpu")},
           {"Instance", uint64_t{0x100}},
           {"CpuIndex", uint64_t{0x10000}}}}});

    auto out = devices(dbus);
    ASSERT_EQ(5U, out.size());
    // the record is kept with the rest of its fields
    EXPECT_EQ("HGX_CPU_0", out[0].name);
    EXPECT_EQ("cpu", out[0].kind);
    EXPECT_EQ(std::optional<uint64_t>(14), out[0].eid);
    EXPECT_FALSE(out[0].instance);
    EXPECT_FALSE(out[0].cpuIndex);
    EXPECT_EQ("HGX_CPU_1", out[1].name);
    EXPECT_EQ(std::optional<uint64_t>(15), out[1].eid);
    EXPECT_EQ(std::optional<uint64_t>(1), out[1].instance);
    EXPECT_FALSE(out[1].cpuIndex);
    // a non-numeric string and one past uint64_t
    EXPECT_EQ("HGX_CPU_2", out[2].name);
    EXPECT_EQ(std::optional<uint64_t>(16), out[2].eid);
    EXPECT_FALSE(out[2].instance);
    EXPECT_FALSE(out[2].cpuIndex);
    // a value of a non-number type; a valid CpuIndex next to it is read
    EXPECT_EQ("HGX_CPU_3", out[3].name);
    EXPECT_EQ(std::optional<uint64_t>(17), out[3].eid);
    EXPECT_FALSE(out[3].instance);
    EXPECT_EQ(std::optional<uint64_t>(3), out[3].cpuIndex);
    EXPECT_EQ("HGX_CPU_4", out[4].name);
    EXPECT_EQ(std::optional<uint64_t>(0x100), out[4].instance);
    EXPECT_EQ(std::optional<uint64_t>(0x10000), out[4].cpuIndex);
}

TEST_F(EmDeviceDevices, takesAValidValueFromTheSecondRecordAfterAnInvalidOne)
{
    const std::string a = "/inventory/a/HGX_CPU_0";
    const std::string b = "/inventory/b/HGX_CPU_0";
    publish({a, b}, {{a + "|" + nvDeviceIntf,
                      {{"Name", std::string("HGX_CPU_0")},
                       {"Instance", std::string("x")},
                       {"CpuIndex", -1.0}}},
                     {b + "|" + nvDeviceIntf,
                      {{"Name", std::string("HGX_CPU_0")},
                       {"InstanceNumber", uint64_t{4}},
                       {"CpuIndex", uint64_t{1}}}}});

    auto out = devices(dbus);
    ASSERT_EQ(1U, out.size());
    // the invalid values of the first record count as absent, not as a
    // conflict that keeps the first
    EXPECT_EQ(std::optional<uint64_t>(4), out[0].instance);
    EXPECT_EQ(std::optional<uint64_t>(1), out[0].cpuIndex);
}

TEST_F(EmDeviceDevices, mergesTheTransportOfTheTwoRecords)
{
    // the first record of each device is the one under /inventory/a
    const std::string noTransport = "/inventory/a/HGX_GPU_0";
    const std::string gpuEntity = "/inventory/b/HGX_GPU_0";
    const std::string badPool = "/inventory/a/HGX_FPGA_0";
    const std::string fpgaEntity = "/inventory/b/HGX_FPGA_0";
    publish(
        {badPool, noTransport, fpgaEntity, gpuEntity},
        {{noTransport + "|" + nvDeviceIntf,
          {{"Name", std::string("HGX_GPU_0")}}},
         {gpuEntity + "|" + nvDeviceIntf, {{"Name", std::string("HGX_GPU_0")}}},
         {gpuEntity + "|" + transportIntf,
          {{"Medium", std::string("usb")},
           {"PoolStartEID", uint64_t{20}},
           {"PoolEndEID", uint64_t{21}}}},
         {badPool + "|" + nvDeviceIntf, {{"Name", std::string("HGX_FPGA_0")}}},
         {badPool + "|" + transportIntf,
          {{"Medium", std::string("spi")},
           {"PoolStartEID", uint64_t{0xFF}},
           {"PoolEndEID", uint64_t{50}}}},
         {fpgaEntity + "|" + nvDeviceIntf,
          {{"Name", std::string("HGX_FPGA_0")}}},
         {fpgaEntity + "|" + transportIntf,
          {{"Medium", std::string("i2c")},
           {"PoolStartEID", uint64_t{40}},
           {"PoolEndEID", uint64_t{60}}}}});

    auto out = devices(dbus);
    ASSERT_EQ(2U, out.size());
    // devices are listed in the path order of their first record
    // a pool EID the first record has out of range counts as none
    EXPECT_EQ("HGX_FPGA_0", out[0].name);
    EXPECT_EQ("spi", out[0].medium);
    EXPECT_EQ(std::optional<uint64_t>(40), out[0].poolStartEid);
    EXPECT_EQ(std::optional<uint64_t>(50), out[0].poolEndEid);
    // a first record without a .Transport interface takes all of it from
    // the second
    EXPECT_EQ("HGX_GPU_0", out[1].name);
    EXPECT_EQ("usb", out[1].medium);
    EXPECT_EQ(std::optional<uint64_t>(20), out[1].poolStartEid);
    EXPECT_EQ(std::optional<uint64_t>(21), out[1].poolEndEid);
}

TEST_F(EmDeviceDevices, readsTheTransportAndBoundsThePool)
{
    const std::string bridge = "/inventory/platform/HGX_FPGA_0";
    const std::string wide = "/inventory/platform/HGX_FPGA_1";
    const std::string unparsable = "/inventory/platform/HGX_FPGA_2";
    const std::string bare = "/inventory/platform/HGX_GPU_0";
    publish(
        {bridge, wide, unparsable, bare},
        {{bridge + "|" + nvDeviceIntf, {{"Name", std::string("HGX_FPGA_0")}}},
         {bridge + "|" + transportIntf,
          {{"Medium", std::string("spi")},
           {"PoolStartEID", std::string("0x00")},
           {"PoolEndEID", uint64_t{0xFE}}}},
         {wide + "|" + nvDeviceIntf, {{"Name", std::string("HGX_FPGA_1")}}},
         {wide + "|" + transportIntf,
          {{"Medium", std::string("usb")},
           {"PoolStartEID", uint64_t{0xFF}},
           {"PoolEndEID", std::numeric_limits<uint64_t>::max()}}},
         {unparsable + "|" + nvDeviceIntf,
          {{"Name", std::string("HGX_FPGA_2")}}},
         {unparsable + "|" + transportIntf,
          {{"Medium", std::string("i2c")},
           {"PoolStartEID", true},
           {"PoolEndEID", std::string("x")}}},
         // no .Transport child interface
         {bare + "|" + nvDeviceIntf, {{"Name", std::string("HGX_GPU_0")}}}});

    auto out = devices(dbus);
    ASSERT_EQ(4U, out.size());
    EXPECT_EQ("HGX_FPGA_0", out[0].name);
    EXPECT_EQ("spi", out[0].medium);
    // both ends of the accepted range [0, maxDeviceEid]
    EXPECT_EQ(std::optional<uint64_t>(0), out[0].poolStartEid);
    EXPECT_EQ(std::optional<uint64_t>(0xFE), out[0].poolEndEid);
    // 0xFF is the broadcast EID; a value past it is dropped too
    EXPECT_EQ("HGX_FPGA_1", out[1].name);
    EXPECT_EQ("usb", out[1].medium);
    EXPECT_FALSE(out[1].poolStartEid);
    EXPECT_FALSE(out[1].poolEndEid);
    // a value that is not a number is dropped, the rest is still read
    EXPECT_EQ("HGX_FPGA_2", out[2].name);
    EXPECT_EQ("i2c", out[2].medium);
    EXPECT_FALSE(out[2].poolStartEid);
    EXPECT_FALSE(out[2].poolEndEid);
    EXPECT_EQ("HGX_GPU_0", out[3].name);
    EXPECT_EQ("", out[3].medium);
    EXPECT_FALSE(out[3].poolStartEid);
    EXPECT_FALSE(out[3].poolEndEid);
}

using EmDevicePlatformPrefix = EmDeviceDevices;

TEST_F(EmDevicePlatformPrefix, isTakenFromTheFirstReadableRecord)
{
    const std::string unreadable = "/inventory/platform/A/DevicePrefix";
    const std::string record = "/inventory/platform/B/DevicePrefix";
    const std::string later = "/inventory/platform/C/DevicePrefix";
    publish({},
            {{record + "|" + devicePrefixIntf,
              {{"Name", std::string("DevicePrefix")},
               {"Prefix", std::string("HGX_")}}},
             {later + "|" + devicePrefixIntf,
              {{"Name", std::string("DevicePrefix")},
               {"Prefix", std::string("XYZ_")}}}},
            {{devicePrefixIntf, {unreadable, record, later}}});
    EXPECT_EQ("HGX_", platformPrefix(dbus));
}

TEST_F(EmDevicePlatformPrefix, isEmptyWhenTheFirstReadableRecordHasNone)
{
    // a readable record without a Prefix is the platform's record all the
    // same: the platform has no prefix
    const std::string record = "/inventory/platform/A/DevicePrefix";
    const std::string later = "/inventory/platform/B/DevicePrefix";
    publish({},
            {{record + "|" + devicePrefixIntf,
              {{"Name", std::string("DevicePrefix")}}},
             {later + "|" + devicePrefixIntf,
              {{"Name", std::string("DevicePrefix")},
               {"Prefix", std::string("HGX_")}}}},
            {{devicePrefixIntf, {record, later}}});
    EXPECT_EQ("", platformPrefix(dbus));
}

TEST_F(EmDevicePlatformPrefix, isEmptyWithoutARecord)
{
    publish({}, {});
    EXPECT_EQ("", platformPrefix(dbus));
}

TEST(EmDeviceTerminusConfig, boundsInstanceAndCpuIndex)
{
    Device dev;
    dev.name = "HGX_CPU_0";
    dev.kind = "cpu";
    dev.parentChassis = "HGX_ProcessorModule_0";
    const std::string prefix = "HGX_";

    dev.instance = 65535;
    dev.cpuIndex = 65535;
    auto config = terminusConfig(dev, prefix);
    ASSERT_TRUE(config);
    EXPECT_EQ("ProcessorModule_0", config->name);
    EXPECT_EQ(65535, config->instance);
    EXPECT_EQ(65535, config->cpuIndex);

    dev.instance = 65536;
    EXPECT_FALSE(terminusConfig(dev, prefix));

    dev.instance = 0;
    dev.cpuIndex = 65536;
    EXPECT_FALSE(terminusConfig(dev, prefix));

    dev.cpuIndex = 0;
    dev.kind = "gpu";
    EXPECT_FALSE(terminusConfig(dev, prefix));
}

TEST(EmDevicePldmDevice, readsTheRecordThroughTheGivenBus)
{
    const std::string path = "/inventory/platform/HGX_CPU_0_PLDM";
    const std::string base(pldmDeviceIntf);
    const std::map<std::string, PropertyMap> props{
        {path + "|" + base,
         {{"Name", std::string("HGX_CPU_0_PLDM")},
          {"FirmwareName", std::string("HGX_FW_CPU_0")},
          {"PortMaxSpeedMBps", uint64_t{50000}}}},
        {path + "|" + base + ".UpdateableComponents",
         {{"HGX_FW_CPU_0", uint64_t{1}}}},
        {path + "|" + base + ".FWInventoryOnlyComponent",
         {{"HGX_FW_CPU_0_ROM", uint64_t{2}}, {"TooBig", uint64_t{65536}}}},
        {path + "|" + base + ".SensorAuxNames",
         {{"HGX_CPU_0_TEMP_0", uint64_t{7}}, {"Bad", std::string("x")}}},
        {path + "|" + base + ".Ports",
         {{"3", std::string("HGX_CPU_0_Port_0")},
          {"x", std::string("skipped")},
          {"4", uint64_t{1}}}}};

    MockdBusHandler dbus;
    EXPECT_CALL(dbus, getSubtree(_, _, _))
        .WillRepeatedly(Return(subtreeOf({path}, pldmDeviceIntf)));
    EXPECT_CALL(dbus, getDbusPropertiesVariant(_, _, _))
        .WillRepeatedly(
            Invoke([&props](const char*, const char* p, const char* intf) {
                auto it = props.find(std::string(p) + "|" + intf);
                if (it == props.end())
                {
                    throw std::runtime_error("no such interface");
                }
                return it->second;
            }));

    auto record = pldmDevice("HGX_CPU_0", dbus);
    ASSERT_TRUE(record);
    EXPECT_EQ("HGX_FW_CPU_0", record->firmwareName);
    EXPECT_EQ(std::optional<uint64_t>(50000), record->portMaxSpeed);
    EXPECT_EQ((std::map<uint16_t, std::string>{{1, "HGX_FW_CPU_0"},
                                               {2, "HGX_FW_CPU_0_ROM"}}),
              record->components);
    EXPECT_EQ(std::set<uint16_t>{2}, record->inventoryOnly);
    EXPECT_EQ((std::map<uint16_t, std::string>{{7, "HGX_CPU_0_TEMP_0"}}),
              record->auxNames);
    EXPECT_EQ((std::map<uint16_t, std::string>{{3, "HGX_CPU_0_Port_0"}}),
              record->ports);

    EXPECT_FALSE(pldmDevice("HGX_CPU_1", dbus));
}
