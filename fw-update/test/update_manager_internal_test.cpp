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
        {{"BridgePoolStartEID", uint64_t{40}},
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
         {"BridgePoolStartEID", uint64_t{40}},
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
        {{"BridgePoolStartEID", uint64_t{42}},
         {"BridgePoolEndEID", uint64_t{40}}});

    // Pool start without an end inserts nothing.
    UpdateManagerTestDBusHandler::setSubtreeResponse(
        spiIntf, {{"/inv/spi0", {{"svc", {spiIntf}}}}});
    UpdateManagerTestDBusHandler::setProps(
        "/inv/spi0", spiIntf, {{"BridgePoolStartEID", uint64_t{10}}});

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
         {"BridgePoolStartEID", uint64_t{20}},
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

TEST_F(UpdateManagerInternalTest, seedRefreshLeavesOutTheIgnoredPoolEids)
{
    // the IO board pool 61..65 with 62 and 64 declared as not populated
    UpdateManagerTestDBusHandler::setSubtreeResponse(
        usbIntf, {{"/inv/usb0", {{"svc", {usbIntf}}}},
                  {"/inv/usb1", {{"svc", {usbIntf}}}}});
    UpdateManagerTestDBusHandler::setProps(
        "/inv/usb0", usbIntf,
        {{"StaticEndpointID", uint64_t{60}},
         {"BridgePoolStartEID", uint64_t{61}},
         {"BridgePoolEndEID", uint64_t{65}},
         {"IgnoreEIDs", std::string("62, 64")}});
    // a list with a token that is no EID keeps the valid ones
    UpdateManagerTestDBusHandler::setProps(
        "/inv/usb1", usbIntf,
        {{"StaticEndpointID", uint64_t{70}},
         {"BridgePoolStartEID", uint64_t{71}},
         {"BridgePoolEndEID", uint64_t{73}},
         {"IgnoreEIDs", std::string("x,72,300")}});

    std::set<mctp_eid_t> eids;
    pldm::fw_update::seedRefreshEidsFromStaticConfig(eids);
    EXPECT_EQ(eids, (std::set<mctp_eid_t>{60, 61, 63, 65, 70, 71, 73}));
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

TEST_F(UpdateManagerInternalTest, seedRefreshCollectsTopologyDeviceEids)
{
    pldm::utils::GetSubTreeResponse subtree;
    publishDevice("/inv/a", "A", 12, {{"Medium", std::string("spi")}}, subtree);
    publishDevice("/inv/b", "B", 13, {{"Medium", std::string("usb")}}, subtree);
    publishDevice("/inv/c", "C", 14, {{"Medium", std::string("i2c")}}, subtree);
    // a device behind a bridge (Medium "bridge") is not seeded
    publishDevice("/inv/d", "D", 15, {{"Medium", std::string("bridge")}},
                  subtree);
    // a device without an EID has nothing to seed
    publishDevice("/inv/e", "E", std::nullopt, {{"Medium", std::string("usb")}},
                  subtree);
    UpdateManagerTestDBusHandler::setSubtreeResponse(
        pldm::em_device::nvDeviceIntf, subtree);

    std::set<mctp_eid_t> eids;
    pldm::fw_update::seedRefreshEidsFromStaticConfig(eids);
    EXPECT_EQ(eids, (std::set<mctp_eid_t>{12, 13, 14}));

    // the pre-update validation scope keeps the devices as well
    eids.clear();
    pldm::fw_update::seedRefreshEidsFromStaticConfig(eids, false);
    EXPECT_EQ(eids, (std::set<mctp_eid_t>{12, 13, 14}));
}

TEST_F(UpdateManagerInternalTest, seedRefreshSkipsTopologyDevicesIgnoringPldm)
{
    // the MCTP target record of a bridge that ignores PLDM; it is matched to
    // the device by EID, whatever the record's interface or the device's
    // medium
    UpdateManagerTestDBusHandler::setSubtreeResponse(
        i2cIntf, {{"/inv/bridge", {{"svc", {i2cIntf}}}}});
    UpdateManagerTestDBusHandler::setProps(
        "/inv/bridge", i2cIntf,
        {{"StaticEndpointID", uint64_t{8}},
         {"IgnoreMessageTypes", std::string("2, 1")}});

    pldm::utils::GetSubTreeResponse subtree;
    // the bridge: its own EID is not seeded
    publishDevice("/inv/e", "E", 8, {{"Medium", std::string("usb")}}, subtree);
    // a device whose EID no target record marks as ignoring PLDM is seeded
    publishDevice("/inv/f", "F", 30, {{"Medium", std::string("spi")}}, subtree);
    UpdateManagerTestDBusHandler::setSubtreeResponse(
        pldm::em_device::nvDeviceIntf, subtree);

    std::set<mctp_eid_t> eids;
    pldm::fw_update::seedRefreshEidsFromStaticConfig(eids);
    EXPECT_EQ(eids, (std::set<mctp_eid_t>{30}));

    // skipped, never removed: the EID already in the set (a live descriptor
    // key) stays
    eids = {8};
    pldm::fw_update::seedRefreshEidsFromStaticConfig(eids);
    EXPECT_EQ(eids, (std::set<mctp_eid_t>{8, 30}));
}
#endif

TEST_F(UpdateManagerInternalTest, seedRefreshKeepsOnlyEidsMctpdEnumeratedForPldm)
{
    constexpr auto endpointIntf = "xyz.openbmc_project.MCTP.Endpoint";
    const auto endpoint = [](unsigned eid) {
        return "/au/com/codeconstruct/mctp1/networks/1/endpoints/" +
               std::to_string(eid);
    };
    pldm::utils::GetSubTreeResponse endpoints;
    for (unsigned eid : {40U, 41U, 43U, 44U})
    {
        endpoints.push_back({endpoint(eid), {{"mctpd", {endpointIntf}}}});
    }
    UpdateManagerTestDBusHandler::setSubtreeResponse(endpointIntf, endpoints);
    UpdateManagerTestDBusHandler::setProps(
        endpoint(40), endpointIntf,
        {{"SupportedMessageTypes", std::vector<uint8_t>{0, 1, 5}}});
    // an NVMe-MI drive: enumerated, but PLDM is not among its types
    UpdateManagerTestDBusHandler::setProps(
        endpoint(41), endpointIntf,
        {{"SupportedMessageTypes", std::vector<uint8_t>{2, 4, 5}}});
    UpdateManagerTestDBusHandler::setProps(
        endpoint(43), endpointIntf,
        {{"SupportedMessageTypes", std::vector<uint8_t>{1}}});
    // 44: the types cannot be read, so the endpoint is taken as capable

    // a bridge with the pool 40..44
    UpdateManagerTestDBusHandler::setSubtreeResponse(
        usbIntf, {{"/inv/usb0", {{"svc", {usbIntf}}}}});
    UpdateManagerTestDBusHandler::setProps(
        "/inv/usb0", usbIntf,
        {{"StaticEndpointID", uint64_t{30}},
         {"BridgePoolStartEID", uint64_t{40}},
         {"BridgePoolEndEID", uint64_t{44}}});

    // 42 is not enumerated and 41 is not PLDM capable; a static EID is kept
    std::set<mctp_eid_t> eids;
    pldm::fw_update::seedRefreshEidsFromStaticConfig(eids);
    EXPECT_EQ(eids, (std::set<mctp_eid_t>{30, 40, 43, 44}));

    // the pre-update validation scope seeds no pool
    eids.clear();
    pldm::fw_update::seedRefreshEidsFromStaticConfig(eids, false);
    EXPECT_EQ(eids, (std::set<mctp_eid_t>{30}));

    // EIDs already in the set (live descriptor keys) stay
    eids = {41};
    pldm::fw_update::seedRefreshEidsFromStaticConfig(eids);
    EXPECT_EQ(eids, (std::set<mctp_eid_t>{30, 40, 41, 43, 44}));

#ifdef OEM_NVIDIA
    // a topology device mctpd has not enumerated is left out of the refresh
    // but not of the validation scope
    pldm::utils::GetSubTreeResponse subtree;
    publishDevice("/inv/a", "A", 51, {{"Medium", std::string("spi")}}, subtree);
    publishDevice("/inv/b", "B", 43, {{"Medium", std::string("usb")}}, subtree);
    UpdateManagerTestDBusHandler::setSubtreeResponse(
        pldm::em_device::nvDeviceIntf, subtree);
    eids.clear();
    pldm::fw_update::seedRefreshEidsFromStaticConfig(eids);
    EXPECT_EQ(eids, (std::set<mctp_eid_t>{30, 40, 43, 44}));
    eids.clear();
    pldm::fw_update::seedRefreshEidsFromStaticConfig(eids, false);
    EXPECT_EQ(eids, (std::set<mctp_eid_t>{30, 43, 51}));
#endif
}

} // namespace
