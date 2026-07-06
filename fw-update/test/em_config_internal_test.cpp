/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
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
#include "common/utils.hpp"

#include <sdbusplus/exception.hpp>

#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#ifdef DBusHandler
#undef DBusHandler
#endif

namespace pldm::utils
{

/** @brief Serves canned mapper/property responses to the em_config unit.
 *
 *  getSubtree returns (or throws) a configurable response.
 *  getDbusPropertiesVariant is keyed by "<objPath>|<interface>"; a missing
 *  key throws, which is also how the Components<N> probe loop terminates.
 */
class EmConfigTestDBusHandler : public DBusLoggingTestHandler
{
  public:
    static void reset()
    {
        subtreeResponse().clear();
        throwGetSubtree() = false;
        propsByKey().clear();
    }

    static void setSubtreeResponse(const GetSubTreeResponse& response)
    {
        subtreeResponse() = response;
    }

    static void setThrowGetSubtree(bool value)
    {
        throwGetSubtree() = value;
    }

    static void setProps(const std::string& objPath, const std::string& intf,
                         const PropertyMap& props)
    {
        propsByKey()[objPath + "|" + intf] = props;
    }

    GetSubTreeResponse getSubtree(
        const std::string&, int, const std::vector<std::string>&) const override
    {
        if (throwGetSubtree())
        {
            throw sdbusplus::exception::SdBusError(EIO, "mock getSubtree");
        }
        return subtreeResponse();
    }

    PropertyMap getDbusPropertiesVariant(
        const char* /*serviceName*/, const char* objPath,
        const char* dbusInterface) const override
    {
        auto it = propsByKey().find(
            std::string(objPath) + "|" + std::string(dbusInterface));
        if (it == propsByKey().end())
        {
            throw sdbusplus::exception::SdBusError(EIO, "mock GetAll");
        }
        return it->second;
    }

  private:
    static GetSubTreeResponse& subtreeResponse()
    {
        static GetSubTreeResponse response{};
        return response;
    }

    static bool& throwGetSubtree()
    {
        static bool value = false;
        return value;
    }

    static std::map<std::string, PropertyMap>& propsByKey()
    {
        static std::map<std::string, PropertyMap> props{};
        return props;
    }
};

} // namespace pldm::utils

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define DBusHandler EmConfigTestDBusHandler
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "fw-update/em_config.cpp" // NOLINT(bugprone-suspicious-include)
#undef DBusHandler

namespace
{

using namespace pldm;
using namespace pldm::fw_update;
using pldm::utils::EmConfigTestDBusHandler;

constexpr auto fwDeviceIntf =
    "xyz.openbmc_project.Configuration.PLDMFirmwareDevice";
constexpr auto devPath = "/xyz/openbmc_project/inventory/system/dev0";

MctpInfo makeMctpInfo(eid mctpEid, const std::optional<std::string>& name)
{
    MctpInfo info{};
    std::get<eid>(info) = mctpEid;
    std::get<std::optional<std::string>>(info) = name;
    return info;
}

class EmConfigInternalTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        EmConfigTestDBusHandler::reset();
    }

    void TearDown() override
    {
        EmConfigTestDBusHandler::reset();
    }
};

TEST_F(EmConfigInternalTest, targetNameForEidEmptyConfigurations)
{
    Configurations configurations;
    EXPECT_EQ(em_config::targetNameForEid(configurations, 12), "");
}

TEST_F(EmConfigInternalTest, targetNameForEidResolvesMatchingEid)
{
    Configurations configurations;
    configurations.emplace("/em/dev0", makeMctpInfo(12, "ERoT_GPU_0"));
    configurations.emplace("/em/dev1", makeMctpInfo(13, "ERoT_GPU_1"));
    EXPECT_EQ(em_config::targetNameForEid(configurations, 12), "ERoT_GPU_0");
    EXPECT_EQ(em_config::targetNameForEid(configurations, 13), "ERoT_GPU_1");
}

TEST_F(EmConfigInternalTest, targetNameForEidUnmatchedEidReturnsEmpty)
{
    Configurations configurations;
    configurations.emplace("/em/dev0", makeMctpInfo(12, "ERoT_GPU_0"));
    EXPECT_EQ(em_config::targetNameForEid(configurations, 99), "");
}

TEST_F(EmConfigInternalTest, targetNameForEidNamelessEntryReturnsEmpty)
{
    Configurations configurations;
    configurations.emplace("/em/dev0", makeMctpInfo(12, std::nullopt));
    EXPECT_EQ(em_config::targetNameForEid(configurations, 12), "");
}

TEST_F(EmConfigInternalTest, fetchComponentInfoUnresolvedNameReturnsNullopt)
{
    Configurations configurations;
    EXPECT_EQ(em_config::fetchComponentInfo(configurations, 12), std::nullopt);
}

TEST_F(EmConfigInternalTest, fetchComponentInfoSubtreeThrowReturnsNullopt)
{
    Configurations configurations;
    configurations.emplace("/em/dev0", makeMctpInfo(12, "ERoT_GPU_0"));
    EmConfigTestDBusHandler::setThrowGetSubtree(true);
    EXPECT_EQ(em_config::fetchComponentInfo(configurations, 12), std::nullopt);
}

TEST_F(EmConfigInternalTest, fetchComponentInfoNoMatchingEntryReturnsNullopt)
{
    Configurations configurations;
    configurations.emplace("/em/dev0", makeMctpInfo(12, "ERoT_GPU_0"));

    // Entry 0: empty service map — skipped.
    // Entry 1: props read throws (no canned props) — skipped.
    // Entry 2: no MCTPTargetName property — skipped.
    // Entry 3: MCTPTargetName has a non-string type — skipped.
    // Entry 4: MCTPTargetName differs — skipped.
    EmConfigTestDBusHandler::setSubtreeResponse({
        {"/em/empty", {}},
        {"/em/throws", {{"svc", {fwDeviceIntf}}}},
        {"/em/noname", {{"svc", {fwDeviceIntf}}}},
        {"/em/wrongtype", {{"svc", {fwDeviceIntf}}}},
        {"/em/other", {{"svc", {fwDeviceIntf}}}},
    });
    EmConfigTestDBusHandler::setProps("/em/noname", fwDeviceIntf,
                                      {{"Type", std::string("PLDMDevice")}});
    EmConfigTestDBusHandler::setProps("/em/wrongtype", fwDeviceIntf,
                                      {{"MCTPTargetName", uint8_t{7}}});
    EmConfigTestDBusHandler::setProps(
        "/em/other", fwDeviceIntf,
        {{"MCTPTargetName", std::string("ERoT_GPU_1")}});

    EXPECT_EQ(em_config::fetchComponentInfo(configurations, 12), std::nullopt);
}

TEST_F(EmConfigInternalTest, fetchComponentInfoMatchWithoutComponents)
{
    Configurations configurations;
    configurations.emplace("/em/dev0", makeMctpInfo(12, "ERoT_GPU_0"));

    EmConfigTestDBusHandler::setSubtreeResponse(
        {{devPath, {{"svc", {fwDeviceIntf}}}}});
    EmConfigTestDBusHandler::setProps(
        devPath, fwDeviceIntf, {{"MCTPTargetName", std::string("ERoT_GPU_0")}});
    // No Components0 child interface — probe loop ends immediately.

    auto info = em_config::fetchComponentInfo(configurations, 12);
    ASSERT_TRUE(info.has_value());
    EXPECT_TRUE(info->emComponents.empty());
    EXPECT_TRUE(info->idNameMap.empty());
}

TEST_F(EmConfigInternalTest, fetchComponentInfoUnpacksComponents)
{
    Configurations configurations;
    configurations.emplace("/em/dev0", makeMctpInfo(12, "ERoT_GPU_0"));

    EmConfigTestDBusHandler::setSubtreeResponse(
        {{devPath, {{"svc", {fwDeviceIntf}}}}});
    EmConfigTestDBusHandler::setProps(
        devPath, fwDeviceIntf, {{"MCTPTargetName", std::string("ERoT_GPU_0")}});

    const std::string base = std::string(fwDeviceIntf) + ".Components";

    // Components0: fully specified, associations zipped, UpdateOnly set.
    EmConfigTestDBusHandler::setProps(
        devPath, base + "0",
        {{"Name", std::string("GPU_FW")},
         {"ComponentIdentifier", uint16_t{1}},
         {"Manufacturer", std::string("ACME")},
         {"AssociationForward",
          std::vector<std::string>{"inventory", "activation"}},
         {"AssociationBackward",
          std::vector<std::string>{"activation_target", "software"}},
         {"AssociationEndpoint",
          std::vector<std::string>{"/inv/gpu0", "/sw/gpu0"}},
         {"UpdateOnly", true}});

    // Components1: missing Name — skipped.
    EmConfigTestDBusHandler::setProps(devPath, base + "1",
                                      {{"ComponentIdentifier", uint16_t{2}}});

    // Components2: Name has wrong type — skipped.
    EmConfigTestDBusHandler::setProps(
        devPath, base + "2",
        {{"Name", uint8_t{3}}, {"ComponentIdentifier", uint16_t{3}}});

    // Components3: identifier published as uint64.
    EmConfigTestDBusHandler::setProps(
        devPath, base + "3",
        {{"Name", std::string("C64")}, {"ComponentIdentifier", uint64_t{4}}});

    // Components4: identifier published as uint32.
    EmConfigTestDBusHandler::setProps(
        devPath, base + "4",
        {{"Name", std::string("C32")}, {"ComponentIdentifier", uint32_t{5}}});

    // Components5: identifier published as int64.
    EmConfigTestDBusHandler::setProps(
        devPath, base + "5",
        {{"Name", std::string("CI64")}, {"ComponentIdentifier", int64_t{6}}});

    // Components6: identifier published as double.
    EmConfigTestDBusHandler::setProps(
        devPath, base + "6",
        {{"Name", std::string("CD")}, {"ComponentIdentifier", double{7.0}}});

    // Components7: identifier has an unsupported type — skipped.
    EmConfigTestDBusHandler::setProps(
        devPath, base + "7",
        {{"Name", std::string("CB")}, {"ComponentIdentifier", true}});

    // Components8: wrong-typed Manufacturer/associations/UpdateOnly fall
    // back to defaults; mismatched association array lengths zip to the
    // shortest.
    EmConfigTestDBusHandler::setProps(
        devPath, base + "8",
        {{"Name", std::string("CDefaults")},
         {"ComponentIdentifier", uint16_t{9}},
         {"Manufacturer", uint8_t{1}},
         {"AssociationForward", std::vector<std::string>{"fwd0", "fwd1"}},
         {"AssociationBackward", std::vector<std::string>{"bwd0"}},
         {"AssociationEndpoint", std::string("not-a-list")},
         {"UpdateOnly", std::string("yes")}});

    // Components9: empty property map — terminates the probe loop.
    EmConfigTestDBusHandler::setProps(devPath, base + "9", {});

    auto info = em_config::fetchComponentInfo(configurations, 12);
    ASSERT_TRUE(info.has_value());

    // Skipped: 1 (no Name), 2 (bad Name), 7 (bad identifier type).
    EXPECT_EQ(info->emComponents.size(), 6u);
    EXPECT_EQ(info->idNameMap.size(), 6u);

    const auto& [name0, assocs0, manufacturer0, updateOnly0] =
        info->emComponents.at(1);
    EXPECT_EQ(name0, "GPU_FW");
    EXPECT_EQ(manufacturer0, "ACME");
    EXPECT_TRUE(updateOnly0);
    ASSERT_EQ(assocs0.size(), 2u);
    EXPECT_EQ(assocs0[0], std::make_tuple(std::string("inventory"),
                                          std::string("activation_target"),
                                          std::string("/inv/gpu0")));
    EXPECT_EQ(assocs0[1], std::make_tuple(std::string("activation"),
                                          std::string("software"),
                                          std::string("/sw/gpu0")));
    EXPECT_EQ(info->idNameMap.at(1), "GPU_FW");

    EXPECT_EQ(std::get<0>(info->emComponents.at(4)), "C64");
    EXPECT_EQ(std::get<0>(info->emComponents.at(5)), "C32");
    EXPECT_EQ(std::get<0>(info->emComponents.at(6)), "CI64");
    EXPECT_EQ(std::get<0>(info->emComponents.at(7)), "CD");

    const auto& [name8, assocs8, manufacturer8, updateOnly8] =
        info->emComponents.at(9);
    EXPECT_EQ(name8, "CDefaults");
    EXPECT_EQ(manufacturer8, "NVIDIA"); // wrong-typed Manufacturer → default
    EXPECT_FALSE(updateOnly8);          // wrong-typed UpdateOnly → default
    EXPECT_TRUE(assocs8.empty());       // endpoint list unreadable → zip to 0
}

} // namespace
