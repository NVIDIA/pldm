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

// Inventory and Entity Manager lookups of Terminus, run against an
// in-memory mapper/property model instead of a bus. The binary is built with
// MOCK_DBUS_ASYNC_UTILS, so the co_await D-Bus helpers call the
// dbusAsyncMock hooks inline.

#include "common/dBusAsyncUtils.hpp"
#include "common/shared_mem_manager.hpp"
#include "common/types.hpp"
#include "common/utils.hpp"
#include "oem/nvidia/platform-mc/derived_sensor/switchBandwidthSensor.hpp"
#include "oem/nvidia/platform-mc/oem_nvidia.hpp"
#include "platform-mc/terminus.hpp"
#include "platform-mc/terminus_manager.hpp"
#include "test/test_instance_id.hpp"

#include <endian.h>

#include <sdbusplus/exception.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>
#include <sdeventplus/event.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace pldm::utils
{

/** @brief In-memory object mapper and property store.
 *
 *  Interfaces are keyed by object path, owning service and interface name.
 *  As in the mapper, GetSubTree and GetObject list each service that
 *  implements a requested interface (any, if none is requested) with all of
 *  its interfaces, and GetSubTree leaves out the root path. Property reads
 *  and GetAll must name the owning service, as on a real bus. A missing
 *  property reads as a default variant, so coGetDbusProperty returns T{}, as
 *  it does when Get fails.
 */
class FakeInventory
{
  public:
    void addInterface(const std::string& path, const std::string& interface,
                      const PropertyMap& properties,
                      const std::string& service = entityManagerService)
    {
        objects[{path, service, interface}] = properties;
    }

    void clear()
    {
        objects.clear();
    }

    GetSubTreeResponse getSubTree(const std::string& root,
                                  const dbus::Interfaces& interfaces) const
    {
        const std::string prefix = root + "/";
        return collect(
            [&prefix](const std::string& path) {
                return path.starts_with(prefix);
            },
            interfaces);
    }

    MapperServiceMap getObject(const std::string& objPath,
                               const dbus::Interfaces& interfaces) const
    {
        auto response = collect(
            [&objPath](const std::string& path) { return path == objPath; },
            interfaces);
        return response.empty() ? MapperServiceMap{} : response.front().second;
    }

    PropertyValue getProperty(const std::string& service,
                              const std::string& path,
                              const std::string& interface,
                              const std::string& property) const
    {
        auto it = objects.find({path, service, interface});
        if (it == objects.end() || !it->second.contains(property))
        {
            return {};
        }
        return it->second.at(property);
    }

    PropertyMap getAll(const std::string& service, const std::string& path,
                       const std::string& interface) const
    {
        auto it = objects.find({path, service, interface});
        if (it == objects.end())
        {
            throw sdbusplus::exception::SdBusError(EIO, "fake GetAll");
        }
        return it->second;
    }

  private:
    struct Key
    {
        std::string path;
        std::string service;
        std::string interface;

        auto operator<=>(const Key&) const = default;
    };

    template <typename PathFilter>
    GetSubTreeResponse collect(PathFilter wantPath,
                               const dbus::Interfaces& interfaces) const
    {
        std::map<std::string, std::map<std::string, dbus::Interfaces>> owners;
        for (const auto& [key, properties] : objects)
        {
            if (wantPath(key.path))
            {
                owners[key.path][key.service].push_back(key.interface);
            }
        }

        GetSubTreeResponse response;
        for (auto& [path, services] : owners)
        {
            MapperServiceMap serviceMap;
            for (auto& [service, serviceInterfaces] : services)
            {
                if (interfaces.empty() ||
                    std::ranges::find_first_of(serviceInterfaces, interfaces) !=
                        serviceInterfaces.end())
                {
                    serviceMap.emplace_back(service,
                                            std::move(serviceInterfaces));
                }
            }
            if (!serviceMap.empty())
            {
                response.emplace_back(path, std::move(serviceMap));
            }
        }
        return response;
    }

    std::map<Key, PropertyMap> objects;
};

FakeInventory& fakeInventory()
{
    static FakeInventory inventory;
    return inventory;
}

/** @brief Stands in for DBusHandler in the sources included below: GetAll
 *         and GetSubTree are served from fakeInventory() and getBus()
 *         returns a mocked bus.
 */
class InventoryTestDBusHandler : public DBusHandler
{
  public:
    static sdbusplus::bus_t& getBus()
    {
        static testing::NiceMock<sdbusplus::SdBusMock> sdbusMock;
        static auto bus = sdbusplus::get_mocked_new(&sdbusMock);
        return bus;
    }

    PropertyMap getDbusPropertiesVariant(
        const char* serviceName, const char* objPath,
        const char* dbusInterface) const override
    {
        return fakeInventory().getAll(serviceName, objPath, dbusInterface);
    }

    GetSubTreeResponse getSubtree(
        const std::string& path, int /*depth*/,
        const std::vector<std::string>& ifaceList) const override
    {
        return fakeInventory().getSubTree(path, ifaceList);
    }
};

} // namespace pldm::utils

// The meson target leaves these two sources out and builds them here. Their
// headers are included above, so only the .cpp bodies see the substitution.
#define DBusHandler InventoryTestDBusHandler
#include "oem/nvidia/platform-mc/derived_sensor/switchBandwidthSensor.cpp" // NOLINT(bugprone-suspicious-include)
#include "platform-mc/terminus.cpp" // NOLINT(bugprone-suspicious-include)
#undef DBusHandler

namespace
{

using namespace pldm::platform_mc;
using pldm::platform_mc::oem_nvidia::InventoryManager;
namespace dbusAsyncMock = pldm::utils::dbusAsyncMock;
using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr uint8_t localEid = 0x08;

class TerminusInventoryTest : public testing::Test
{
  protected:
    TerminusInventoryTest() :
        event(sdeventplus::Event::get_default()),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 2,
                   milliseconds(100)),
        terminusManager(event, reqHandler, instanceIdDb, termini, localEid,
                        nullptr)
    {}

    void SetUp() override
    {
        dbusAsyncMock::getSubTree = [](const std::string& path, int,
                                       const pldm::dbus::Interfaces& ifaces) {
            return pldm::utils::fakeInventory().getSubTree(path, ifaces);
        };
        dbusAsyncMock::getServiceMap =
            [](const std::string& path, const pldm::dbus::Interfaces& ifaces) {
                return pldm::utils::fakeInventory().getObject(path, ifaces);
            };
        dbusAsyncMock::getProperty =
            [](const std::string& path, const std::string& property,
               const std::string& interface, const std::string& service) {
                return pldm::utils::fakeInventory().getProperty(
                    service, path, interface, property);
            };
    }

    void TearDown() override
    {
        dbusAsyncMock::reset();
        inventory.clear();
    }

    pldm::utils::FakeInventory& inventory = pldm::utils::fakeInventory();
    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    pldm::requester::Handler<pldm::requester::Request> reqHandler;
    std::map<pldm::tid_t, std::shared_ptr<Terminus>> termini;
    TerminusManager terminusManager;
};

TEST_F(TerminusInventoryTest,
       checkDeviceInventoryReturnsFailedWithoutMappedMctpInfo)
{
    std::string uuid("00000000-0000-0000-0000-000000000211");
    Terminus terminus(0x22, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov211";
    inventory.addInterface(
        rootPath + "/usb_assoc",
        "xyz.openbmc_project.Configuration.USBDeviceAssociation",
        {{"EID", uint64_t{0x41}}});

    auto rc = stdexec::sync_wait(terminus.checkDeviceInventory(rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_FAILED, std::get<0>(*rc));
}

TEST_F(TerminusInventoryTest, checkDeviceInventoryUsbAssociationCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000212");
    Terminus terminus(0x23, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const pldm::MctpInfo mctpInfo(0x42, uuid, "smbus", 1, std::nullopt,
                                  "mctp-over-smbus", std::nullopt);
    ASSERT_EQ(terminusManager.mapTid(mctpInfo, terminus.getTid()),
              terminus.getTid());

    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov212";
    inventory.addInterface(
        rootPath + "/usb_assoc",
        "xyz.openbmc_project.Configuration.USBDeviceAssociation",
        {{"EID", uint64_t{0x42}}});
    inventory.addInterface(
        rootPath + "/aux0", "xyz.openbmc_project.Configuration.SensorAuxName",
        {{"SensorId", uint64_t{0x5500}},
         {"AuxNames", std::vector<std::string>{"Port 0", "Port 1"}},
         {"ParentObjPath", rootPath},
         {"EID", uint64_t{0x42}}});

    auto rc = stdexec::sync_wait(terminus.checkDeviceInventory(rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
    // The EID read from the association is the one used to load the
    // EID-filtered SensorAuxName entry.
    EXPECT_TRUE(terminus.sensorAuxNameOverwriteTbl.contains(0x5500));
}

TEST_F(TerminusInventoryTest, checkDeviceInventoryNsmAssociationCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000213");
    Terminus terminus(0x24, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const pldm::MctpInfo mctpInfo(0x43, uuid, "usb", 1, std::nullopt,
                                  "mctp-over-usb", std::nullopt);
    ASSERT_EQ(terminusManager.mapTid(mctpInfo, terminus.getTid()),
              terminus.getTid());

    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov213";
    // nsmd, not Entity Manager, publishes the association that carries the
    // MCTP UUID.
    const std::string nsmService = "xyz.openbmc_project.NSM";
    inventory.addInterface(
        rootPath + "/nsm_assoc",
        "xyz.openbmc_project.Configuration.NsmDeviceAssociation",
        {{"UUID", uuid}}, nsmService);

    auto rc = stdexec::sync_wait(terminus.checkDeviceInventory(rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));

    // The same association with another device's UUID is rejected.
    inventory.addInterface(
        rootPath + "/nsm_assoc",
        "xyz.openbmc_project.Configuration.NsmDeviceAssociation",
        {{"UUID", std::string("00000000-0000-0000-0000-000000000299")}},
        nsmService);

    rc = stdexec::sync_wait(terminus.checkDeviceInventory(rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_FAILED, std::get<0>(*rc));
}

TEST_F(TerminusInventoryTest, checkDeviceInventoryI2cAssociationCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000215");
    Terminus terminus(0x26, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const pldm::MctpInfo mctpInfo(0x46, uuid, "smbus", 1, std::nullopt,
                                  "mctp-over-smbus", std::nullopt);
    ASSERT_EQ(terminusManager.mapTid(mctpInfo, terminus.getTid()),
              terminus.getTid());

    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov215";
    inventory.addInterface(
        rootPath + "/i2c_assoc",
        "xyz.openbmc_project.Configuration.I2CDeviceAssociation",
        {{"Bus", uint64_t{91}},
         {"Address", uint64_t{0x56}},
         {"EID", uint64_t{0x46}}});
    inventory.addInterface(
        rootPath + "/aux_match",
        "xyz.openbmc_project.Configuration.SensorAuxName",
        {{"SensorId", uint64_t{0x5500}},
         {"AuxNames", std::vector<std::string>{"Port 0"}},
         {"ParentObjPath", rootPath},
         {"Bus", uint64_t{91}},
         {"Address", uint64_t{0x56}},
         {"EID", uint64_t{0x46}}});

    auto rc = stdexec::sync_wait(terminus.checkDeviceInventory(rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
    // The entry loads only if Bus, Address and EID all reach
    // getSensorAuxNameFromEM.
    EXPECT_TRUE(terminus.sensorAuxNameOverwriteTbl.contains(0x5500));

    // The same association with another endpoint's EID is rejected.
    inventory.addInterface(
        rootPath + "/i2c_assoc",
        "xyz.openbmc_project.Configuration.I2CDeviceAssociation",
        {{"Bus", uint64_t{91}},
         {"Address", uint64_t{0x56}},
         {"EID", uint64_t{0x47}}});

    rc = stdexec::sync_wait(terminus.checkDeviceInventory(rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_FAILED, std::get<0>(*rc));
}

TEST_F(TerminusInventoryTest,
       checkDeviceInventoryMctpEndpointAssociationCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000217");
    Terminus terminus(0x28, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const pldm::MctpInfo mctpInfo(0x48, uuid, "smbus", 1, std::nullopt,
                                  "mctp-over-smbus", std::nullopt);
    ASSERT_EQ(terminusManager.mapTid(mctpInfo, terminus.getTid()),
              terminus.getTid());

    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov217";
    inventory.addInterface(
        rootPath + "/mctp_assoc",
        "xyz.openbmc_project.Configuration.MctpEndpointAssociation",
        {{"EID", uint64_t{0x48}}});
    inventory.addInterface(rootPath + "/aux_eid_match",
                           "xyz.openbmc_project.Configuration.SensorAuxName",
                           {{"SensorId", uint64_t{0x5700}},
                            {"AuxNames", std::vector<std::string>{"Port 0"}},
                            {"ParentObjPath", rootPath},
                            {"EID", uint64_t{0x48}}});
    inventory.addInterface(rootPath + "/aux_eid_mismatch",
                           "xyz.openbmc_project.Configuration.SensorAuxName",
                           {{"SensorId", uint64_t{0x5701}},
                            {"AuxNames", std::vector<std::string>{"WrongEid"}},
                            {"ParentObjPath", rootPath},
                            {"EID", uint64_t{0x49}}});

    auto rc = stdexec::sync_wait(terminus.checkDeviceInventory(rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
    // The matched EID has to reach getSensorAuxNameFromEM, otherwise every
    // EID filtered SensorAuxName would be dropped.
    EXPECT_TRUE(terminus.sensorAuxNameOverwriteTbl.contains(0x5700));
    EXPECT_FALSE(terminus.sensorAuxNameOverwriteTbl.contains(0x5701));
}

TEST_F(TerminusInventoryTest,
       checkDeviceInventoryReturnsFailedWhenMctpEndpointEidDoesNotMatch)
{
    std::string uuid("00000000-0000-0000-0000-000000000218");
    Terminus terminus(0x29, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const pldm::MctpInfo mctpInfo(0x4a, uuid, "smbus", 1, std::nullopt,
                                  "mctp-over-smbus", std::nullopt);
    ASSERT_EQ(terminusManager.mapTid(mctpInfo, terminus.getTid()),
              terminus.getTid());

    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov218";
    inventory.addInterface(
        rootPath + "/mctp_assoc",
        "xyz.openbmc_project.Configuration.MctpEndpointAssociation",
        {{"EID", uint64_t{0x4b}}});
    inventory.addInterface(
        rootPath + "/aux0", "xyz.openbmc_project.Configuration.SensorAuxName",
        {{"SensorId", uint64_t{0x5702}},
         {"AuxNames", std::vector<std::string>{"OtherEndpoint"}},
         {"ParentObjPath", rootPath}});

    auto rc = stdexec::sync_wait(terminus.checkDeviceInventory(rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_FAILED, std::get<0>(*rc));
    EXPECT_TRUE(terminus.sensorAuxNameOverwriteTbl.empty());
}

TEST_F(TerminusInventoryTest, getSensorAuxNameFromEMI2cFilterCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000215a");
    Terminus terminus(0x26, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov215a";
    const std::string boardPath =
        "/xyz/openbmc_project/inventory/system/board/cov215a";

    inventory.addInterface(
        rootPath + "/aux_match",
        "xyz.openbmc_project.Configuration.SensorAuxName",
        {{"SensorId", uint64_t{0x5600}},
         {"AuxNames", std::vector<std::string>{"Port 0"}},
         {"ParentObjPath", boardPath},
         {"Bus", uint64_t{91}},
         {"Address", uint64_t{0x56}},
         {"EID", uint64_t{0x46}}});
    inventory.addInterface(rootPath + "/aux_bus_mismatch",
                           "xyz.openbmc_project.Configuration.SensorAuxName",
                           {{"SensorId", uint64_t{0x5601}},
                            {"AuxNames", std::vector<std::string>{"WrongBus"}},
                            {"ParentObjPath", rootPath},
                            {"Bus", uint64_t{92}}});
    inventory.addInterface(
        rootPath + "/aux_addr_mismatch",
        "xyz.openbmc_project.Configuration.SensorAuxName",
        {{"SensorId", uint64_t{0x5602}},
         {"AuxNames", std::vector<std::string>{"WrongAddress"}},
         {"ParentObjPath", rootPath},
         {"Address", uint64_t{0x57}}});
    inventory.addInterface(rootPath + "/aux_eid_mismatch",
                           "xyz.openbmc_project.Configuration.SensorAuxName",
                           {{"SensorId", uint64_t{0x5603}},
                            {"AuxNames", std::vector<std::string>{"WrongEid"}},
                            {"ParentObjPath", rootPath},
                            {"EID", uint64_t{0x47}}});
    inventory.addInterface(rootPath + "/aux_no_filter",
                           "xyz.openbmc_project.Configuration.SensorAuxName",
                           {{"SensorId", uint64_t{0x5604}},
                            {"AuxNames", std::vector<std::string>{"NoFilter"}},
                            {"ParentObjPath", rootPath}});
    // Zero Bus/Address/EID values match any device.
    inventory.addInterface(
        rootPath + "/aux_zero_filter",
        "xyz.openbmc_project.Configuration.SensorAuxName",
        {{"SensorId", uint64_t{0x5605}},
         {"AuxNames", std::vector<std::string>{"ZeroFilter"}},
         {"ParentObjPath", rootPath},
         {"Bus", uint64_t{0}},
         {"Address", uint64_t{0}},
         {"EID", uint64_t{0}}});

    auto rc = stdexec::sync_wait(
        terminus.getSensorAuxNameFromEM(91, 0x56, 0x46, rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
    ASSERT_TRUE(terminus.sensorAuxNameOverwriteTbl.contains(0x5600));
    EXPECT_FALSE(terminus.sensorAuxNameOverwriteTbl.contains(0x5601));
    EXPECT_FALSE(terminus.sensorAuxNameOverwriteTbl.contains(0x5602));
    EXPECT_FALSE(terminus.sensorAuxNameOverwriteTbl.contains(0x5603));
    EXPECT_TRUE(terminus.sensorAuxNameOverwriteTbl.contains(0x5604));
    EXPECT_TRUE(terminus.sensorAuxNameOverwriteTbl.contains(0x5605));

    // The matching entry keeps its names and its ParentObjPath.
    const auto& [auxNames, parentPath] =
        terminus.sensorAuxNameOverwriteTbl.at(0x5600);
    EXPECT_EQ((pldm::platform_mc::AuxiliaryNames{{{"en", "Port 0"}}}),
              auxNames);
    EXPECT_EQ(boardPath, parentPath);
}

TEST_F(TerminusInventoryTest,
       checkDeviceInventoryReturnsFailedWhenAssociationDoesNotMatch)
{
    std::string uuid("00000000-0000-0000-0000-000000000214");
    Terminus terminus(0x25, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const pldm::MctpInfo mctpInfo(0x44, uuid, "usb", 1, std::nullopt,
                                  "mctp-over-usb", std::nullopt);
    ASSERT_EQ(terminusManager.mapTid(mctpInfo, terminus.getTid()),
              terminus.getTid());

    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov214";
    inventory.addInterface(
        rootPath + "/usb_assoc",
        "xyz.openbmc_project.Configuration.USBDeviceAssociation",
        {{"EID", uint64_t{0x45}}});
    inventory.addInterface(
        rootPath + "/aux0", "xyz.openbmc_project.Configuration.SensorAuxName",
        {{"SensorId", uint64_t{0x5501}},
         {"AuxNames", std::vector<std::string>{"OtherDevice"}},
         {"ParentObjPath", rootPath}});

    auto rc = stdexec::sync_wait(terminus.checkDeviceInventory(rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_FAILED, std::get<0>(*rc));
    EXPECT_TRUE(terminus.sensorAuxNameOverwriteTbl.empty());
}

TEST_F(TerminusInventoryTest,
       getSensorAuxNameFromEMSkipsEntriesMissingMandatoryPropertiesCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000216");
    Terminus terminus(0x27, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov216";

    inventory.addInterface(
        rootPath + "/missing_sensor_id",
        "xyz.openbmc_project.Configuration.SensorAuxName",
        {{"AuxNames", std::vector<std::string>{"MissingSensorId"}},
         {"ParentObjPath", rootPath}});
    inventory.addInterface(
        rootPath + "/missing_aux_names",
        "xyz.openbmc_project.Configuration.SensorAuxName",
        {{"SensorId", uint64_t{0x5611}}, {"ParentObjPath", rootPath}});
    inventory.addInterface(
        rootPath + "/valid", "xyz.openbmc_project.Configuration.SensorAuxName",
        {{"SensorId", uint64_t{0x5612}},
         {"AuxNames", std::vector<std::string>{"ValidAux"}},
         {"ParentObjPath", rootPath}});

    auto rc =
        stdexec::sync_wait(terminus.getSensorAuxNameFromEM(0, 0, 0, rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
    EXPECT_FALSE(terminus.sensorAuxNameOverwriteTbl.contains(0x5611));
    EXPECT_TRUE(terminus.sensorAuxNameOverwriteTbl.contains(0x5612));
    EXPECT_EQ(1U, terminus.sensorAuxNameOverwriteTbl.size());
}

TEST_F(TerminusInventoryTest,
       getPortInfoFromEMRejectsMalformedAssociationCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000218");
    Terminus terminus(0x29, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov218";
    const std::string portType =
        "xyz.openbmc_project.Inventory.Decorator.PortInfo.PortType."
        "BidirectionalPort";
    const std::string portProtocol =
        "xyz.openbmc_project.Inventory.Decorator.PortInfo.PortProtocol.NVLink";

    inventory.addInterface(
        rootPath + "/port_bad",
        "xyz.openbmc_project.Configuration.SensorPortInfo",
        {{"SensorId", uint64_t{0x5701}},
         {"MaxSpeedMBps", uint64_t{16000}},
         {"PortType", portType},
         {"PortProtocol", portProtocol},
         {"Association", std::vector<std::string>{"only", "two"}}});

    auto rc = stdexec::sync_wait(terminus.getPortInfoFromEM(rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_FAILED, std::get<0>(*rc));
    EXPECT_TRUE(terminus.sensorPortInfoOverwriteTbl.empty());

    // A well-formed (forward, reverse, path) triplet is accepted.
    const std::string goodRootPath = rootPath + "_good";
    inventory.addInterface(
        goodRootPath + "/port",
        "xyz.openbmc_project.Configuration.SensorPortInfo",
        {{"SensorId", uint64_t{0x5702}},
         {"MaxSpeedMBps", uint64_t{16000}},
         {"PortType", portType},
         {"PortProtocol", portProtocol},
         {"Association",
          std::vector<std::string>{"connected_to", "connecting", rootPath}}});

    rc = stdexec::sync_wait(terminus.getPortInfoFromEM(goodRootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
    ASSERT_TRUE(terminus.sensorPortInfoOverwriteTbl.contains(0x5702));
    const auto& [type, protocol, maxSpeed, associations] =
        terminus.sensorPortInfoOverwriteTbl.at(0x5702);
    EXPECT_EQ(pldm::platform_mc::PortType::BidirectionalPort, type);
    EXPECT_EQ(portProtocol, protocol);
    EXPECT_EQ(16000U, maxSpeed);
    ASSERT_EQ(1U, associations.size());
    EXPECT_EQ("connected_to", associations[0].forward);
    EXPECT_EQ("connecting", associations[0].reverse);
    EXPECT_EQ(rootPath, associations[0].path);
}

TEST_F(TerminusInventoryTest, loadDeviceRecordAddsPortsToSensorPortInfo)
{
    std::string uuid("00000000-0000-0000-0000-00000000021D");
    Terminus terminus(0x2E, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov21D";
    const std::string portProtocol =
        "xyz.openbmc_project.Inventory.Decorator.PortInfo.PortProtocol.NVLink";
    const std::string switchPath =
        "/xyz/openbmc_project/inventory/system/fabrics/Switch_0";
    inventory.addInterface(
        rootPath + "/port0", "xyz.openbmc_project.Configuration.SensorPortInfo",
        {{"SensorId", uint64_t{0x5801}},
         {"MaxSpeedMBps", uint64_t{16000}},
         {"PortType", std::string("xyz.openbmc_project.Inventory.Decorator."
                                  "PortInfo.PortType.DownstreamPort")},
         {"PortProtocol", portProtocol},
         {"Association",
          std::vector<std::string>{"connected_to", "connecting", switchPath}}});

    pldm::em_device::PldmDevice record;
    record.ports = {{0x5801, "Port_0"}, {0x5802, "Port_1"}};
    record.networkAdapter = "NIC_0";
    const std::string portsPath = rootPath + "/NetworkAdapters/NIC_0/Ports/";

    auto checkSensorPortInfo = [&](uint64_t recordSpeed, uint64_t infoSpeed) {
        const auto& [type, protocol, maxSpeed, associations] =
            terminus.sensorPortInfoOverwriteTbl.at(0x5801);
        EXPECT_EQ(pldm::platform_mc::PortType::DownstreamPort, type);
        EXPECT_EQ(portProtocol, protocol);
        EXPECT_EQ(infoSpeed, maxSpeed);
        ASSERT_EQ(2U, associations.size());
        EXPECT_EQ("connected_to", associations[0].forward);
        EXPECT_EQ("connecting", associations[0].reverse);
        EXPECT_EQ(switchPath, associations[0].path);
        EXPECT_EQ("associated_port", associations[1].forward);
        EXPECT_EQ("associated_port", associations[1].reverse);
        EXPECT_EQ(portsPath + "Port_0", associations[1].path);

        // A port of the record alone gets the record's defaults.
        const auto& [newType, newProtocol, newSpeed, newAssociations] =
            terminus.sensorPortInfoOverwriteTbl.at(0x5802);
        EXPECT_EQ(pldm::platform_mc::PortType::BidirectionalPort, newType);
        EXPECT_EQ("", newProtocol);
        EXPECT_EQ(recordSpeed, newSpeed);
        ASSERT_EQ(1U, newAssociations.size());
        EXPECT_EQ(portsPath + "Port_1", newAssociations[0].path);
        EXPECT_EQ(2U, terminus.sensorPortInfoOverwriteTbl.size());
    };

    // Without a speed of its own, the record keeps the SensorPortInfo one.
    auto rc = stdexec::sync_wait(terminus.getPortInfoFromEM(rootPath));
    ASSERT_TRUE(rc.has_value());
    ASSERT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
    terminus.loadDeviceRecord(record, rootPath);
    checkSensorPortInfo(0, 16000);

    // With one, its speed applies. getPortInfoFromEM() clears the table, so
    // the record starts again from the SensorPortInfo entry alone.
    record.portMaxSpeed = 400000;
    rc = stdexec::sync_wait(terminus.getPortInfoFromEM(rootPath));
    ASSERT_TRUE(rc.has_value());
    ASSERT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
    terminus.loadDeviceRecord(record, rootPath);
    checkSensorPortInfo(400000, 400000);
}

TEST_F(TerminusInventoryTest, loadDeviceRecordPortsWithoutANetworkAdapter)
{
    std::string uuid("00000000-0000-0000-0000-00000000021E");
    Terminus terminus(0x2F, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov21E";

    pldm::em_device::PldmDevice record;
    record.ports = {{0x5901, "Port_0"}};
    terminus.loadDeviceRecord(record, rootPath);

    // The ports of a device without a NIC are the device's own.
    ASSERT_TRUE(terminus.sensorPortInfoOverwriteTbl.contains(0x5901));
    const auto& associations =
        std::get<3>(terminus.sensorPortInfoOverwriteTbl.at(0x5901));
    ASSERT_EQ(1U, associations.size());
    EXPECT_EQ(rootPath + "/Ports/Port_0", associations[0].path);
}

constexpr auto platformPath = "/xyz/openbmc_project/inventory/system/platform/";
constexpr auto sensorAuxNameIntf =
    "xyz.openbmc_project.Configuration.SensorAuxName";

/** @brief The chassis object of the device @p name. */
std::string chassisOf(const std::string& name)
{
    return pldm::em_device::chassisRoot + name;
}

/** @brief Publish the NvDevice @p name, with @p eid when given. */
void addNvDevice(pldm::utils::FakeInventory& inventory, const std::string& name,
                 std::optional<uint64_t> eid)
{
    pldm::utils::PropertyMap props{{"Name", name},
                                   {"Kind", std::string("gpu")}};
    if (eid)
    {
        props.emplace("EID", *eid);
    }
    inventory.addInterface(platformPath + name, pldm::em_device::nvDeviceIntf,
                           props);
}

/** @brief Publish the PLDMDevice record of the device @p name: sensor 100 is
 *         named "Temp_0" and sensor 101 is port "Port_0" of the NIC
 *         "NIC_0". */
void addPldmRecord(pldm::utils::FakeInventory& inventory,
                   const std::string& name)
{
    const std::string path = platformPath + name + "_PLDM";
    const std::string intf = pldm::em_device::pldmDeviceIntf;
    inventory.addInterface(
        path, intf,
        {{"Name", name + "_PLDM"}, {"NetworkAdapter", std::string("NIC_0")}});
    inventory.addInterface(path, intf + ".SensorAuxNames",
                           {{"Temp_0", uint64_t{100}}});
    inventory.addInterface(path, intf + ".Ports",
                           {{"101", std::string("Port_0")}});
}

TEST_F(TerminusInventoryTest, checkDeviceInventoryLoadsTheDeviceRecord)
{
    std::string uuid("00000000-0000-0000-0000-000000000230");
    Terminus terminus(0x30, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const pldm::MctpInfo mctpInfo(0x50, uuid, "usb", 1, std::string("Dev_230"),
                                  "mctp-over-usb", std::nullopt);
    ASSERT_EQ(terminusManager.mapTid(mctpInfo, terminus.getTid()),
              terminus.getTid());

    const std::string chassisPath = chassisOf("Dev_230");
    addNvDevice(inventory, "Dev_230", 0x50);
    addPldmRecord(inventory, "Dev_230");
    // SensorAuxName configs of the chassis are read first with the device's
    // EID: the record replaces the name of sensor 100, sensor 102 keeps its
    // own and the config of another EID is left out.
    inventory.addInterface(chassisPath + "/aux_100", sensorAuxNameIntf,
                           {{"SensorId", uint64_t{100}},
                            {"AuxNames", std::vector<std::string>{"Old"}},
                            {"ParentObjPath", chassisPath}});
    inventory.addInterface(chassisPath + "/aux_102", sensorAuxNameIntf,
                           {{"SensorId", uint64_t{102}},
                            {"AuxNames", std::vector<std::string>{"Kept"}},
                            {"ParentObjPath", chassisPath},
                            {"EID", uint64_t{0x50}}});
    inventory.addInterface(chassisPath + "/aux_103", sensorAuxNameIntf,
                           {{"SensorId", uint64_t{103}},
                            {"AuxNames", std::vector<std::string>{"OtherEid"}},
                            {"ParentObjPath", chassisPath},
                            {"EID", uint64_t{0x51}}});

    auto rc = stdexec::sync_wait(terminus.checkDeviceInventory(chassisPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));

    const auto& auxNames = terminus.sensorAuxNameOverwriteTbl;
    EXPECT_EQ(2U, auxNames.size());
    ASSERT_TRUE(auxNames.contains(100));
    EXPECT_EQ((pldm::platform_mc::AuxiliaryNames{{{"en", "Temp_0"}}}),
              std::get<0>(auxNames.at(100)));
    EXPECT_EQ(chassisPath, std::get<1>(auxNames.at(100)));
    ASSERT_TRUE(auxNames.contains(102));
    EXPECT_EQ((pldm::platform_mc::AuxiliaryNames{{{"en", "Kept"}}}),
              std::get<0>(auxNames.at(102)));

    const auto& ports = terminus.sensorPortInfoOverwriteTbl;
    ASSERT_EQ(1U, ports.size());
    ASSERT_TRUE(ports.contains(101));
    const auto& [type, protocol, maxSpeed, associations] = ports.at(101);
    EXPECT_EQ(pldm::platform_mc::PortType::BidirectionalPort, type);
    ASSERT_EQ(1U, associations.size());
    EXPECT_EQ("associated_port", associations[0].forward);
    EXPECT_EQ("associated_port", associations[0].reverse);
    EXPECT_EQ(chassisPath + "/NetworkAdapters/NIC_0/Ports/Port_0",
              associations[0].path);
}

TEST_F(TerminusInventoryTest, checkDeviceInventoryMatchesTheSanitisedLeaf)
{
    // EM writes "Dev-23.2" into the chassis path as "Dev_23_2"; the record
    // is still found by the raw name.
    std::string uuid("00000000-0000-0000-0000-000000000232");
    Terminus terminus(0x32, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const pldm::MctpInfo mctpInfo(0x53, uuid, "usb", 1, std::string("Dev-23.2"),
                                  "mctp-over-usb", std::nullopt);
    ASSERT_EQ(terminusManager.mapTid(mctpInfo, terminus.getTid()),
              terminus.getTid());

    const std::string chassisPath = chassisOf("Dev_23_2");
    addNvDevice(inventory, "Dev-23.2", 0x53);
    addPldmRecord(inventory, "Dev-23.2");

    auto rc = stdexec::sync_wait(terminus.checkDeviceInventory(chassisPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
    ASSERT_TRUE(terminus.sensorAuxNameOverwriteTbl.contains(100));
    EXPECT_EQ((pldm::platform_mc::AuxiliaryNames{{{"en", "Temp_0"}}}),
              std::get<0>(terminus.sensorAuxNameOverwriteTbl.at(100)));

    // the raw name is not a path EM publishes
    terminus.sensorAuxNameOverwriteTbl.clear();
    rc = stdexec::sync_wait(
        terminus.checkDeviceInventory(chassisOf("Dev-23.2")));
    ASSERT_TRUE(rc.has_value());
    EXPECT_TRUE(terminus.sensorAuxNameOverwriteTbl.empty());
}

TEST_F(TerminusInventoryTest, checkDeviceInventoryRejectsAnotherDevicesChassis)
{
    std::string uuid("00000000-0000-0000-0000-000000000231");
    Terminus terminus(0x31, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);

    // The chassis has no association child, which the association matcher
    // alone would accept for any terminus.
    const std::string chassisPath = chassisOf("Dev_231");
    addNvDevice(inventory, "Dev_231", 0x52);
    addPldmRecord(inventory, "Dev_231");

    // A terminus without an MCTP endpoint is no device's.
    auto rc = stdexec::sync_wait(terminus.checkDeviceInventory(chassisPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_FAILED, std::get<0>(*rc));
    EXPECT_TRUE(terminus.sensorAuxNameOverwriteTbl.empty());
    EXPECT_TRUE(terminus.sensorPortInfoOverwriteTbl.empty());

    const pldm::MctpInfo mctpInfo(0x52, uuid, "usb", 1, std::nullopt,
                                  "mctp-over-usb", std::nullopt);
    ASSERT_EQ(terminusManager.mapTid(mctpInfo, terminus.getTid()),
              terminus.getTid());
    addNvDevice(inventory, "Dev_231", 0x53);

    rc = stdexec::sync_wait(terminus.checkDeviceInventory(chassisPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_FAILED, std::get<0>(*rc));
    EXPECT_TRUE(terminus.sensorAuxNameOverwriteTbl.empty());
    EXPECT_TRUE(terminus.sensorPortInfoOverwriteTbl.empty());

    // An association child naming this terminus does not win over the
    // device's EID either.
    inventory.addInterface(
        chassisPath + "/usb_assoc",
        "xyz.openbmc_project.Configuration.USBDeviceAssociation",
        {{"EID", uint64_t{0x52}}});
    rc = stdexec::sync_wait(terminus.checkDeviceInventory(chassisPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_FAILED, std::get<0>(*rc));

    // A device without an EID does not decide: the association child does,
    // and the record is not loaded.
    addNvDevice(inventory, "Dev_231", std::nullopt);
    rc = stdexec::sync_wait(terminus.checkDeviceInventory(chassisPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
    EXPECT_FALSE(terminus.sensorAuxNameOverwriteTbl.contains(100));
    EXPECT_TRUE(terminus.sensorPortInfoOverwriteTbl.empty());
}

TEST_F(TerminusInventoryTest, checkDeviceInventoryMatchesTheEndpointName)
{
    std::string uuid("00000000-0000-0000-0000-000000000232");
    Terminus terminus(0x32, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const std::string chassisPath = chassisOf("Dev_232");
    addNvDevice(inventory, "Dev_232", 0x54);
    addPldmRecord(inventory, "Dev_232");

    // The endpoint is the device's when it has the device's EID or the
    // device's name. Its name is that of its MCTP target record, which may
    // differ from the device's (BF4_0). A device on two media has an
    // endpoint per medium under its name; the terminus moved to the one
    // with another EID (0x55) still gets the chassis. Only an endpoint with
    // neither is refused, and the association child with its EID does not
    // overrule the device.
    inventory.addInterface(
        chassisPath + "/usb_assoc",
        "xyz.openbmc_project.Configuration.USBDeviceAssociation",
        {{"EID", uint64_t{0x55}}});
    // SensorAuxName configs of the chassis are picked by the device's EID,
    // also for the endpoint of another medium.
    inventory.addInterface(chassisPath + "/aux_102", sensorAuxNameIntf,
                           {{"SensorId", uint64_t{102}},
                            {"AuxNames", std::vector<std::string>{"Dev"}},
                            {"EID", uint64_t{0x54}}});
    inventory.addInterface(chassisPath + "/aux_103", sensorAuxNameIntf,
                           {{"SensorId", uint64_t{103}},
                            {"AuxNames", std::vector<std::string>{"Other"}},
                            {"EID", uint64_t{0x55}}});
    struct Case
    {
        std::optional<std::string> name;
        uint8_t eid;
        int rc;
    };
    const std::vector<Case> cases{
        {std::nullopt, 0x54, PLDM_SUCCESS},
        {"", 0x54, PLDM_SUCCESS},
        {"Dev_232", 0x54, PLDM_SUCCESS},
        {"BF4_0", 0x54, PLDM_SUCCESS},
        {"Dev_232", 0x55, PLDM_SUCCESS},
        {"BF4_0", 0x55, PLDM_FAILED},
        {"", 0x55, PLDM_FAILED},
        {std::nullopt, 0x55, PLDM_FAILED}};
    for (const auto& [name, eid, expected] : cases)
    {
        SCOPED_TRACE(
            name.value_or("<no name>") + " EID " + std::to_string(eid));
        terminusManager.unmapTid(terminus.getTid());
        const pldm::MctpInfo mctpInfo(eid, uuid, "usb", 1, name,
                                      "mctp-over-usb", std::nullopt);
        ASSERT_EQ(terminusManager.mapTid(mctpInfo, terminus.getTid()),
                  terminus.getTid());
        terminus.sensorAuxNameOverwriteTbl.clear();
        terminus.sensorPortInfoOverwriteTbl.clear();

        auto rc =
            stdexec::sync_wait(terminus.checkDeviceInventory(chassisPath));
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(expected, std::get<0>(*rc));
        EXPECT_EQ(expected == PLDM_SUCCESS,
                  terminus.sensorAuxNameOverwriteTbl.contains(100));
        EXPECT_EQ(expected == PLDM_SUCCESS,
                  terminus.sensorAuxNameOverwriteTbl.contains(102));
        EXPECT_FALSE(terminus.sensorAuxNameOverwriteTbl.contains(103));
        EXPECT_EQ(expected == PLDM_SUCCESS,
                  terminus.sensorPortInfoOverwriteTbl.contains(101));
    }
}

TEST_F(TerminusInventoryTest, checkDeviceInventoryDeviceWithoutARecord)
{
    std::string uuid("00000000-0000-0000-0000-000000000234");
    Terminus terminus(0x34, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const pldm::MctpInfo mctpInfo(0x58, uuid, "usb", 1, std::string("Dev_234"),
                                  "mctp-over-usb", std::nullopt);
    ASSERT_EQ(terminusManager.mapTid(mctpInfo, terminus.getTid()),
              terminus.getTid());
    const std::string chassisPath = chassisOf("Dev_234");
    inventory.addInterface(chassisPath + "/aux_100", sensorAuxNameIntf,
                           {{"SensorId", uint64_t{100}},
                            {"AuxNames", std::vector<std::string>{"Aux"}}});

    // The device of this endpoint has no PLDMDevice record: the chassis is
    // left to its association children, and without one it is accepted
    // with no EM config loaded.
    addNvDevice(inventory, "Dev_234", 0x58);
    auto rc = stdexec::sync_wait(terminus.checkDeviceInventory(chassisPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
    EXPECT_TRUE(terminus.sensorAuxNameOverwriteTbl.empty());
    EXPECT_TRUE(terminus.sensorPortInfoOverwriteTbl.empty());

    // The association children decide: one of another EID refuses it, one
    // of this EID accepts it and loads the SensorAuxName configs.
    const std::string usbAssociation =
        "xyz.openbmc_project.Configuration.USBDeviceAssociation";
    inventory.addInterface(chassisPath + "/usb_assoc", usbAssociation,
                           {{"EID", uint64_t{0x59}}});
    rc = stdexec::sync_wait(terminus.checkDeviceInventory(chassisPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_FAILED, std::get<0>(*rc));
    EXPECT_TRUE(terminus.sensorAuxNameOverwriteTbl.empty());
    inventory.addInterface(chassisPath + "/usb_assoc", usbAssociation,
                           {{"EID", uint64_t{0x58}}});
    rc = stdexec::sync_wait(terminus.checkDeviceInventory(chassisPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
    EXPECT_TRUE(terminus.sensorAuxNameOverwriteTbl.contains(100));

    // The chassis of another device is still refused before the record is
    // looked for, whatever its association children say.
    addNvDevice(inventory, "Dev_234", 0x59);
    terminusManager.unmapTid(terminus.getTid());
    const pldm::MctpInfo otherEndpoint(0x58, uuid, "usb", 1,
                                       std::string("Dev_999"), "mctp-over-usb",
                                       std::nullopt);
    ASSERT_EQ(terminusManager.mapTid(otherEndpoint, terminus.getTid()),
              terminus.getTid());
    terminus.sensorAuxNameOverwriteTbl.clear();
    rc = stdexec::sync_wait(terminus.checkDeviceInventory(chassisPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_FAILED, std::get<0>(*rc));
    EXPECT_TRUE(terminus.sensorAuxNameOverwriteTbl.empty());
}

TEST_F(TerminusInventoryTest, checkDeviceInventoryDeviceWithoutAValidEid)
{
    std::string uuid("00000000-0000-0000-0000-000000000235");
    Terminus terminus(0x35, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const pldm::MctpInfo mctpInfo(0x5A, uuid, "usb", 1, std::string("Dev_235"),
                                  "mctp-over-usb", std::nullopt);
    ASSERT_EQ(terminusManager.mapTid(mctpInfo, terminus.getTid()),
              terminus.getTid());
    const std::string chassisPath = chassisOf("Dev_235");
    addPldmRecord(inventory, "Dev_235");

    // A device without an EID, or with an invalid one, is matched by
    // neither EID nor name, though its name is the endpoint's: its record
    // is not loaded, and the chassis is left to the association children,
    // here none.
    const std::vector<std::optional<pldm::utils::PropertyValue>> eids{
        std::nullopt, uint64_t{0xFF}, std::string("x")};
    for (const auto& eid : eids)
    {
        SCOPED_TRACE(
            eid ? "EID of variant index " + std::to_string(eid->index())
                : std::string("no EID"));
        pldm::utils::PropertyMap props{{"Name", std::string("Dev_235")},
                                       {"Kind", std::string("gpu")}};
        if (eid)
        {
            props.emplace("EID", *eid);
        }
        inventory.addInterface(platformPath + std::string("Dev_235"),
                               pldm::em_device::nvDeviceIntf, props);
        terminus.sensorAuxNameOverwriteTbl.clear();
        terminus.sensorPortInfoOverwriteTbl.clear();

        auto rc =
            stdexec::sync_wait(terminus.checkDeviceInventory(chassisPath));
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
        EXPECT_TRUE(terminus.sensorAuxNameOverwriteTbl.empty());
        EXPECT_TRUE(terminus.sensorPortInfoOverwriteTbl.empty());
    }

    // An association child of another EID refuses it.
    inventory.addInterface(
        chassisPath + "/usb_assoc",
        "xyz.openbmc_project.Configuration.USBDeviceAssociation",
        {{"EID", uint64_t{0x5B}}});
    auto rc = stdexec::sync_wait(terminus.checkDeviceInventory(chassisPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_FAILED, std::get<0>(*rc));
}

/** @brief Publish @p roles as the SensorRoles of the PLDMDevice record of
 *         the device @p name. */
void addSensorRoles(pldm::utils::FakeInventory& inventory,
                    const std::string& name,
                    const pldm::utils::PropertyMap& roles)
{
    inventory.addInterface(
        platformPath + name + "_PLDM",
        std::string(pldm::em_device::pldmDeviceIntf) + ".SensorRoles", roles);
}

/** Terminus 0x3A, the endpoint (EID 0x62, name Dev_23A) of the device
 *  Dev_23A, whose PLDMDevice record may carry SensorRoles. */
class DeviceRecordSensorRolesTest : public TerminusInventoryTest
{
  protected:
    using Roles = std::map<std::string, std::string>;

    void SetUp() override
    {
        TerminusInventoryTest::SetUp();
        const pldm::MctpInfo mctpInfo(0x62, uuid, "usb", 1,
                                      std::string("Dev_23A"), "mctp-over-usb",
                                      std::nullopt);
        ASSERT_EQ(terminusManager.mapTid(mctpInfo, terminus.getTid()),
                  terminus.getTid());
    }

    /** Publish Dev_23A and its record, with @p roles as its SensorRoles
     *  when given. */
    void publishDevice(std::optional<pldm::utils::PropertyMap> roles)
    {
        inventory.clear();
        addNvDevice(inventory, "Dev_23A", 0x62);
        addPldmRecord(inventory, "Dev_23A");
        if (roles)
        {
            addSensorRoles(inventory, "Dev_23A", *roles);
        }
    }

    /** checkDeviceInventory() of @p path; PLDM_ERROR if it did not
     *  complete. */
    int check(const std::string& path)
    {
        auto rc = stdexec::sync_wait(terminus.checkDeviceInventory(path));
        return rc ? std::get<0>(*rc) : PLDM_ERROR;
    }

    /** Match Dev_23A with a record whose SensorRoles are @p roles, read as
     *  @p expected. Call through ASSERT_NO_FATAL_FAILURE. */
    void matchWithRoles(const pldm::utils::PropertyMap& roles,
                        const Roles& expected)
    {
        publishDevice(roles);
        ASSERT_EQ(PLDM_SUCCESS, check(chassisPath));
        ASSERT_EQ("Dev_23A", terminus.deviceName);
        ASSERT_EQ(expected, terminus.sensorRoles);
    }

    std::string uuid{"00000000-0000-0000-0000-00000000023A"};
    Terminus terminus{0x3A, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager};
    const std::string chassisPath = chassisOf("Dev_23A");
    const pldm::utils::PropertyMap tempRole{
        {"primary_temperature_sensor", std::string("TempLimit_0")}};
    const Roles tempRoles{
        {pldm::em_device::primaryTemperatureSensor, "TempLimit_0"}};
};

TEST_F(DeviceRecordSensorRolesTest, takesTheRolesOfTheRecord)
{
    publishDevice(tempRole);
    EXPECT_EQ(PLDM_SUCCESS, check(chassisPath));
    EXPECT_EQ("Dev_23A", terminus.deviceName);
    EXPECT_EQ(tempRoles, terminus.sensorRoles);
}

TEST_F(DeviceRecordSensorRolesTest, rescanReplacesThem)
{
    ASSERT_NO_FATAL_FAILURE(matchWithRoles(tempRole, tempRoles));

    publishDevice(pldm::utils::PropertyMap{
        {"primary_power_control", std::string("TDPPowerNonvolatile_0")}});
    EXPECT_EQ(PLDM_SUCCESS, check(chassisPath));
    EXPECT_EQ((Roles{{pldm::em_device::primaryPowerControl,
                      "TDPPowerNonvolatile_0"}}),
              terminus.sensorRoles);
}

TEST_F(DeviceRecordSensorRolesTest, recordWithoutSensorRolesLeavesNone)
{
    // That nvidiaUpdateAssociations() then takes the build options is
    // TerminusTest.nvidiaUpdateAssociationsPrefersSensorRolesToBuildOptions.
    ASSERT_NO_FATAL_FAILURE(matchWithRoles(tempRole, tempRoles));

    publishDevice(std::nullopt);
    EXPECT_EQ(PLDM_SUCCESS, check(chassisPath));
    EXPECT_EQ("Dev_23A", terminus.deviceName);
    EXPECT_TRUE(terminus.sensorRoles.empty());
}

TEST_F(DeviceRecordSensorRolesTest, deviceWithoutARecordLeavesNone)
{
    ASSERT_NO_FATAL_FAILURE(matchWithRoles(tempRole, tempRoles));

    // Accepted with no association child, and no record to read.
    inventory.clear();
    addNvDevice(inventory, "Dev_23A", 0x62);
    EXPECT_EQ(PLDM_SUCCESS, check(chassisPath));
    EXPECT_EQ("Dev_23A", terminus.deviceName);
    EXPECT_TRUE(terminus.sensorRoles.empty());
}

TEST_F(DeviceRecordSensorRolesTest, anotherDeviceOfTheEndpointClearsThem)
{
    ASSERT_NO_FATAL_FAILURE(matchWithRoles(tempRole, tempRoles));

    // Dev_23B has the endpoint's EID and no record.
    addNvDevice(inventory, "Dev_23B", 0x62);
    EXPECT_EQ(PLDM_SUCCESS, check(chassisOf("Dev_23B")));
    EXPECT_EQ("Dev_23B", terminus.deviceName);
    EXPECT_TRUE(terminus.sensorRoles.empty());
}

TEST_F(DeviceRecordSensorRolesTest, deviceNoLongerMatchedClearsThem)
{
    ASSERT_NO_FATAL_FAILURE(matchWithRoles(tempRole, tempRoles));

    // Neither the EID nor the name of the endpoint is the device's now.
    addNvDevice(inventory, "Dev_23A", 0x63);
    terminusManager.unmapTid(terminus.getTid());
    const pldm::MctpInfo otherEndpoint(0x62, uuid, "usb", 1,
                                       std::string("Dev_999"), "mctp-over-usb",
                                       std::nullopt);
    ASSERT_EQ(terminusManager.mapTid(otherEndpoint, terminus.getTid()),
              terminus.getTid());
    EXPECT_EQ(PLDM_FAILED, check(chassisPath));
    EXPECT_TRUE(terminus.deviceName.empty());
    EXPECT_TRUE(terminus.sensorRoles.empty());
}

TEST_F(DeviceRecordSensorRolesTest, pathsOfNoDeviceOrAnotherKeepThem)
{
    ASSERT_NO_FATAL_FAILURE(matchWithRoles(tempRole, tempRoles));

    // A scan offers every inventory path: the chassis of another device,
    // refused for its EID, a chassis of no device and a path outside the
    // chassis, both accepted without association children. None of them
    // concerns the device of the terminus.
    addNvDevice(inventory, "Dev_23C", 0x70);
    addPldmRecord(inventory, "Dev_23C");
    addSensorRoles(inventory, "Dev_23C",
                   {{"primary_cpupower_sensor", std::string("CpuPower_0")}});
    const std::vector<std::pair<std::string, int>> paths{
        {chassisOf("Dev_23C"), PLDM_FAILED},
        {chassisOf("NoDevice_0"), PLDM_SUCCESS},
        {"/xyz/openbmc_project/inventory/system/processors/CPU_0",
         PLDM_SUCCESS}};
    for (const auto& [path, expected] : paths)
    {
        SCOPED_TRACE(path);
        EXPECT_EQ(expected, check(path));
        EXPECT_EQ("Dev_23A", terminus.deviceName);
        EXPECT_EQ(tempRoles, terminus.sensorRoles);
    }
}

/** @brief An InterfacesAdded signal of @p intf with @p props at @p path,
 *         ready to be read. */
sdbusplus::message_t interfacesAdded(
    sdbusplus::bus_t& bus, const std::string& path, const std::string& intf,
    const pldm::dbus::PropertyMap& props)
{
    auto msg = bus.new_signal(
        path.c_str(), "org.freedesktop.DBus.ObjectManager", "InterfacesAdded");
    msg.append(sdbusplus::object_path(path),
               pldm::dbus::InterfaceMap{{intf, props}});
    EXPECT_GE(sd_bus_message_seal(msg.get(), 0, 0), 0);
    EXPECT_GE(sd_bus_message_rewind(msg.get(), true), 0);
    return msg;
}

TEST_F(TerminusInventoryTest, interfaceAddedOfItsRecordsConfiguresTheTerminus)
{
    std::string uuid("00000000-0000-0000-0000-000000000233");
    const pldm::tid_t tid = 0x33;
    auto terminus = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid] = terminus;
    const pldm::MctpInfo mctpInfo(0x56, uuid, "usb", 1, std::string("Dev_233"),
                                  "mctp-over-usb", std::nullopt);
    ASSERT_EQ(terminusManager.mapTid(mctpInfo, tid), tid);
    auto bus = sdbusplus::bus::new_default();

    // The terminus is being initialized when the records of its device
    // arrive. A record of another device does not concern it.
    const std::string nvDevice = platformPath + std::string("Dev_233");
    inventory.addInterface(nvDevice, pldm::em_device::nvDeviceIntf,
                           {{"Name", std::string("Dev_233")},
                            {"Kind", std::string("cpu")},
                            {"EID", uint64_t{0x56}},
                            {"Instance", uint64_t{3}},
                            {"ParentChassis", std::string("CPUModule_3")}});
    auto msg = interfacesAdded(
        bus, platformPath + std::string("Dev_999"),
        pldm::em_device::nvDeviceIntf,
        {{"Name", std::string("Dev_999")}, {"EID", uint64_t{0x57}}});
    terminus->interfaceAdded(msg);
    EXPECT_FALSE(terminus->getTerminusName().has_value());
    EXPECT_FALSE(terminus->needRefresh);

    // Its NvDevice: the config it yields applies.
    msg = interfacesAdded(
        bus, nvDevice, pldm::em_device::nvDeviceIntf,
        {{"Name", std::string("Dev_233")}, {"EID", uint64_t{0x56}}});
    terminus->interfaceAdded(msg);
    EXPECT_EQ("CPUModule_3", terminus->getTerminusName().value_or(""));
    EXPECT_EQ(std::optional<uint16_t>(3), terminus->getInstance());
    EXPECT_EQ(std::optional<uint16_t>(0), terminus->getCpuIndex());
    EXPECT_TRUE(terminus->needRefresh);

    // Its PLDM device record, named after the device: the record's config
    // replaces it.
    const std::string record = nvDevice + "_PLDM";
    inventory.addInterface(record, pldm::em_device::pldmDeviceIntf,
                           {{"Name", std::string("Dev_233_PLDM")},
                            {"MCTPTargetName", std::string("Dev_233")},
                            {"TerminusName", std::string("Late_0")},
                            {"TerminusInstance", uint64_t{2}},
                            {"CpuIndex", uint64_t{1}}});
    msg = interfacesAdded(bus, record, pldm::em_device::pldmDeviceIntf,
                          {{"Name", std::string("Dev_233_PLDM")}});
    terminus->interfaceAdded(msg);
    EXPECT_EQ("Late_0", terminus->getTerminusName().value_or(""));
    EXPECT_EQ(std::optional<uint16_t>(2), terminus->getInstance());
    EXPECT_EQ(std::optional<uint16_t>(1), terminus->getCpuIndex());
}

TEST_F(TerminusInventoryTest, interfaceAddedMatchesARecordOfTheScannedDevice)
{
    std::string uuid("00000000-0000-0000-0000-000000000236");
    const pldm::tid_t tid = 0x36;
    auto terminus = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid] = terminus;
    // The endpoint is named after its MCTP target record, not the device.
    pldm::MctpInfo mctpInfo(0x5C, uuid, "usb", 1, std::string("BF4_0"),
                            "mctp-over-usb", std::nullopt);
    ASSERT_EQ(terminusManager.mapTid(mctpInfo, tid), tid);
    auto bus = sdbusplus::bus::new_default();
    addNvDevice(inventory, "BlueField_0", 0x5C);
    addNvDevice(inventory, "BlueField_1", 0x5D);
    addNvDevice(inventory, "BlueField_2", std::nullopt);

    auto interestedIn = [&](const pldm::dbus::PropertyMap& props) {
        terminus->needRefresh = false;
        auto msg = interfacesAdded(bus, platformPath + std::string("Record"),
                                   pldm::em_device::pldmDeviceIntf, props);
        terminus->interfaceAdded(msg);
        return terminus->needRefresh;
    };
    auto scan = [&](const std::string& device) {
        auto rc = stdexec::sync_wait(
            terminus->checkDeviceInventory(chassisOf(device)));
        return rc ? std::get<0>(*rc) : -1;
    };
    const pldm::dbus::PropertyMap byName{
        {"Name", std::string("BlueField_0_PLDM")}};
    const pldm::dbus::PropertyMap byTarget{
        {"Name", std::string("Record_PLDM")},
        {"MCTPTargetName", std::string("BlueField_0")}};
    const pldm::dbus::PropertyMap otherDevice{
        {"Name", std::string("BlueField_1_PLDM")}};
    const pldm::dbus::PropertyMap noEidDevice{
        {"Name", std::string("BlueField_2_PLDM")}};

    // Before the chassis scan, the device of the endpoint is not known:
    // its record names neither the endpoint nor a known device.
    EXPECT_FALSE(interestedIn(byName));
    EXPECT_FALSE(interestedIn(byTarget));

    // A refused chassis, and one accepted through the association configs
    // for lack of a valid EID, make no device known.
    ASSERT_EQ(PLDM_FAILED, scan("BlueField_1"));
    EXPECT_FALSE(interestedIn(otherDevice));
    ASSERT_EQ(PLDM_SUCCESS, scan("BlueField_2"));
    EXPECT_FALSE(interestedIn(noEidDevice));
    EXPECT_FALSE(interestedIn(byName));

    // The scan accepts the chassis of BlueField_0 by EID: its record is
    // matched by Name or MCTPTargetName from then on, also after another
    // chassis is refused. A Name without the suffix, or with an empty
    // device name before it, names no device.
    ASSERT_EQ(PLDM_SUCCESS, scan("BlueField_0"));
    ASSERT_EQ(PLDM_FAILED, scan("BlueField_1"));
    EXPECT_TRUE(interestedIn(byName));
    EXPECT_TRUE(interestedIn(byTarget));
    EXPECT_FALSE(interestedIn(otherDevice));
    EXPECT_FALSE(interestedIn({{"Name", std::string("BlueField_0")}}));
    EXPECT_FALSE(interestedIn({{"Name", std::string("_PLDM")}}));

    // So for an endpoint without a name.
    terminusManager.unmapTid(tid);
    std::get<4>(mctpInfo) = std::nullopt;
    ASSERT_EQ(terminusManager.mapTid(mctpInfo, tid), tid);
    EXPECT_TRUE(interestedIn(byName));
    EXPECT_TRUE(interestedIn(byTarget));
    EXPECT_FALSE(interestedIn(otherDevice));

    // A rescan that no longer matches the device forgets it: one that
    // finds no valid EID, and one that refuses the chassis.
    addNvDevice(inventory, "BlueField_0", std::nullopt);
    ASSERT_EQ(PLDM_SUCCESS, scan("BlueField_0"));
    EXPECT_FALSE(interestedIn(byName));
    addNvDevice(inventory, "BlueField_0", 0x5C);
    ASSERT_EQ(PLDM_SUCCESS, scan("BlueField_0"));
    ASSERT_TRUE(interestedIn(byName));
    addNvDevice(inventory, "BlueField_0", 0x5E);
    ASSERT_EQ(PLDM_FAILED, scan("BlueField_0"));
    EXPECT_FALSE(interestedIn(byName));
    EXPECT_FALSE(interestedIn(byTarget));
}

TEST_F(TerminusInventoryTest,
       getInfoForNVSwitchFromEMEarlyReturnAndMalformedAssociationCoverage)
{
    std::string uuid("00000000-0000-0000-0000-000000000219");
    Terminus terminus(0x2A, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov219";
    const std::string switchInterface =
        "xyz.openbmc_project.Configuration.NSM_NVSwitch.Switch";
    const std::string nvlink =
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.NVLink";
    const std::string validRootPath = rootPath + "_valid";
    inventory.addInterface(
        validRootPath + "/switch", switchInterface,
        {{"Name", std::string("switch219_valid")},
         {"SwitchType", nvlink},
         {"SwitchSupportedProtocols", std::vector<std::string>{nvlink}},
         {"Association",
          std::vector<std::string>{"chassis", "all_switches", rootPath}}});

    std::string switchType =
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet";
    std::vector<std::string> switchProtocols{};
    std::vector<pldm::dbus::PathAssociation> associations{};
    terminus.switchBandwidthSensor =
        std::make_shared<oem_nvidia::SwitchBandwidthSensor>(
            static_cast<pldm::tid_t>(0x2A), "preexisting", switchType,
            switchProtocols, associations);
    auto existingSensor = terminus.switchBandwidthSensor;

    // An existing sensor is kept even though a valid configuration exists.
    auto earlyRc =
        stdexec::sync_wait(terminus.getInfoForNVSwitchFromEM(validRootPath));
    ASSERT_TRUE(earlyRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*earlyRc));
    EXPECT_EQ(existingSensor, terminus.switchBandwidthSensor);

    // Without one, the valid configuration creates it.
    terminus.switchBandwidthSensor.reset();
    auto validRc =
        stdexec::sync_wait(terminus.getInfoForNVSwitchFromEM(validRootPath));
    ASSERT_TRUE(validRc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*validRc));
    ASSERT_NE(nullptr, terminus.switchBandwidthSensor);
    EXPECT_EQ("switch219_valid",
              terminus.switchBandwidthSensor->getSensorName());

    terminus.switchBandwidthSensor.reset();
    inventory.addInterface(
        rootPath + "/switch_bad", switchInterface,
        {{"Name", std::string("switch219")},
         {"SwitchType",
          std::string(
              "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet")},
         {"SwitchSupportedProtocols", std::vector<std::string>{"PCIe"}},
         {"Association", std::vector<std::string>{"bad", "assoc"}}});

    auto malformedRc =
        stdexec::sync_wait(terminus.getInfoForNVSwitchFromEM(rootPath));
    ASSERT_TRUE(malformedRc.has_value());
    EXPECT_EQ(PLDM_FAILED, std::get<0>(*malformedRc));
    EXPECT_EQ(nullptr, terminus.switchBandwidthSensor);
}

TEST_F(TerminusInventoryTest, getInventoryParentStoresFirstEndpointCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000021B");
    Terminus terminus(0x2C, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov21B";

    // Owned by neither the mapper nor Entity Manager, so the endpoints are
    // found only through the owner that the mapper reports.
    inventory.addInterface(
        rootPath + "/parent_chassis", "xyz.openbmc_project.Association",
        {{"endpoints",
          std::vector<std::string>{
              "/xyz/openbmc_project/inventory/system/chassis/parent0",
              "/xyz/openbmc_project/inventory/system/chassis/parent1"}}},
        "xyz.openbmc_project.Inventory.Manager");

    InventoryManager::ParentMap parents;
    auto rc = stdexec::sync_wait(
        InventoryManager::getInventoryParent(rootPath, parents));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
    ASSERT_TRUE(parents.contains(rootPath));
    EXPECT_EQ("/xyz/openbmc_project/inventory/system/chassis/parent0",
              parents.at(rootPath));
}

TEST_F(TerminusInventoryTest, getInventoryParentIgnoresEmptyEndpointsCoverage)
{
    std::string uuid("00000000-0000-0000-0000-00000000021C");
    Terminus terminus(0x2D, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid,
                      terminusManager);
    const std::string rootPath =
        "/xyz/openbmc_project/inventory/system/chassis/cov21C";

    inventory.addInterface(rootPath + "/parent_chassis",
                           "xyz.openbmc_project.Association",
                           {{"endpoints", std::vector<std::string>{}}},
                           "xyz.openbmc_project.Inventory.Manager");

    InventoryManager::ParentMap parents;
    auto rc = stdexec::sync_wait(
        InventoryManager::getInventoryParent(rootPath, parents));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
    EXPECT_FALSE(parents.contains(rootPath));
}

constexpr auto chassis0Path =
    "/xyz/openbmc_project/inventory/system/chassis/Chassis_0";

/** Entity Association PDR: the Network Controller (100, 1) contains Ethernet
 *  port 1 of container 1000, as a CX9 reports it. */
std::vector<uint8_t> networkControllerPdr()
{
    std::vector<uint8_t> pdr(
        sizeof(pldm_pdr_hdr) + sizeof(pldm_pdr_entity_association), 0);
    auto* hdr = reinterpret_cast<pldm_pdr_hdr*>(pdr.data());
    hdr->type = PLDM_PDR_ENTITY_ASSOCIATION;
    auto* body = reinterpret_cast<pldm_pdr_entity_association*>(
        pdr.data() + sizeof(pldm_pdr_hdr));
    body->container_id = htole16(1000);
    body->association_type = PLDM_ENTITY_ASSOCIAION_PHYSICAL;
    body->container.entity_type = htole16(PLDM_ENTITY_NETWORK_CONTROLLER);
    body->container.entity_instance_num = htole16(1);
    body->container.entity_container_id = htole16(100);
    body->num_children = 1;
    body->children[0].entity_type = htole16(PLDM_ENTITY_ETHERNET);
    body->children[0].entity_instance_num = htole16(1);
    body->children[0].entity_container_id = htole16(1000);
    return pdr;
}

/** Termini bound through the shared InventoryManager, which the fixture
 *  resets. addTerminus() creates termini that share the system inventory:
 *  each defines container 1000 and has a state sensor on container 1011,
 *  which no PDR defines, so the sensor associates with the system
 *  inventory. */
class SharedSystemPathTest : public TerminusInventoryTest
{
  protected:
    SharedSystemPathTest()
    {
        InventoryManager::get().reset();
    }

    ~SharedSystemPathTest() override
    {
        termini.clear();
        InventoryManager::get().reset();
    }

    std::shared_ptr<Terminus> addTerminus(pldm::tid_t tid)
    {
        std::string uuid("00000000-0000-0000-0000-000000000300");
        auto terminus = std::make_shared<Terminus>(
            tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
        termini[tid] = terminus;
        terminus->parseEntityAssociationPDR(networkControllerPdr());
        // Created on the system inventory, as addStateSensor() does.
        std::string associationPath = systemInventoryPath();
        StateSetInfo info{
            EntityInfo{1011, PLDM_ENTITY_ETHERNET, 1},
            {StateSetData{PLDM_STATESET_ID_HEALTHSTATE,
                          PossibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL}}}};
        terminus->stateSensors.emplace_back(std::make_shared<StateSensor>(
            tid, false, 0x200, info, nullptr, associationPath, nullptr));
        terminus->initialized = true;
        return terminus;
    }

    static std::string& systemInventoryPath()
    {
        return InventoryManager::get().systemInventoryPath;
    }

    /** The inventory the terminus's state sensor associates with. */
    static std::string endpoint(const Terminus& terminus)
    {
        const auto& stateSet = terminus.stateSensors.front()->stateSets.front();
        const auto assocs =
            stateSet->associationDefinitionsIntf->associations();
        return assocs.size() == 1 ? std::get<2>(assocs.front()) : std::string{};
    }

    /** Chassis_0 becomes the overall system: a chassis with Item.System. */
    void publishChassis0()
    {
        inventory.addInterface(chassis0Path, oem_nvidia::overallSystemInterface,
                               {});
        inventory.addInterface(chassis0Path, oem_nvidia::chassisInterface, {});
    }

    static int scan(Terminus& terminus)
    {
        auto rc = stdexec::sync_wait(terminus.scanInventories());
        return rc ? std::get<0>(*rc) : PLDM_ERROR;
    }

    static int update(Terminus& terminus)
    {
        auto rc = stdexec::sync_wait(terminus.updateAssociations());
        return rc ? std::get<0>(*rc) : PLDM_ERROR;
    }

    /** A terminus @p tid with SYS_BUS state sensor 0x220 in container
     *  @p busContainer, created on the system inventory as
     *  addStateSensor() does. */
    std::shared_ptr<Terminus> addSysBusTerminus(pldm::tid_t tid,
                                                uint16_t busContainer)
    {
        std::string uuid("00000000-0000-0000-0000-000000000303");
        auto terminus = std::make_shared<Terminus>(
            tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
        termini[tid] = terminus;
        StateSetInfo info{
            EntityInfo{busContainer, PLDM_ENTITY_SYS_BUS, 1},
            {StateSetData{PLDM_STATESET_ID_HEALTHSTATE,
                          PossibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL}}}};
        std::string associationPath = systemInventoryPath();
        terminus->stateSensors.emplace_back(std::make_shared<StateSensor>(
            tid, false, 0x220, info, nullptr, associationPath, nullptr));
        return terminus;
    }

    static constexpr auto modulePath =
        "/xyz/openbmc_project/inventory/system/chassis/ProcessorModule_0";
    static constexpr auto moduleCpuPath =
        "/xyz/openbmc_project/inventory/system/processors/CPU_0";
};

TEST_F(SharedSystemPathTest, systemPathFoundByAnotherTerminusRebindsSensors)
{
    auto a = addTerminus(0x31);
    auto b = addTerminus(0x32);

    // A binds before any scan has found the system, as the first terminus
    // to initialise can.
    EXPECT_EQ(PLDM_SUCCESS, update(*a));
    EXPECT_EQ(PLATFORM_CHASSIS_PATH, endpoint(*a));

    // B's scan then finds Chassis_0.
    publishChassis0();
    size_t scans = 0;
    dbusAsyncMock::getSubTree = [&scans](const std::string& path, int,
                                         const pldm::dbus::Interfaces& ifaces) {
        if (ifaces == InventoryManager::interestingInterfaces())
        {
            ++scans;
        }
        return pldm::utils::fakeInventory().getSubTree(path, ifaces);
    };
    EXPECT_EQ(PLDM_SUCCESS, scan(*b));
    EXPECT_EQ(chassis0Path, systemInventoryPath());

    // A follows, with no scan or PDR parsing of its own.
    EXPECT_EQ(chassis0Path, endpoint(*a));
    EXPECT_EQ(1U, scans);
}

TEST_F(SharedSystemPathTest, systemPathChangeDefersBusyTermini)
{
    auto initialising = addTerminus(0x33);
    initialising->initialized = false;
    auto refreshing = addTerminus(0x37);
    refreshing->refreshAssociationsTaskHandle.emplace();
    auto scanning = addTerminus(0x34);

    // Neither is re-bound now: each re-runs its refresh once done.
    publishChassis0();
    EXPECT_EQ(PLDM_SUCCESS, scan(*scanning));
    for (const auto& busy : {initialising, refreshing})
    {
        EXPECT_TRUE(busy->needRefresh);
        EXPECT_EQ(PLATFORM_CHASSIS_PATH, endpoint(*busy));
    }

    // Finding the same system again changes nothing for the others.
    initialising->needRefresh = false;
    EXPECT_EQ(PLDM_SUCCESS, scan(*scanning));
    EXPECT_FALSE(initialising->needRefresh);
}

TEST_F(SharedSystemPathTest, failedScanKeepsTheSystemPath)
{
    auto terminus = addTerminus(0x35);
    publishChassis0();
    // The InstanceNumber of a CPU listed after Chassis_0 cannot be read.
    const std::string cpuPath =
        "/xyz/openbmc_project/inventory/system/processors/CPU_0";
    inventory.addInterface(cpuPath, "xyz.openbmc_project.Inventory.Item.Cpu",
                           {});
    inventory.addInterface(cpuPath, oem_nvidia::instanceInterface,
                           {{oem_nvidia::instanceProperty, uint64_t{0}}});
    dbusAsyncMock::getProperty =
        [](const std::string& path, const std::string& property,
           const std::string& interface,
           const std::string& service) -> pldm::utils::PropertyValue {
        if (property == oem_nvidia::instanceProperty)
        {
            throw std::runtime_error("fake Get");
        }
        return pldm::utils::fakeInventory().getProperty(service, path,
                                                        interface, property);
    };

    // The scan throws after it has found Chassis_0, and keeps it.
    EXPECT_EQ(PLDM_FAILED, scan(*terminus));
    EXPECT_EQ(chassis0Path, systemInventoryPath());

    // A scan that throws before it finds anything keeps it too.
    dbusAsyncMock::getSubTree =
        [](const std::string&, int,
           const pldm::dbus::Interfaces&) -> pldm::utils::GetSubTreeResponse {
        throw std::runtime_error("fake GetSubTree");
    };
    EXPECT_EQ(PLDM_FAILED, scan(*terminus));
    EXPECT_EQ(chassis0Path, systemInventoryPath());
}

TEST_F(SharedSystemPathTest, scanWithoutASystemKeepsTheSystemPath)
{
    auto terminus = addTerminus(0x36);
    publishChassis0();
    EXPECT_EQ(PLDM_SUCCESS, scan(*terminus));
    EXPECT_EQ(chassis0Path, systemInventoryPath());

    // An empty subtree, as a failed GetSubTree returns, does not fall back
    // to the default.
    inventory.clear();
    EXPECT_EQ(PLDM_SUCCESS, scan(*terminus));
    EXPECT_EQ(chassis0Path, systemInventoryPath());
}

TEST_F(SharedSystemPathTest, stateSensorKeepsItsInventoryOverride)
{
    constexpr pldm::tid_t tid = 0x38;
    std::string uuid("00000000-0000-0000-0000-000000000301");
    auto terminus = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid] = terminus;

    // CPU 1 defines container 10 and has inventory of its own.
    const std::string cpuPath =
        "/xyz/openbmc_project/inventory/system/processors/CPU_1";
    auto& manager = InventoryManager::get();
    manager.entityTree(tid).add(
        10, oem_nvidia::EntityId{EntityInfo{0, PLDM_ENTITY_PROC, 1}});
    // Typed as scan() records a Cpu object.
    manager.terminusData[tid].inventories.emplace_back(
        cpuPath, PLDM_ENTITY_LOGICAL | PLDM_ENTITY_PROC, 1);

    // Two MemoryController state sensors in it; Entity Manager gives the
    // first an inventory of its own.
    const std::string overridePath =
        "/xyz/openbmc_project/inventory/system/memory/DIMM_0";
    terminus->sensorAuxNameOverwriteTbl[0x210] = std::make_tuple(
        pldm::platform_mc::AuxiliaryNames{{{"en", "DIMM_0"}}}, overridePath);
    for (const uint16_t sensorId : {0x210, 0x211})
    {
        StateSetInfo info{
            EntityInfo{10, PLDM_ENTITY_MEMORY_CONTROLLER, 1},
            {StateSetData{PLDM_STATESET_ID_HEALTHSTATE,
                          PossibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL}}}};
        std::string associationPath = systemInventoryPath();
        terminus->stateSensors.emplace_back(std::make_shared<StateSensor>(
            tid, false, sensorId, info, nullptr, associationPath, nullptr));
    }

    EXPECT_EQ(PLDM_SUCCESS, update(*terminus));

    auto endpointOf = [&terminus](size_t index) {
        const auto& stateSet =
            terminus->stateSensors.at(index)->stateSets.front();
        const auto assocs =
            stateSet->associationDefinitionsIntf->associations();
        return assocs.size() == 1 ? std::get<2>(assocs.front()) : std::string{};
    };
    // The override wins over the CPU redirection; without one, the sensor
    // moves to the CPU.
    EXPECT_EQ(overridePath, endpointOf(0));
    EXPECT_EQ(cpuPath, endpointOf(1));
}

TEST_F(SharedSystemPathTest, sysBusStateSensorMovesToTheModulesCpu)
{
    constexpr pldm::tid_t tid = 0x39;
    std::string uuid("00000000-0000-0000-0000-000000000302");
    auto terminus = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid] = terminus;

    // ProcessorModule 0 defines container 10: CPU 0 and a SYS_BUS (a CLink
    // port) side by side. The module and the CPU have inventory; the bus has
    // none, so it resolves to the module.
    const std::string modulePath =
        "/xyz/openbmc_project/inventory/system/chassis/ProcessorModule_0";
    const std::string cpuPath =
        "/xyz/openbmc_project/inventory/system/processors/CPU_0";
    auto& manager = InventoryManager::get();
    manager.entityTree(tid).add(
        10, oem_nvidia::EntityId{EntityInfo{0, PLDM_ENTITY_PROC_IO_MODULE, 0}},
        {oem_nvidia::EntityId{EntityInfo{10, PLDM_ENTITY_PROC, 0}},
         oem_nvidia::EntityId{EntityInfo{10, PLDM_ENTITY_SYS_BUS, 1}}});
    auto& inventories = manager.terminusData[tid].inventories;
    inventories.emplace_back(modulePath, PLDM_ENTITY_PROC_IO_MODULE, 0);
    inventories.emplace_back(cpuPath, PLDM_ENTITY_LOGICAL | PLDM_ENTITY_PROC,
                             0);
    ASSERT_EQ(std::vector<std::string>{modulePath},
              manager.findInventory(tid, oem_nvidia::EntityId{EntityInfo{
                                             10, PLDM_ENTITY_SYS_BUS, 1}}));

    StateSetInfo info{
        EntityInfo{10, PLDM_ENTITY_SYS_BUS, 1},
        {StateSetData{PLDM_STATESET_ID_HEALTHSTATE,
                      PossibleStates{PLDM_STATESET_HEALTH_STATE_NORMAL}}}};
    std::string associationPath = systemInventoryPath();
    terminus->stateSensors.emplace_back(std::make_shared<StateSensor>(
        tid, false, 0x220, info, nullptr, associationPath, nullptr));

    EXPECT_EQ(PLDM_SUCCESS, update(*terminus));

    // The sensor moves from the module to the CPU, a dedicated home for a
    // SYS_BUS, so its metric-report updates are not gated.
    EXPECT_EQ(cpuPath, endpoint(*terminus));
    EXPECT_FALSE(
        terminus->stateSensors.front()->isDefaultInventoryAssociated());
}

TEST_F(SharedSystemPathTest, sysBusStateSensorWithoutACpuStaysOnTheModule)
{
    // ProcessorModule 0 defines container 10 with a SYS_BUS and no CPU.
    constexpr pldm::tid_t tid = 0x3B;
    auto terminus = addSysBusTerminus(tid, 10);
    auto& manager = InventoryManager::get();
    manager.entityTree(tid).add(
        10, oem_nvidia::EntityId{EntityInfo{0, PLDM_ENTITY_PROC_IO_MODULE, 0}},
        {oem_nvidia::EntityId{EntityInfo{10, PLDM_ENTITY_SYS_BUS, 1}}});
    manager.terminusData[tid].inventories.emplace_back(
        modulePath, PLDM_ENTITY_PROC_IO_MODULE, 0);

    EXPECT_EQ(PLDM_SUCCESS, update(*terminus));

    // No CPU to move to: the sensor stays on the module, a fallback for a
    // SYS_BUS, and its metric-report updates stay gated.
    EXPECT_EQ(modulePath, endpoint(*terminus));
    EXPECT_TRUE(terminus->stateSensors.front()->isDefaultInventoryAssociated());
}

TEST_F(SharedSystemPathTest, sysBusStateSensorKeepsItsInventoryOverride)
{
    // As in sysBusStateSensorMovesToTheModulesCpu, with an inventory from
    // Entity Manager for the sensor.
    constexpr pldm::tid_t tid = 0x3C;
    auto terminus = addSysBusTerminus(tid, 10);
    auto& manager = InventoryManager::get();
    manager.entityTree(tid).add(
        10, oem_nvidia::EntityId{EntityInfo{0, PLDM_ENTITY_PROC_IO_MODULE, 0}},
        {oem_nvidia::EntityId{EntityInfo{10, PLDM_ENTITY_PROC, 0}},
         oem_nvidia::EntityId{EntityInfo{10, PLDM_ENTITY_SYS_BUS, 1}}});
    auto& inventories = manager.terminusData[tid].inventories;
    inventories.emplace_back(modulePath, PLDM_ENTITY_PROC_IO_MODULE, 0);
    inventories.emplace_back(moduleCpuPath,
                             PLDM_ENTITY_LOGICAL | PLDM_ENTITY_PROC, 0);
    const std::string overridePath =
        "/xyz/openbmc_project/inventory/system/chassis/ProcessorModule_0/"
        "Ports/CLink_0";
    terminus->sensorAuxNameOverwriteTbl[0x220] = std::make_tuple(
        pldm::platform_mc::AuxiliaryNames{{{"en", "CLink_0"}}}, overridePath);

    EXPECT_EQ(PLDM_SUCCESS, update(*terminus));

    // The override is not redirected to the CPU.
    EXPECT_EQ(overridePath, endpoint(*terminus));
}

TEST_F(SharedSystemPathTest, scanAddsTheProcessorsLinkedToACpu)
{
    // CPU_0 is found by the scan. Its Associations link CPU_1 (and itself)
    // as all_processors, and a chassis by another reverse name. CPU_1 is
    // taken with CPU_0's type and instance, and with its own parent; CPU_0
    // is not taken twice and the chassis not at all.
    constexpr pldm::tid_t tid = 0x3E;
    std::string uuid("00000000-0000-0000-0000-000000000304");
    auto terminus = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid] = terminus;

    const std::string cpu0Path =
        "/xyz/openbmc_project/inventory/system/processors/CPU_0";
    const std::string cpu1Path =
        "/xyz/openbmc_project/inventory/system/processors/CPU_1";
    const std::string otherChassis =
        "/xyz/openbmc_project/inventory/system/chassis/Chassis_9";
    constexpr auto assocDefIntf = "xyz.openbmc_project.Association.Definitions";
    using Associations =
        std::vector<std::tuple<std::string, std::string, std::string>>;
    inventory.addInterface(cpu0Path, "xyz.openbmc_project.Inventory.Item.Cpu",
                           {});
    inventory.addInterface(cpu0Path, oem_nvidia::instanceInterface,
                           {{oem_nvidia::instanceProperty, uint64_t{2}}});
    inventory.addInterface(
        cpu0Path, assocDefIntf,
        {{"Associations", Associations{{"", "all_processors", cpu1Path},
                                       {"", "all_processors", cpu0Path},
                                       {"", "all_chassis", otherChassis}}}});
    inventory.addInterface(
        cpu1Path + "/parent_chassis", "xyz.openbmc_project.Association",
        {{"endpoints", std::vector<std::string>{modulePath}}});
    size_t associationReads = 0;
    dbusAsyncMock::getProperty =
        [&associationReads](
            const std::string& path, const std::string& property,
            const std::string& interface, const std::string& service) {
            if (property == "Associations")
            {
                ++associationReads;
            }
            return pldm::utils::fakeInventory().getProperty(
                service, path, interface, property);
        };

    ASSERT_EQ(PLDM_SUCCESS, scan(*terminus));
    // CPU_0's Associations are read once; CPU_1 is not scanned for links.
    EXPECT_EQ(1U, associationReads);

    const auto& data = InventoryManager::get().terminusData[tid];
    const uint16_t cpuType = PLDM_ENTITY_LOGICAL | PLDM_ENTITY_PROC;
    const std::vector<InventoryManager::Inventory> expected{
        {cpu0Path, cpuType, 2}, {cpu1Path, cpuType, 2}};
    EXPECT_EQ(expected, data.inventories);
    ASSERT_TRUE(data.inventoryParents.contains(cpu1Path));
    EXPECT_EQ(modulePath, data.inventoryParents.at(cpu1Path));
    EXPECT_FALSE(data.inventoryParents.contains(cpu0Path));
}

TEST_F(SharedSystemPathTest, scanKeepsACpuWhoseAssociationsCannotBeRead)
{
    // A failed read of a CPU's Associations loses its linked processors
    // only: the CPU itself is still taken.
    constexpr pldm::tid_t tid = 0x3F;
    std::string uuid("00000000-0000-0000-0000-000000000305");
    auto terminus = std::make_shared<Terminus>(
        tid, 1 << PLDM_BASE | 1 << PLDM_PLATFORM, uuid, terminusManager);
    termini[tid] = terminus;

    const std::string cpuPath =
        "/xyz/openbmc_project/inventory/system/processors/CPU_0";
    inventory.addInterface(cpuPath, "xyz.openbmc_project.Inventory.Item.Cpu",
                           {});
    inventory.addInterface(cpuPath,
                           "xyz.openbmc_project.Association.Definitions", {});
    size_t associationReads = 0;
    dbusAsyncMock::getProperty =
        [&associationReads](
            const std::string& path, const std::string& property,
            const std::string& interface,
            const std::string& service) -> pldm::utils::PropertyValue {
        if (property == "Associations")
        {
            ++associationReads;
            throw std::runtime_error("fake Get");
        }
        return pldm::utils::fakeInventory().getProperty(service, path,
                                                        interface, property);
    };

    ASSERT_EQ(PLDM_SUCCESS, scan(*terminus));
    EXPECT_EQ(1U, associationReads);

    const auto& inventories =
        InventoryManager::get().terminusData[tid].inventories;
    ASSERT_EQ(1U, inventories.size());
    EXPECT_EQ(cpuPath, std::get<0>(inventories.front()));
}

TEST_F(SharedSystemPathTest, sysBusStateSensorInACpuGoesToThatCpu)
{
    // ProcessorModules 0 and 1 define containers 10 and 11, each with a CPU.
    // The CPU of module 1 defines container 21, which holds the SYS_BUS. The
    // sensor goes to that CPU (CPU_1), not to the first CPU of the terminus
    // (CPU_0).
    constexpr pldm::tid_t tid = 0x3D;
    auto terminus = addSysBusTerminus(tid, 21);
    auto& tree = InventoryManager::get().entityTree(tid);
    tree.add(10,
             oem_nvidia::EntityId{EntityInfo{0, PLDM_ENTITY_PROC_IO_MODULE, 0}},
             {oem_nvidia::EntityId{EntityInfo{10, PLDM_ENTITY_PROC, 0}}});
    tree.add(11,
             oem_nvidia::EntityId{EntityInfo{0, PLDM_ENTITY_PROC_IO_MODULE, 1}},
             {oem_nvidia::EntityId{EntityInfo{11, PLDM_ENTITY_PROC, 0}}});
    tree.add(21, oem_nvidia::EntityId{EntityInfo{11, PLDM_ENTITY_PROC, 0}},
             {oem_nvidia::EntityId{EntityInfo{21, PLDM_ENTITY_SYS_BUS, 1}}});
    const std::string module1Path =
        "/xyz/openbmc_project/inventory/system/chassis/ProcessorModule_1";
    const std::string cpu1Path =
        "/xyz/openbmc_project/inventory/system/processors/CPU_1";
    auto& inventories = InventoryManager::get().terminusData[tid].inventories;
    inventories.emplace_back(modulePath, PLDM_ENTITY_PROC_IO_MODULE, 0);
    inventories.emplace_back(module1Path, PLDM_ENTITY_PROC_IO_MODULE, 1);
    inventories.emplace_back(moduleCpuPath,
                             PLDM_ENTITY_LOGICAL | PLDM_ENTITY_PROC, 0);
    inventories.emplace_back(cpu1Path, PLDM_ENTITY_LOGICAL | PLDM_ENTITY_PROC,
                             1);
    ASSERT_EQ(std::vector<std::string>{moduleCpuPath},
              InventoryManager::get().moduleProcessorInventory(tid));

    EXPECT_EQ(PLDM_SUCCESS, update(*terminus));

    EXPECT_EQ(cpu1Path, endpoint(*terminus));
    EXPECT_FALSE(
        terminus->stateSensors.front()->isDefaultInventoryAssociated());
}

} // namespace
