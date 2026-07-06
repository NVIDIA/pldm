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
#include "fw-update/config.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldm::fw_update;

TEST(ParseConfig, SingleEntry)
{
    FirmwareInventoryInfo fwInventoryInfo(
        {{{"xyz.openbmc_project.Common.UUID",
           {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130003"}}},
          {{{1,
             {"ComponentName1",
              {{"inventory", "activation",
                "/xyz/openbmc_project/software/ComponentName1"}},
              "NVIDIA",
              false}}},
           {{2, "ComponentName2"}}}}});

    ComponentNameMapInfo componentNameMapInfo(
        {{{"xyz.openbmc_project.Common.UUID",
           {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130003"}}},
          {{1, "ComponentName1"}, {2, "ComponentName2"}}}});

    FirmwareInventoryInfo outFwInventoryInfo;
    ComponentNameMapInfo outComponentNameMapConfig;

    parseConfig("./fw_update_jsons/fw_update_config_single_entry.json",
                outFwInventoryInfo, outComponentNameMapConfig);

    EXPECT_EQ(outFwInventoryInfo.infos, fwInventoryInfo.infos);
    EXPECT_EQ(outComponentNameMapConfig.infos, componentNameMapInfo.infos);
}

TEST(ParseConfig, CombinedPropertyMatch)
{
    FirmwareInventoryInfo fwInventoryInfo(
        {{{"xyz.openbmc_project.Inventory.Decorator.I2CDevice",
           {{"Address", uint32_t(0)}, {"Bus", uint32_t(16)}}},
          {{{1,
             {"ComponentName1",
              {{"inventory", "activation",
                "/xyz/openbmc_project/software/ComponentName1"}},
              "NVIDIA",
              false}}},
           {{2, "ComponentName2"}}}}});

    ComponentNameMapInfo componentNameMapInfo(
        {{{"xyz.openbmc_project.Inventory.Decorator.I2CDevice",
           {{"Address", uint32_t(0)}, {"Bus", uint32_t(16)}}},
          {{1, "ComponentName1"}, {2, "ComponentName2"}}}});

    FirmwareInventoryInfo outFwInventoryInfo;
    ComponentNameMapInfo outComponentNameMapConfig;

    parseConfig(
        "./fw_update_jsons/fw_update_config_combined_properties_match.json",
        outFwInventoryInfo, outComponentNameMapConfig);

    EXPECT_EQ(outFwInventoryInfo.infos, fwInventoryInfo.infos);
    EXPECT_EQ(outComponentNameMapConfig.infos, componentNameMapInfo.infos);
}

TEST(ParseConfig, MultipleEntry)
{
    FirmwareInventoryInfo fwInventoryInfo(
        {{{"xyz.openbmc_project.Common.UUID",
           {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130003"}}},
          {{}, {{1, "ComponentName1"}}}},
         {{"xyz.openbmc_project.Common.UUID",
           {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130004"}}},
          {{{3,
             {"ComponentName3",
              {{"inventory", "activation",
                "/xyz/openbmc_project/software/ComponentName3"}},
              "NVIDIA",
              false}},
            {4,
             {"ComponentName4",
              {{"inventory", "activation",
                "/xyz/openbmc_project/software/ComponentName4"}},
              "NVIDIA",
              false}}},
           {}}}});

    ComponentNameMapInfo componentNameMapInfo(
        {{{"xyz.openbmc_project.Common.UUID",
           {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130003"}}},
          {{1, "ComponentName1"}, {2, "ComponentName2"}}},
         {{"xyz.openbmc_project.Common.UUID",
           {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130004"}}},
          {{3, "ComponentName3"}, {4, "ComponentName4"}}}});

    FirmwareInventoryInfo outFwInventoryInfo;
    ComponentNameMapInfo outComponentNameMapConfig;

    parseConfig("./fw_update_jsons/fw_update_config_multiple_entry.json",
                outFwInventoryInfo, outComponentNameMapConfig);

    EXPECT_EQ(outFwInventoryInfo.infos, fwInventoryInfo.infos);
    EXPECT_EQ(outComponentNameMapConfig.infos, componentNameMapInfo.infos);
}

TEST(ParseConfig, LimitedEntry)
{
    FirmwareInventoryInfo fwInventoryInfo(
        {{{"xyz.openbmc_project.Common.UUID",
           {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130003"}}},
          {{{1,
             {"ComponentName1",
              {{"inventory", "activation",
                "/xyz/openbmc_project/software/ComponentName1"}},
              "NVIDIA",
              false}}},
           {}}}});

    ComponentNameMapInfo componentNameMapInfo(
        {{{"xyz.openbmc_project.Common.UUID",
           {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130003"}}},
          {{1, "ComponentName1"}, {2, "ComponentName2"}}}});

    FirmwareInventoryInfo outFwInventoryInfo;
    ComponentNameMapInfo outComponentNameMapConfig;

    parseConfig("./fw_update_jsons/fw_update_config_limited_entry.json",
                outFwInventoryInfo, outComponentNameMapConfig);

    EXPECT_EQ(outFwInventoryInfo.infos, fwInventoryInfo.infos);
    EXPECT_EQ(outComponentNameMapConfig.infos, componentNameMapInfo.infos);
}

TEST(ParseConfig, SingleEntryWithoutFwInvAssociations)
{
    FirmwareInventoryInfo fwInventoryInfo(
        {{{"xyz.openbmc_project.Common.UUID",
           {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130003"}}},
          {{{1, {"ComponentName1", {}, "NVIDIA", false}}},
           {{2, "ComponentName2"}}}}});

    ComponentNameMapInfo componentNameMapInfo(
        {{{"xyz.openbmc_project.Common.UUID",
           {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130003"}}},
          {{1, "ComponentName1"}, {2, "ComponentName2"}}}});

    FirmwareInventoryInfo outFwInventoryInfo;
    ComponentNameMapInfo outComponentNameMapConfig;

    parseConfig(
        "./fw_update_jsons/fw_update_config_fw_inv_without_associations.json",
        outFwInventoryInfo, outComponentNameMapConfig);

    EXPECT_EQ(outFwInventoryInfo.infos, fwInventoryInfo.infos);
    EXPECT_EQ(outComponentNameMapConfig.infos, componentNameMapInfo.infos);
}
