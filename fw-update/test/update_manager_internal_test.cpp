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

#include <limits>
#include <map>
#include <optional>
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
class UpdateManagerTestDBusHandler : public DBusHandler
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

TEST_F(UpdateManagerInternalTest, seedWithoutBridgePoolsCollectsStaticEidsOnly)
{
    // The pre-update validation scope seeds without bridge pools: a pool is
    // an address range, so its unpopulated slots are not devices.
    UpdateManagerTestDBusHandler::setSubtreeResponse(
        usbIntf, {{"/inv/usb0", {{"svc", {usbIntf}}}}});
    UpdateManagerTestDBusHandler::setProps(
        "/inv/usb0", usbIntf, {{"StaticEndpointID", uint64_t{30}}});
    UpdateManagerTestDBusHandler::setSubtreeResponse(
        spiIntf, {{"/inv/spi0", {{"svc", {spiIntf}}}}});
    UpdateManagerTestDBusHandler::setProps(
        "/inv/spi0", spiIntf,
        {{"StaticEndpointID", uint64_t{12}},
         {"BridgePoolStartEid", uint64_t{40}},
         {"BridgePoolEndEID", uint64_t{42}}});

    std::set<mctp_eid_t> eids;
    pldm::fw_update::seedRefreshEidsFromStaticConfig(eids, false);

    EXPECT_EQ(eids, (std::set<mctp_eid_t>{12, 30}));
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

TEST_F(UpdateManagerInternalTest, seedRefreshSkipsEndpointsThatIgnorePldm)
{
    // A bridge entry (e.g. HMC_MCTP_BRIDGE) whose IgnoreMessageTypes lists
    // MCTP message type 1 (PLDM) must not have its own EID seeded, but its
    // downstream pool is still covered.
    UpdateManagerTestDBusHandler::setSubtreeResponse(
        usbIntf, {{"/inv/hmc-bridge", {{"svc", {usbIntf}}}},
                  {"/inv/usb0", {{"svc", {usbIntf}}}}});
    UpdateManagerTestDBusHandler::setProps(
        "/inv/hmc-bridge", usbIntf,
        {{"StaticEndpointID", uint64_t{8}},
         {"IgnoreMessageTypes", std::string("1,2,3,4,6,126,127")},
         {"BridgePoolStartEid", uint64_t{20}},
         {"BridgePoolEndEID", uint64_t{21}}});
    // An entry ignoring only non-PLDM types is still seeded.
    UpdateManagerTestDBusHandler::setProps(
        "/inv/usb0", usbIntf,
        {{"StaticEndpointID", uint64_t{30}},
         {"IgnoreMessageTypes", std::string("2, 6")}});

    std::set<mctp_eid_t> eids;
    pldm::fw_update::seedRefreshEidsFromStaticConfig(eids);

    EXPECT_EQ(eids, (std::set<mctp_eid_t>{20, 21, 30}));
}

TEST_F(UpdateManagerInternalTest, seedRefreshIgnoreTypesMatchesWholeTokens)
{
    // "126" and "127" must not match type 1 by substring; a lone "1" token
    // (with surrounding spaces) must.
    UpdateManagerTestDBusHandler::setSubtreeResponse(
        usbIntf, {{"/inv/usb0", {{"svc", {usbIntf}}}},
                  {"/inv/usb1", {{"svc", {usbIntf}}}}});
    UpdateManagerTestDBusHandler::setProps(
        "/inv/usb0", usbIntf,
        {{"StaticEndpointID", uint64_t{40}},
         {"IgnoreMessageTypes", std::string("126,127")}});
    UpdateManagerTestDBusHandler::setProps(
        "/inv/usb1", usbIntf,
        {{"StaticEndpointID", uint64_t{41}},
         {"IgnoreMessageTypes", std::string("6, 1 ,126")}});

    std::set<mctp_eid_t> eids;
    pldm::fw_update::seedRefreshEidsFromStaticConfig(eids);

    EXPECT_EQ(eids, (std::set<mctp_eid_t>{40}));
}

#ifdef OEM_NVIDIA
/** @brief Publish an NvDevice @p name at @p path, with @p eid if set, and
 *         its .Transport properties @p transport. */
void publishDevice(const std::string& path, const std::string& name,
                   std::optional<uint64_t> eid,
                   const pldm::utils::PropertyMap& transport,
                   pldm::utils::GetSubTreeResponse& subtree)
{
    subtree.emplace_back(
        path, std::vector<std::pair<std::string, std::vector<std::string>>>{
                  {"svc", {pldm::em_device::nvDeviceIntf}}});
    pldm::utils::PropertyMap props{{"Name", name}};
    if (eid)
    {
        props.emplace("EID", *eid);
    }
    UpdateManagerTestDBusHandler::setProps(path, pldm::em_device::nvDeviceIntf,
                                           props);
    UpdateManagerTestDBusHandler::setProps(path, pldm::em_device::transportIntf,
                                           transport);
}

TEST_F(UpdateManagerInternalTest, seedRefreshCollectsTopologyBridgePools)
{
    pldm::utils::GetSubTreeResponse subtree;
    publishDevice("/inv/a", "A", 12,
                  {{"Medium", std::string("spi")},
                   {"PoolStartEID", uint64_t{40}},
                   {"PoolEndEID", uint64_t{42}}},
                  subtree);
    // the last EID a device can have closes the pool
    publishDevice("/inv/b", "B", std::nullopt,
                  {{"Medium", std::string("usb")},
                   {"PoolStartEID", uint64_t{0xFD}},
                   {"PoolEndEID", uint64_t{0xFE}}},
                  subtree);
    // a pool of one EID
    publishDevice("/inv/c", "C", std::nullopt,
                  {{"Medium", std::string("i2c")},
                   {"PoolStartEID", uint64_t{50}},
                   {"PoolEndEID", uint64_t{50}}},
                  subtree);
    // a device behind a bridge (Medium "bridge") is not seeded, nor is its
    // pool
    publishDevice("/inv/d", "D", 13,
                  {{"Medium", std::string("bridge")},
                   {"PoolStartEID", uint64_t{60}},
                   {"PoolEndEID", uint64_t{61}}},
                  subtree);
    // a bridge whose MCTPEndpoint override record ignores PLDM: its own EID
    // is not seeded, the devices of its pool are
    publishDevice("/inv/e", "E", 8,
                  {{"Medium", std::string("usb")},
                   {"PoolStartEID", uint64_t{20}},
                   {"PoolEndEID", uint64_t{21}}},
                  subtree);
    UpdateManagerTestDBusHandler::setSubtreeResponse(
        pldm::em_device::nvDeviceIntf, subtree);
    const auto* mctpEndpointIntf =
        pldm::fw_update::em_device_policy::mctpEndpointIntf;
    UpdateManagerTestDBusHandler::setSubtreeResponse(
        mctpEndpointIntf, {{"/inv/e_mctp", {{"svc", {mctpEndpointIntf}}}}});
    UpdateManagerTestDBusHandler::setProps(
        "/inv/e_mctp", mctpEndpointIntf,
        {{"Name", std::string("E_MCTP")},
         {"IgnoreMessageTypes", std::string("1")}});

    std::set<mctp_eid_t> eids;
    pldm::fw_update::seedRefreshEidsFromStaticConfig(eids);
    EXPECT_EQ(eids,
              (std::set<mctp_eid_t>{12, 20, 21, 40, 41, 42, 50, 0xFD, 0xFE}));

    // the pre-update validation scope leaves the pools out, not the devices
    eids.clear();
    pldm::fw_update::seedRefreshEidsFromStaticConfig(eids, false);
    EXPECT_EQ(eids, (std::set<mctp_eid_t>{12}));
}

TEST_F(UpdateManagerInternalTest, seedRefreshSkipsBadTopologyBridgePools)
{
    pldm::utils::GetSubTreeResponse subtree;
    // a pool with a single usable bound is skipped; devices() drops a bound
    // outside [0, maxDeviceEid]
    // - an end past the EID range
    publishDevice("/inv/a", "A", std::nullopt,
                  {{"Medium", std::string("usb")},
                   {"PoolStartEID", uint64_t{40}},
                   {"PoolEndEID", std::numeric_limits<uint64_t>::max()}},
                  subtree);
    // - the broadcast EID as the end
    publishDevice("/inv/b", "B", std::nullopt,
                  {{"Medium", std::string("usb")},
                   {"PoolStartEID", uint64_t{70}},
                   {"PoolEndEID", uint64_t{0xFF}}},
                  subtree);
    // - a start past the EID range
    publishDevice("/inv/c", "C", std::nullopt,
                  {{"Medium", std::string("usb")},
                   {"PoolStartEID", uint64_t{0x100}},
                   {"PoolEndEID", uint64_t{80}}},
                  subtree);
    // - an end without a start
    publishDevice(
        "/inv/e", "E", std::nullopt,
        {{"Medium", std::string("usb")}, {"PoolEndEID", uint64_t{100}}},
        subtree);
    // an inverted pool
    publishDevice("/inv/d", "D", std::nullopt,
                  {{"Medium", std::string("usb")},
                   {"PoolStartEID", uint64_t{91}},
                   {"PoolEndEID", uint64_t{90}}},
                  subtree);
    // a device with an EID and no pool is still seeded
    publishDevice("/inv/f", "F", 30, {{"Medium", std::string("usb")}}, subtree);
    UpdateManagerTestDBusHandler::setSubtreeResponse(
        pldm::em_device::nvDeviceIntf, subtree);

    std::set<mctp_eid_t> eids;
    pldm::fw_update::seedRefreshEidsFromStaticConfig(eids);
    EXPECT_EQ(eids, (std::set<mctp_eid_t>{30}));
}

TEST_F(UpdateManagerInternalTest, seedBridgePoolBoundsAnUncheckedDevice)
{
    // a Device not read through em_device::devices() can carry an end past
    // the EID range; the pool stops at the last EID a device can have
    pldm::em_device::Device dev;
    dev.name = "A";
    dev.poolStartEid = 0xFD;
    dev.poolEndEid = std::numeric_limits<uint64_t>::max();

    std::set<mctp_eid_t> eids;
    pldm::fw_update::seedBridgePool(dev, eids);
    EXPECT_EQ(eids, (std::set<mctp_eid_t>{0xFD, 0xFE}));
}
#endif

} // namespace
