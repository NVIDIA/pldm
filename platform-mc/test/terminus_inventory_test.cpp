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

#include <sdbusplus/exception.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>
#include <sdeventplus/event.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <map>
#include <memory>
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
 *         is served from fakeInventory() and getBus() returns a mocked bus.
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

    auto rc = stdexec::sync_wait(terminus.getInventoryParent(rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
    ASSERT_TRUE(terminus.inventoryParentMap.contains(rootPath));
    EXPECT_EQ("/xyz/openbmc_project/inventory/system/chassis/parent0",
              terminus.inventoryParentMap.at(rootPath));
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

    auto rc = stdexec::sync_wait(terminus.getInventoryParent(rootPath));
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(PLDM_SUCCESS, std::get<0>(*rc));
    EXPECT_FALSE(terminus.inventoryParentMap.contains(rootPath));
}

} // namespace
