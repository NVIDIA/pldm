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

#include "fw-update/em_device_policy.hpp"

#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

using namespace pldm::fw_update::em_device_policy;

namespace
{

const std::string chassis(pldm::em_device::chassisRoot);
const std::string component(pldm::em_device::componentRoot);
const std::string bmc(pldm::em_device::bmcRoot);

Device device(const std::string& name, const std::string& kind)
{
    Device dev;
    dev.name = name;
    dev.kind = kind;
    dev.parentChassis = "HGX_BMC_0";
    dev.instance = 3;
    return dev;
}

} // namespace

TEST(EmDevicePolicyComponentRule, bmcFirmwareIsUpdateOnly)
{
    // the BMC firmware behind an ERoT (16) and behind an IRoT (10), on any
    // kind of device
    PldmDevice record;
    record.inventoryOnly = {10, 16};
    for (const auto* kind : {"erot", "irot", "cpu"})
    {
        for (uint16_t id : {10, 16})
        {
            auto rule = componentRule(device("HGX_ERoT_BMC_0", kind), "HGX_",
                                      record, id);
            EXPECT_TRUE(rule.updateOnly) << kind << " " << id;
            EXPECT_TRUE(rule.forward.empty()) << kind << " " << id;
            // not "active", although the record lists it inventory-only
            EXPECT_TRUE(rule.backward.empty()) << kind << " " << id;
            EXPECT_TRUE(rule.endpoints.empty()) << kind << " " << id;
            EXPECT_TRUE(rule.chassisEndpoint.empty()) << kind << " " << id;
        }
    }
    // neighbouring ids are not
    EXPECT_FALSE(componentRule(device("HGX_ERoT_BMC_0", "erot"), "", record, 15)
                     .updateOnly);
    EXPECT_FALSE(componentRule(device("HGX_ERoT_BMC_0", "erot"), "", record, 17)
                     .updateOnly);
}

TEST(EmDevicePolicyComponentRule, inventoryOnlyComponentsAreActive)
{
    PldmDevice record;
    record.inventoryOnly = {2};
    const auto dev = device("HGX_GPU_0", "gpu");

    auto updateable = componentRule(dev, "HGX_", record, 1);
    EXPECT_EQ("inventory", updateable.forward);
    EXPECT_EQ("activation", updateable.backward);
    EXPECT_FALSE(updateable.updateOnly);

    auto inventoryOnly = componentRule(dev, "HGX_", record, 2);
    EXPECT_EQ("inventory", inventoryOnly.forward);
    EXPECT_EQ("active", inventoryOnly.backward);
    EXPECT_FALSE(inventoryOnly.updateOnly);
}

TEST(EmDevicePolicyComponentRule, endpointsFollowTheKind)
{
    const PldmDevice record;

    auto cpu = componentRule(device("HGX_CPU_0", "cpu"), "HGX_", record, 1);
    EXPECT_EQ(std::vector<std::string>{component + "HGX_CPU_0"}, cpu.endpoints);
    EXPECT_TRUE(cpu.chassisEndpoint.empty());
    EXPECT_EQ("NVIDIA", cpu.manufacturer);

    // an IRoT's firmware belongs to the BMC it is the RoT of
    auto irot =
        componentRule(device("HGX_IRoT_BMC_0", "irot"), "HGX_", record, 1);
    EXPECT_EQ(std::vector<std::string>{bmc + "HGX_BMC_0"}, irot.endpoints);
    EXPECT_TRUE(irot.chassisEndpoint.empty());

    // the CPU firmware behind the ERoT of a CPU (218) belongs to the CPU
    // chassis; the ERoT's own firmware (65280) to the ERoT chassis
    for (const auto* kind : {"cpu_erot", "cpu_erot_i2c"})
    {
        auto erot = device("MGX_ERoT_CPU_0", kind);
        erot.parentChassis = "MGX_CPU_0";
        EXPECT_EQ(std::vector<std::string>{chassis + "MGX_CPU_0"},
                  componentRule(erot, "", record, 218).endpoints)
            << kind;
        erot.parentChassis = "board/MGX_CPU_0";
        EXPECT_EQ(std::vector<std::string>{chassis + "MGX_CPU_0"},
                  componentRule(erot, "", record, 218).endpoints)
            << kind;
        EXPECT_EQ(std::vector<std::string>{chassis + "MGX_ERoT_CPU_0"},
                  componentRule(erot, "", record, 65280).endpoints)
            << kind;
    }

    // any other kind: the device chassis
    auto gpu = componentRule(device("HGX_GPU_0", "gpu"), "HGX_", record, 1);
    EXPECT_EQ(std::vector<std::string>{chassis + "HGX_GPU_0"}, gpu.endpoints);
    EXPECT_TRUE(gpu.chassisEndpoint.empty());
    EXPECT_EQ("NVIDIA", gpu.manufacturer);
}

TEST(EmDevicePolicyComponentRule, cxFirmwareBelongsToTheNicAndTheChassis)
{
    PldmDevice record;
    record.networkAdapter = "HGX_NIC_0";
    const auto dev = device("HGX_CX9_0", "cx9_io");

    auto rule = componentRule(dev, "HGX_", record, 1);
    EXPECT_EQ(std::vector<std::string>{chassis +
                                       "HGX_CX9_0/NetworkAdapters/HGX_NIC_0"},
              rule.endpoints);
    EXPECT_EQ(chassis + "HGX_CX9_0", rule.chassisEndpoint);
    EXPECT_EQ("inventory", rule.forward);
    EXPECT_EQ("activation", rule.backward);

    // without a NIC the firmware is associated with the chassis only
    record.networkAdapter.clear();
    rule = componentRule(dev, "HGX_", record, 1);
    EXPECT_TRUE(rule.endpoints.empty());
    EXPECT_EQ(chassis + "HGX_CX9_0", rule.chassisEndpoint);

    // another component of the device belongs to its chassis
    record.networkAdapter = "HGX_NIC_0";
    rule = componentRule(dev, "HGX_", record, 2);
    EXPECT_EQ(std::vector<std::string>{chassis + "HGX_CX9_0"}, rule.endpoints);
    EXPECT_TRUE(rule.chassisEndpoint.empty());
}

TEST(EmDevicePolicyDownstreamRule, devicesSitOnThePortsOfTheDevice)
{
    PldmDevice record;
    record.networkAdapter = "NIC_0";
    record.ports = {{201, "C3_2"}, {200, "C4_1"}};
    const auto dev = device("CX_1", "cx9_io");

    auto rule = downstreamRule(dev, record);
    EXPECT_EQ((std::vector<std::string>{
                  chassis + "CX_1/NetworkAdapters/NIC_0/Ports/C4_1",
                  chassis + "CX_1/NetworkAdapters/NIC_0/Ports/C3_2"}),
              rule.endpoints);
    EXPECT_EQ("inventory", rule.forward);
    EXPECT_EQ("activation", rule.backward);
    EXPECT_EQ("IEEE_OUI", rule.manufacturerNameSource);

    // without a network adapter the ports belong to the chassis
    record.networkAdapter.clear();
    rule = downstreamRule(dev, record);
    EXPECT_EQ(chassis + "CX_1/Ports/C4_1", rule.endpoints.front());
}

TEST(EmDevicePolicyComponentRule, hpmCpldIsALatticeComponent)
{
    const PldmDevice record;
    const auto dev = device("HGX_HPM_SMA_0", "hmc_hpm_sma");

    auto cpld = componentRule(dev, "HGX_", record, 65283);
    EXPECT_EQ(std::vector<std::string>{component + "HGX_CPLD_3"},
              cpld.endpoints);
    EXPECT_EQ("Lattice", cpld.manufacturer);
    EXPECT_FALSE(cpld.updateOnly);

    // without a prefix or an instance
    auto bare = dev;
    bare.instance.reset();
    EXPECT_EQ(std::vector<std::string>{component + "CPLD_0"},
              componentRule(bare, "", record, 65283).endpoints);

    // another component of the device belongs to its chassis
    auto other = componentRule(dev, "HGX_", record, 1);
    EXPECT_EQ(std::vector<std::string>{chassis + "HGX_HPM_SMA_0"},
              other.endpoints);
    EXPECT_EQ("NVIDIA", other.manufacturer);
}

TEST(EmDevicePolicyInventoryPaths, followTheKind)
{
    auto gpu = inventoryPaths(device("HGX_GPU_0", "gpu"), "HGX_");
    ASSERT_TRUE(gpu);
    // the IRoT chassis name keeps the prefix in front
    EXPECT_EQ(chassis + "HGX_IRoT_GPU_0", gpu->create);
    EXPECT_EQ(chassis + "HGX_GPU_0", gpu->update);

    auto unprefixed = inventoryPaths(device("GPU_0", "gpu"), "");
    ASSERT_TRUE(unprefixed);
    EXPECT_EQ(chassis + "IRoT_GPU_0", unprefixed->create);
    EXPECT_EQ(chassis + "GPU_0", unprefixed->update);

    for (const auto* kind : {"bluefield_sma", "cx_sma"})
    {
        auto sma = inventoryPaths(device("HGX_SMA_0", kind), "HGX_");
        ASSERT_TRUE(sma) << kind;
        // the IRoT_ goes in front of the full name, prefix included
        EXPECT_EQ(chassis + "IRoT_HGX_SMA_0", sma->create) << kind;
        EXPECT_EQ(chassis + "HGX_SMA_0", sma->update) << kind;
    }

    auto erot = inventoryPaths(device("HGX_ERoT_BMC_0", "erot"), "HGX_");
    ASSERT_TRUE(erot);
    EXPECT_EQ(chassis + "HGX_ERoT_BMC_0", erot->create);
    EXPECT_EQ(bmc + "HGX_BMC_0", erot->update);

    // nsmd owns the SKU of an IRoT: pldmd writes nothing for it
    EXPECT_FALSE(inventoryPaths(device("HGX_IRoT_BMC_0", "irot"), "HGX_"));
    EXPECT_FALSE(inventoryPaths(device("IRoT_BMC_0", "irot"), ""));

    auto cx = inventoryPaths(device("HGX_CX9_0", "cx9_io"), "HGX_");
    ASSERT_TRUE(cx);
    EXPECT_EQ("", cx->create);
    EXPECT_EQ(chassis + "HGX_CX9_0", cx->update);

    EXPECT_FALSE(inventoryPaths(device("HGX_CPU_0", "cpu"), "HGX_"));
    EXPECT_FALSE(inventoryPaths(device("HGX_X_0", ""), "HGX_"));
}

// The GPU and HPM SMAs (kinds gpu_sma and hmc_hpm_sma) have no RoT chassis:
// they get the AP-SKU on their own chassis and nothing else, as develop's
// HGX_*_SMA_n_DevInv records (UpdateInventoryPath only) gave them.
TEST(EmDevicePolicyInventoryPaths, gpuAndHpmSmasGetTheirOwnChassisOnly)
{
    for (const auto& [name, kind] :
         {std::pair{"HGX_GPU_SMA_1", "gpu_sma"},
          std::pair{"HGX_ProcessorModule_SMA_1", "hmc_hpm_sma"}})
    {
        auto sma = inventoryPaths(device(name, kind), "HGX_");
        ASSERT_TRUE(sma) << kind;
        EXPECT_EQ("", sma->create) << kind;
        // the device's own chassis, not its parent's
        EXPECT_EQ(chassis + name, sma->update) << kind;
    }

    // the HPM SMA of a BMC is another kind
    EXPECT_FALSE(
        inventoryPaths(device("ProcessorModule_SMA_0", "bmc_hpm_sma"), ""));
}
