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

#include "common/types.hpp"

#include <fstream>
#include <sstream>

#include <gtest/gtest.h>

#undef FW_UPDATE_CONFIG_JSON
#define FW_UPDATE_CONFIG_JSON "/tmp/fw_update_config_internal_test.json"
#include "fw-update/config.cpp" // NOLINT(bugprone-suspicious-include)

namespace pldm::fw_update
{

namespace
{

std::string readTextFile(const std::filesystem::path& path)
{
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void writeRuntimeConfig(const std::string& content)
{
    std::ofstream out(FW_UPDATE_CONFIG_JSON);
    out << content;
}

} // namespace

TEST(ConfigInternal, extractEidFromUint8)
{
    pldm::dbus::Value value = static_cast<uint8_t>(9);
    auto eid = extractEid(value);

    ASSERT_TRUE(eid.has_value());
    EXPECT_EQ(*eid, 9);
}

TEST(ConfigInternal, extractEidFromUnsignedIntInRange)
{
    pldm::dbus::Value value = static_cast<unsigned int>(14);
    auto eid = extractEid(value);

    ASSERT_TRUE(eid.has_value());
    EXPECT_EQ(*eid, 14);
}

TEST(ConfigInternal, extractEidFromUnsignedIntOutOfRange)
{
    pldm::dbus::Value value = static_cast<unsigned int>(300);
    auto eid = extractEid(value);

    EXPECT_FALSE(eid.has_value());
}

TEST(ConfigInternal, extractEidFromInvalidType)
{
    pldm::dbus::Value value = std::string("14");
    auto eid = extractEid(value);

    EXPECT_FALSE(eid.has_value());
}

TEST(ConfigInternal, parseConfigInvalidJsonReturnsEmptyOutputs)
{
    writeRuntimeConfig("{ invalid json");
    DeviceInventoryInfo deviceInventoryInfo;
    FirmwareInventoryInfo fwInventoryInfo;
    ComponentNameMapInfo componentNameMapInfo;
    ExcludedFwUpdateEids excludedFwUpdateEids;

    parseConfig(FW_UPDATE_CONFIG_JSON, deviceInventoryInfo, fwInventoryInfo,
                componentNameMapInfo, excludedFwUpdateEids);

    EXPECT_TRUE(deviceInventoryInfo.infos.empty());
    EXPECT_TRUE(fwInventoryInfo.infos.empty());
    EXPECT_TRUE(componentNameMapInfo.infos.empty());
    EXPECT_TRUE(excludedFwUpdateEids.empty());
}

TEST(ConfigInternal, getDeviceNameFromEidLoadsFromRuntimeConfig)
{
    auto defaultConfig =
        readTextFile("./fw_update_jsons/fw_update_config_eid_map.json");
    writeRuntimeConfig(defaultConfig);

    auto fromDeviceInventory = getDeviceNameFromEid(9);
    auto fromFirmwareFallback = getDeviceNameFromEid(14);
    auto outOfRange = getDeviceNameFromEid(44);

    ASSERT_TRUE(fromDeviceInventory.has_value());
    EXPECT_EQ(*fromDeviceInventory, "HGX_ERoT_BMC_0");
    ASSERT_TRUE(fromFirmwareFallback.has_value());
    EXPECT_EQ(*fromFirmwareFallback, "HGX_SBIOS_FMC_0");
    EXPECT_FALSE(outOfRange.has_value());
}

TEST(ConfigInternal, buildEidToNameMapExercisesFallbackAndSkipBranches)
{
    const std::string config = R"({
        "entries": [
            {
                "match": {
                    "Interface": "xyz.openbmc_project.MCTP.Endpoint",
                    "Properties": [{"Name": "EID", "Type": "u", "Value": 0}]
                },
                "component_info": {"COMP_ZERO": 1}
            },
            {
                "match": {
                    "Interface": "xyz.openbmc_project.MCTP.Endpoint",
                    "Properties": [{"Name": "UUID", "Type": "s", "Value": "uuid-no-eid"}]
                },
                "device_inventory": {
                    "create": {"object_path": "/xyz/openbmc_project/inventory/system/chassis/NoEid"}
                }
            },
            {
                "match": {
                    "Interface": "xyz.openbmc_project.MCTP.Endpoint",
                    "Properties": [{"Name": "EID", "Type": "s", "Value": "bad"}]
                },
                "device_inventory": {
                    "create": {"object_path": "/xyz/openbmc_project/inventory/system/chassis/BadType"}
                }
            },
            {
                "match": {
                    "Interface": "xyz.openbmc_project.MCTP.Endpoint",
                    "Properties": [{"Name": "EID", "Type": "b", "Value": true}]
                },
                "device_inventory": {
                    "create": {"object_path": "/xyz/openbmc_project/inventory/system/chassis/UnknownType"}
                }
            },
            {
                "match": {
                    "Interface": "xyz.openbmc_project.MCTP.Endpoint",
                    "Properties": [{"Name": "EID", "Type": "y", "Value": 21}]
                },
                "device_inventory": {
                    "create": {"object_path": ""},
                    "update": {"object_path": "/xyz/openbmc_project/inventory/system/chassis/DeviceFromUpdate"}
                }
            },
            {
                "match": {
                    "Interface": "xyz.openbmc_project.MCTP.Endpoint",
                    "Properties": [{"Name": "EID", "Type": "y", "Value": 22}]
                },
                "device_inventory": {
                    "create": {"object_path": "/xyz/openbmc_project/inventory/system/chassis/DeviceFromCreate"}
                }
            },
            {
                "match": {
                    "Interface": "xyz.openbmc_project.MCTP.Endpoint",
                    "Properties": [{"Name": "EID", "Type": "u", "Value": 300}]
                },
                "device_inventory": {
                    "create": {"object_path": "/xyz/openbmc_project/inventory/system/chassis/OutOfRangeShouldSkip"}
                }
            },
            {
                "match": {
                    "Interface": "xyz.openbmc_project.MCTP.Endpoint",
                    "Properties": [{"Name": "EID", "Type": "y", "Value": 22}]
                },
                "firmware_inventory": {
                    "create": {"DuplicateShouldSkip": {"component_id": 50}}
                },
                "component_info": {"DuplicateShouldSkip": 50}
            },
            {
                "match": {
                    "Interface": "xyz.openbmc_project.MCTP.Endpoint",
                    "Properties": [{"Name": "EID", "Type": "y", "Value": 23}]
                },
                "firmware_inventory": {
                    "create": {"FallbackComp": {"component_id": 32}}
                },
                "component_info": {"FallbackComp": 32}
            },
            {
                "match": {
                    "Interface": "xyz.openbmc_project.MCTP.Endpoint",
                    "Properties": [{"Name": "EID", "Type": "y", "Value": 24}]
                },
                "firmware_inventory": {
                    "create": {"NoComponentInfo": {"component_id": 40}}
                }
            },
            {
                "match": {
                    "Interface": "xyz.openbmc_project.MCTP.Endpoint",
                    "Properties": [{"Name": "EID", "Type": "y", "Value": 25}]
                },
                "firmware_inventory": {
                    "create": {"": {"component_id": 41}}
                },
                "component_info": {"": 41}
            },
            {
                "match": {
                    "Interface": "xyz.openbmc_project.MCTP.Endpoint",
                    "Properties": [{"Name": "EID", "Type": "y", "Value": 26}]
                },
                "firmware_inventory": {
                    "create": {"NoCreateMapEntry": {"associations": []}}
                },
                "component_info": {"NoCreateMapEntry": 42}
            }
        ]
    })";
    writeRuntimeConfig(config);

    auto eidToNameMap = buildEidToNameMap();

    ASSERT_EQ(eidToNameMap.at(21), "DeviceFromUpdate");
    ASSERT_EQ(eidToNameMap.at(22), "DeviceFromCreate");
    ASSERT_EQ(eidToNameMap.at(23), "FallbackComp");
    EXPECT_FALSE(eidToNameMap.contains(24));
    EXPECT_FALSE(eidToNameMap.contains(25));
    EXPECT_FALSE(eidToNameMap.contains(26));
}

TEST(ConfigInternal, targetNameLookupSkipsExcludedFwUpdateEids)
{
    const std::string config = R"({
        "excluded_fw_update_eids": [31],
        "entries": [
            {
                "match": {
                    "Interface": "xyz.openbmc_project.MCTP.Endpoint",
                    "Properties": [{"Name": "EID", "Type": "y", "Value": 31}]
                },
                "device_inventory": {
                    "create": {"object_path": "/xyz/openbmc_project/inventory/system/chassis/ExcludedDevice"}
                },
                "firmware_inventory": {
                    "create": {"ExcludedComponent": {"component_id": 1}},
                    "update": {"ExcludedUpdateComponent": 2}
                },
                "component_info": {"ExcludedComponentInfo": 3}
            },
            {
                "match": {
                    "Interface": "xyz.openbmc_project.MCTP.Endpoint",
                    "Properties": [{"Name": "EID", "Type": "y", "Value": 32}]
                },
                "device_inventory": {
                    "create": {"object_path": "/xyz/openbmc_project/inventory/system/chassis/IncludedDevice"}
                },
                "firmware_inventory": {
                    "create": {"IncludedComponent": {"component_id": 4}}
                },
                "component_info": {"IncludedComponentInfo": 5}
            }
        ]
    })";
    writeRuntimeConfig(config);

    auto eidToNameMap = buildEidToNameMap();
    auto nameToEidMap = buildNameToEidMap();

    ASSERT_TRUE(eidToNameMap.contains(31));
    EXPECT_EQ(eidToNameMap.at(31), "ExcludedDevice");
    EXPECT_TRUE(eidToNameMap.contains(32));
    EXPECT_FALSE(nameToEidMap.contains("ExcludedDevice"));
    EXPECT_FALSE(nameToEidMap.contains("ExcludedComponent"));
    EXPECT_FALSE(nameToEidMap.contains("ExcludedUpdateComponent"));
    EXPECT_FALSE(nameToEidMap.contains("ExcludedComponentInfo"));
    ASSERT_TRUE(nameToEidMap.contains("IncludedDevice"));
    EXPECT_EQ(nameToEidMap.at("IncludedDevice"), 32);
}

} // namespace pldm::fw_update
