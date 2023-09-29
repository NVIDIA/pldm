#include "common/types.hpp"
#include "fw-update/config.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldm::fw_update;

TEST(ParseConfig, SingleEntry)
{
    DeviceInventoryInfo deviceInventoryInfo({
        {{"xyz.openbmc_project.Common.UUID", {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130003"}}},
         {{"/xyz/openbmc_project/inventory/chassis/DeviceName1",
           {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"}}},
          "/xyz/openbmc_project/inventory/chassis/DeviceName2"}}});

    FirmwareInventoryInfo fwInventoryInfo({
        {{"xyz.openbmc_project.Common.UUID", {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130003"}}},
         {{{1, {"ComponentName1", 
           {{"inventory", "activation", "/xyz/openbmc_project/software/ComponentName1"}}}}}, 
         {{2, "ComponentName2"}}}}});

    ComponentNameMapInfo componentNameMapInfo({
        {{"xyz.openbmc_project.Common.UUID", {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130003"}}},
         {{1, "ComponentName1"}, {2, "ComponentName2"}}}});

    DeviceInventoryInfo outdeviceInventoryInfo;
    FirmwareInventoryInfo outFwInventoryInfo;
    ComponentNameMapInfo outComponentNameMapConfig;

    parseConfig("./fw_update_jsons/fw_update_config_single_entry.json",
                outdeviceInventoryInfo, outFwInventoryInfo,
                outComponentNameMapConfig);

    EXPECT_EQ(outdeviceInventoryInfo.infos, deviceInventoryInfo.infos);
    EXPECT_EQ(outFwInventoryInfo.infos, fwInventoryInfo.infos);
    EXPECT_EQ(outComponentNameMapConfig.infos, componentNameMapInfo.infos);
}

TEST(ParseConfig, CombinedPropertyMatch)
{
    DeviceInventoryInfo deviceInventoryInfo({
        {{"xyz.openbmc_project.Inventory.Decorator.I2CDevice", {{"Address", uint32_t(0)}, {"Bus", uint32_t(16)}}},
         {{"/xyz/openbmc_project/inventory/chassis/DeviceName1",
           {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"}}},
          "/xyz/openbmc_project/inventory/chassis/DeviceName2"}}});

    FirmwareInventoryInfo fwInventoryInfo({
        {{"xyz.openbmc_project.Inventory.Decorator.I2CDevice", {{"Address", uint32_t(0)}, {"Bus", uint32_t(16)}}},
         {{{1, {"ComponentName1", 
           {{"inventory", "activation", "/xyz/openbmc_project/software/ComponentName1"}}}}}, 
         {{2, "ComponentName2"}}}}});

    ComponentNameMapInfo componentNameMapInfo({
        {{"xyz.openbmc_project.Inventory.Decorator.I2CDevice", {{"Address", uint32_t(0)}, {"Bus", uint32_t(16)}}},
         {{1, "ComponentName1"}, {2, "ComponentName2"}}}});

    DeviceInventoryInfo outdeviceInventoryInfo;
    FirmwareInventoryInfo outFwInventoryInfo;
    ComponentNameMapInfo outComponentNameMapConfig;

    parseConfig("./fw_update_jsons/fw_update_config_combined_properties_match.json",
                outdeviceInventoryInfo, outFwInventoryInfo,
                outComponentNameMapConfig);

    EXPECT_EQ(outdeviceInventoryInfo.infos, deviceInventoryInfo.infos);
    EXPECT_EQ(outFwInventoryInfo.infos, fwInventoryInfo.infos);
    EXPECT_EQ(outComponentNameMapConfig.infos, componentNameMapInfo.infos);

    pldm::dbus::InterfaceMap interfaceMap = {{"xyz.openbmc_project.Inventory.Decorator.I2CDevice", {{"Address", uint32_t(0)}, {"Bus", uint32_t(16)}}}};
    DeviceInfo deviceInfo;

    EXPECT_EQ(outdeviceInventoryInfo.matchInventoryEntry(interfaceMap, deviceInfo), true);
}

TEST(ParseConfig, MultipleEntry)
{
    DeviceInventoryInfo deviceInventoryInfo({
        {{"xyz.openbmc_project.Common.UUID", {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130003"}}},
         {{"/xyz/openbmc_project/inventory/chassis/DeviceName1",
           {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"},
            {"right", "left", "/xyz/openbmc_project/inventory/direction"}}},
          {}}},
        {{"xyz.openbmc_project.Common.UUID", {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130004"}}},
         {{"", {}}, "/xyz/openbmc_project/inventory/chassis/DeviceName2"}}});

    FirmwareInventoryInfo fwInventoryInfo({
        {{"xyz.openbmc_project.Common.UUID", {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130003"}}}, {{}, {{1, "ComponentName1"}}}},
        {{"xyz.openbmc_project.Common.UUID", {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130004"}}},
         {{{3, {"ComponentName3", 
            {{"inventory", "activation", "/xyz/openbmc_project/software/ComponentName3"}}}}, 
           {4, {"ComponentName4", 
            {{"inventory", "activation", "/xyz/openbmc_project/software/ComponentName4"}}}}
           }, 
        {}}}});

    ComponentNameMapInfo componentNameMapInfo({
        {{"xyz.openbmc_project.Common.UUID", {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130003"}}},
         {{1, "ComponentName1"}, {2, "ComponentName2"}}},
        {{"xyz.openbmc_project.Common.UUID", {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130004"}}},
         {{3, "ComponentName3"}, {4, "ComponentName4"}}}});

    DeviceInventoryInfo outdeviceInventoryInfo;
    FirmwareInventoryInfo outFwInventoryInfo;
    ComponentNameMapInfo outComponentNameMapConfig;

    parseConfig("./fw_update_jsons/fw_update_config_multiple_entry.json",
                outdeviceInventoryInfo, outFwInventoryInfo,
                outComponentNameMapConfig);

    EXPECT_EQ(outdeviceInventoryInfo.infos, deviceInventoryInfo.infos);
    EXPECT_EQ(outFwInventoryInfo.infos, fwInventoryInfo.infos);
    EXPECT_EQ(outComponentNameMapConfig.infos, componentNameMapInfo.infos);
}

TEST(ParseConfig, LimitedEntry)
{
    DeviceInventoryInfo deviceInventoryInfo{};
    FirmwareInventoryInfo fwInventoryInfo({
        {{"xyz.openbmc_project.Common.UUID", {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130003"}}},
         {{{1, {"ComponentName1", 
          {{"inventory", "activation", "/xyz/openbmc_project/software/ComponentName1"}}}}
         }, 
        {}}}});

    ComponentNameMapInfo componentNameMapInfo({
        {{"xyz.openbmc_project.Common.UUID", {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130003"}}},
         {{1, "ComponentName1"}, {2, "ComponentName2"}}}});

    DeviceInventoryInfo outdeviceInventoryInfo;
    FirmwareInventoryInfo outFwInventoryInfo;
    ComponentNameMapInfo outComponentNameMapConfig;

    parseConfig("./fw_update_jsons/fw_update_config_limited_entry.json",
                outdeviceInventoryInfo, outFwInventoryInfo,
                outComponentNameMapConfig);

    EXPECT_EQ(outdeviceInventoryInfo.infos, deviceInventoryInfo.infos);
    EXPECT_EQ(outFwInventoryInfo.infos, fwInventoryInfo.infos);
    EXPECT_EQ(outComponentNameMapConfig.infos, componentNameMapInfo.infos);
}

TEST(ParseConfig, SingleEntryWithoutFwInvAssociations)
{
    DeviceInventoryInfo deviceInventoryInfo({
        {{"xyz.openbmc_project.Common.UUID", {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130003"}}},
         {{"/xyz/openbmc_project/inventory/chassis/DeviceName1",
           {{"parent", "child", "/xyz/openbmc_project/inventory/chassis"}}},
          "/xyz/openbmc_project/inventory/chassis/DeviceName2"}}});

    FirmwareInventoryInfo fwInventoryInfo({
        {{"xyz.openbmc_project.Common.UUID", {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130003"}}},
         {{{1, {"ComponentName1", 
           {}}}}, 
         {{2, "ComponentName2"}}}}});

    ComponentNameMapInfo componentNameMapInfo({
        {{"xyz.openbmc_project.Common.UUID", {{"UUID", "ad4c8360-c54c-11eb-8529-0242ac130003"}}},
         {{1, "ComponentName1"}, {2, "ComponentName2"}}}});

    DeviceInventoryInfo outdeviceInventoryInfo;
    FirmwareInventoryInfo outFwInventoryInfo;
    ComponentNameMapInfo outComponentNameMapConfig;

    parseConfig("./fw_update_jsons/fw_update_config_fw_inv_without_associations.json",
                outdeviceInventoryInfo, outFwInventoryInfo,
                outComponentNameMapConfig);

    EXPECT_EQ(outdeviceInventoryInfo.infos, deviceInventoryInfo.infos);
    EXPECT_EQ(outFwInventoryInfo.infos, fwInventoryInfo.infos);
    EXPECT_EQ(outComponentNameMapConfig.infos, componentNameMapInfo.infos);
}