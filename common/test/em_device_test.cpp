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

#include "common/em_device.hpp"
#include "common/test/mocked_utils.hpp"

#include <cmath>
#include <limits>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldm::em_device;
using pldm::utils::PropertyMap;
using pldm::utils::PropertyValue;
using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;

TEST(EmDeviceNumberValue, acceptsIntegersAndIntegralDoubles)
{
    EXPECT_EQ(numberValue(uint8_t{14}), 14U);
    EXPECT_EQ(numberValue(uint64_t{0}), 0U);
    EXPECT_EQ(numberValue(int64_t{7}), 7U);
    EXPECT_EQ(numberValue(std::numeric_limits<uint64_t>::max()),
              std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(numberValue(double{14.0}), 14U);
    EXPECT_EQ(numberValue(double{0.0}), 0U);
    // the largest double below 2^64
    EXPECT_EQ(numberValue(double{18446744073709549568.0}),
              18446744073709549568U);
    EXPECT_EQ(numberValue(std::string("14")), 14U);
    EXPECT_EQ(numberValue(std::string("0x0E")), 14U);
    EXPECT_EQ(numberValue(std::string("0X0e")), 14U);
}

TEST(EmDeviceNumberValue, rejectsUnusableValues)
{
    EXPECT_FALSE(numberValue(true));
    EXPECT_FALSE(numberValue(int16_t{-1}));
    EXPECT_FALSE(numberValue(int64_t{-1}));
    EXPECT_FALSE(numberValue(double{-1.0}));
    EXPECT_FALSE(numberValue(double{1.5}));
    EXPECT_FALSE(numberValue(std::numeric_limits<double>::infinity()));
    EXPECT_FALSE(numberValue(std::numeric_limits<double>::quiet_NaN()));
    EXPECT_FALSE(numberValue(double{1e20}));
    // 2^64 is one past the largest uint64_t
    EXPECT_FALSE(numberValue(double{18446744073709551616.0}));
    EXPECT_FALSE(numberValue(std::string("")));
    EXPECT_FALSE(numberValue(std::string("14x")));
    EXPECT_FALSE(numberValue(std::string("-1")));
    EXPECT_FALSE(numberValue(std::string("0x")));
    EXPECT_FALSE(numberValue(std::vector<uint8_t>{14}));
}

TEST(EmDeviceNumberProp, appliesTheBound)
{
    const PropertyMap props{
        {"A", uint64_t{14}}, {"B", uint64_t{255}}, {"C", uint64_t{254}}};
    EXPECT_EQ(numberProp(props, "A"), 14U);
    EXPECT_EQ(numberProp(props, "A", 14), 14U);
    EXPECT_FALSE(numberProp(props, "A", 13));
    EXPECT_FALSE(numberProp(props, "B", maxDeviceEid));
    EXPECT_EQ(numberProp(props, "C", maxDeviceEid), 0xFEU);
    EXPECT_FALSE(numberProp(props, "missing"));
}

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

/** @brief A mock D-Bus with NvDevice records at @p paths whose properties
 *         are looked up as "<path>|<interface>". */
class EmDeviceDevices : public ::testing::Test
{
  protected:
    void publish(const std::vector<std::string>& paths,
                 std::map<std::string, PropertyMap> properties)
    {
        props = std::move(properties);
        EXPECT_CALL(dbus, getSubtree(_, _, _))
            .WillRepeatedly(Invoke([paths](const std::string&, int,
                                           const std::vector<std::string>& i) {
                if (!i.empty() && i.front() == nvDeviceIntf)
                {
                    return subtreeOf(paths, nvDeviceIntf);
                }
                return pldm::utils::GetSubTreeResponse{};
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
             {topo + "|" + transportIntf, {{"Medium", std::string("USB")}}},
             {entity + "|" + nvDeviceIntf,
              {{"Name", std::string("HGX_CPU_0")},
               {"Kind", std::string("cpu")},
               {"InstanceNumber", uint64_t{0}},
               {"CpuIndex", uint64_t{2}},
               {"EID", uint64_t{14}}}}});

    auto out = devices(dbus);
    ASSERT_EQ(1U, out.size());
    EXPECT_EQ("HGX_CPU_0", out[0].name);
    EXPECT_EQ("cpu", out[0].kind);
    EXPECT_EQ("HGX_ProcessorModule_0", out[0].parentChassis);
    EXPECT_EQ(std::optional<uint64_t>(14), out[0].eid);
    EXPECT_EQ(std::optional<uint64_t>(0), out[0].instance);
    // taken from the second record, the first lacks it
    EXPECT_EQ(std::optional<uint64_t>(2), out[0].cpuIndex);
    EXPECT_EQ("USB", stringProp(out[0].transport, "Medium"));

    auto byName = deviceByName("HGX_CPU_0", dbus);
    ASSERT_TRUE(byName);
    EXPECT_EQ(std::optional<uint64_t>(14), byName->eid);
    EXPECT_FALSE(deviceByName("HGX_CPU_1", dbus));
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

TEST(EmDeviceTerminusConfig, boundsInstanceAndCpuIndex)
{
    Device dev;
    dev.name = "HGX_CPU_0";
    dev.kind = "cpu";
    dev.parentChassis = "HGX_ProcessorModule_0";
    const PlatformNames platform{"HGX_"};

    dev.instance = 65535;
    dev.cpuIndex = 65535;
    auto config = terminusConfig(dev, platform);
    ASSERT_TRUE(config);
    EXPECT_EQ("ProcessorModule_0", config->name);
    EXPECT_EQ(65535, config->instance);
    EXPECT_EQ(65535, config->cpuIndex);

    dev.instance = 65536;
    EXPECT_FALSE(terminusConfig(dev, platform));

    dev.instance = 0;
    dev.cpuIndex = 65536;
    EXPECT_FALSE(terminusConfig(dev, platform));

    dev.cpuIndex = 0;
    dev.kind = "gpu";
    EXPECT_FALSE(terminusConfig(dev, platform));
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
