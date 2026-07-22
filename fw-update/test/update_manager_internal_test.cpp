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

// Pre-include every project header update_manager.cpp pulls in so the
// DBusHandler macro swap below only affects the .cpp body, not any header
// declarations (include guards make the .cpp's own includes no-ops).
#include "common/mmap_stream.hpp"
#include "common/types.hpp"
#include "common/utils.hpp"
#include "fw-update/activation.hpp"
#include "fw-update/config.hpp"
#include "fw-update/error_handling.hpp"
#include "fw-update/package_parser.hpp"
#include "fw-update/package_signature.hpp"
#include "fw-update/update_manager.hpp"

#include <sdbusplus/exception.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#ifdef DBusHandler
#undef DBusHandler
#endif

namespace pldm::utils
{

/** @brief Serves canned mapper/property responses to the update_manager
 *         unit.
 *
 *  getSubtree is keyed by the first requested interface (or throws when the
 *  interface is marked to throw). getDbusPropertiesVariant is keyed by
 *  "<objPath>|<interface>"; a missing key throws, exercising the skip
 *  branches of seedRefreshEidsFromStaticConfig.
 */
class UpdateManagerTestDBusHandler : public DBusLoggingTestHandler
{
  public:
    static void reset()
    {
        subtreeByIntf().clear();
        throwIntfs().clear();
        propsByKey().clear();
    }

    static void setSubtreeResponse(const std::string& intf,
                                   const GetSubTreeResponse& response)
    {
        subtreeByIntf()[intf] = response;
    }

    static void setThrowGetSubtree(const std::string& intf)
    {
        throwIntfs().insert(intf);
    }

    static void setProps(const std::string& objPath, const std::string& intf,
                         const PropertyMap& props)
    {
        propsByKey()[objPath + "|" + intf] = props;
    }

    GetSubTreeResponse getSubtree(
        const std::string&, int,
        const std::vector<std::string>& intfs) const override
    {
        const std::string key = intfs.empty() ? "" : intfs.front();
        if (throwIntfs().contains(key))
        {
            throw sdbusplus::exception::SdBusError(EIO, "mock getSubtree");
        }
        auto it = subtreeByIntf().find(key);
        if (it == subtreeByIntf().end())
        {
            return {};
        }
        return it->second;
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
    static std::map<std::string, GetSubTreeResponse>& subtreeByIntf()
    {
        static std::map<std::string, GetSubTreeResponse> response{};
        return response;
    }

    static std::set<std::string>& throwIntfs()
    {
        static std::set<std::string> intfs{};
        return intfs;
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
#define DBusHandler UpdateManagerTestDBusHandler
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "fw-update/update_manager.cpp" // NOLINT(bugprone-suspicious-include)
#undef DBusHandler

namespace
{

using pldm::utils::UpdateManagerTestDBusHandler;

constexpr auto usbIntf = "xyz.openbmc_project.Configuration.MCTPUSBDevice";
constexpr auto i2cIntf = "xyz.openbmc_project.Configuration.MCTPI2CTarget";
constexpr auto spiIntf = "xyz.openbmc_project.Configuration.MCTPSPIDevice";

class UpdateManagerInternalTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        UpdateManagerTestDBusHandler::reset();
    }

    void TearDown() override
    {
        UpdateManagerTestDBusHandler::reset();
    }
};

TEST_F(UpdateManagerInternalTest, seedRefreshAllSubtreesThrowKeepsExisting)
{
    UpdateManagerTestDBusHandler::setThrowGetSubtree(usbIntf);
    UpdateManagerTestDBusHandler::setThrowGetSubtree(i2cIntf);
    UpdateManagerTestDBusHandler::setThrowGetSubtree(spiIntf);

    std::set<mctp_eid_t> eids{5};
    pldm::fw_update::seedRefreshEidsFromStaticConfig(eids);

    EXPECT_EQ(eids, (std::set<mctp_eid_t>{5}));
}

TEST_F(UpdateManagerInternalTest, seedRefreshCollectsStaticAndBridgePoolEids)
{
    // USB: one entry with an empty service map (skipped) and one carrying a
    // uint64 StaticEndpointID.
    UpdateManagerTestDBusHandler::setSubtreeResponse(
        usbIntf, {{"/inv/usb-empty", {}}, {"/inv/usb0", {{"svc", {usbIntf}}}}});
    UpdateManagerTestDBusHandler::setProps(
        "/inv/usb0", usbIntf, {{"StaticEndpointID", uint64_t{30}}});

    // I2C: one entry whose props read throws (skipped) and one carrying a
    // decimal-string StaticEndpointID.
    UpdateManagerTestDBusHandler::setSubtreeResponse(
        i2cIntf, {{"/inv/i2c-throws", {{"svc", {i2cIntf}}}},
                  {"/inv/i2c0", {{"svc", {i2cIntf}}}}});
    UpdateManagerTestDBusHandler::setProps(
        "/inv/i2c0", i2cIntf, {{"StaticEndpointID", std::string("31")}});

    // SPI: a bridge declaring a downstream EID pool 40..42.
    UpdateManagerTestDBusHandler::setSubtreeResponse(
        spiIntf, {{"/inv/spi0", {{"svc", {spiIntf}}}}});
    UpdateManagerTestDBusHandler::setProps(
        "/inv/spi0", spiIntf,
        {{"BridgePoolStartEid", uint64_t{40}},
         {"BridgePoolEndEID", uint64_t{42}}});

    std::set<mctp_eid_t> eids;
    pldm::fw_update::seedRefreshEidsFromStaticConfig(eids);

    EXPECT_EQ(eids, (std::set<mctp_eid_t>{30, 31, 40, 41, 42}));
}

TEST_F(UpdateManagerInternalTest, seedRefreshIgnoresInvalidEidsAndPools)
{
    // Out-of-range StaticEndpointID is rejected by readOptionalEidProperty.
    UpdateManagerTestDBusHandler::setSubtreeResponse(
        usbIntf, {{"/inv/usb0", {{"svc", {usbIntf}}}}});
    UpdateManagerTestDBusHandler::setProps(
        "/inv/usb0", usbIntf, {{"StaticEndpointID", int64_t{300}}});

    // Inverted pool range inserts nothing.
    UpdateManagerTestDBusHandler::setSubtreeResponse(
        i2cIntf, {{"/inv/i2c0", {{"svc", {i2cIntf}}}}});
    UpdateManagerTestDBusHandler::setProps(
        "/inv/i2c0", i2cIntf,
        {{"BridgePoolStartEid", uint64_t{42}},
         {"BridgePoolEndEID", uint64_t{40}}});

    // Pool start without an end inserts nothing.
    UpdateManagerTestDBusHandler::setSubtreeResponse(
        spiIntf, {{"/inv/spi0", {{"svc", {spiIntf}}}}});
    UpdateManagerTestDBusHandler::setProps(
        "/inv/spi0", spiIntf, {{"BridgePoolStartEid", uint64_t{10}}});

    std::set<mctp_eid_t> eids;
    pldm::fw_update::seedRefreshEidsFromStaticConfig(eids);

    EXPECT_TRUE(eids.empty());
}

} // namespace
