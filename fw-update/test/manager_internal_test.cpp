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

// Clang analyzer reports a false-positive leak on Manager's bound callbacks in
// this test translation unit.
// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)

#include "common/instance_id.hpp"
#include "common/types.hpp"
#include "common/utils.hpp"
#ifdef DBusHandler
#undef DBusHandler
#endif
namespace pldm::utils
{

class ManagerTestDBusHandler : public DBusLoggingTestHandler
{
  public:
    static auto& getBus()
    {
        if (auto* bus = testBus())
        {
            return *bus;
        }
        return pldm::utils::DBusHandler::getBus();
    }

    static void reset()
    {
        testBus() = nullptr;
        subtreeResponse().clear();
        throwGetSubtree() = false;
    }

    static void setSubtreeResponse(const GetSubTreeResponse& response)
    {
        subtreeResponse() = response;
    }

    static void setThrowGetSubtree(bool value)
    {
        throwGetSubtree() = value;
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

  private:
    static sdbusplus::bus_t*& testBus()
    {
        static sdbusplus::bus_t* bus = nullptr;
        return bus;
    }

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
};

} // namespace pldm::utils
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define DBusHandler ManagerTestDBusHandler
#define private public
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "fw-update/manager.hpp"
#undef private
#undef DBusHandler
#include "../../test/test_valgrind_utils.hpp"
#include "requester/handler.hpp"
#include "test/test_instance_id.hpp"

#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <xyz/openbmc_project/Software/ApplyTime/server.hpp>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <thread>

#include <gtest/gtest.h>

namespace manager_internal_test_alloc
{

thread_local bool failAllocations = false;
thread_local std::size_t failAtAllocation = 0;
thread_local std::size_t allocationCount = 0;

bool shouldFailAllocation()
{
    return failAllocations && failAtAllocation != 0 &&
           ++allocationCount == failAtAllocation;
}

void* allocate(std::size_t size,
               std::size_t alignment = alignof(std::max_align_t))
{
    if (shouldFailAllocation())
    {
        throw std::bad_alloc();
    }

    if (size == 0)
    {
        size = 1;
    }

    void* ptr = nullptr;
    if (alignment <= alignof(std::max_align_t))
    {
        ptr = std::malloc(size);
    }
    else if (posix_memalign(&ptr, alignment, size) != 0)
    {
        ptr = nullptr;
    }

    if (ptr == nullptr)
    {
        throw std::bad_alloc();
    }

    return ptr;
}

struct ScopedAllocationFailure
{
    explicit ScopedAllocationFailure(std::size_t failIndex) :
        previousFailAllocations(failAllocations),
        previousFailAtAllocation(failAtAllocation),
        previousAllocationCount(allocationCount)
    {
        failAllocations = true;
        failAtAllocation = failIndex;
        allocationCount = 0;
    }

    ~ScopedAllocationFailure()
    {
        failAllocations = previousFailAllocations;
        failAtAllocation = previousFailAtAllocation;
        allocationCount = previousAllocationCount;
    }

  private:
    bool previousFailAllocations;
    std::size_t previousFailAtAllocation;
    std::size_t previousAllocationCount;
};

template <typename Operation>
bool exerciseAllBadAlloc(Operation&& operation, std::size_t maxFailAt = 256)
{
    bool sawBadAlloc = false;

    for (std::size_t failIndex = 1; failIndex <= maxFailAt; ++failIndex)
    {
        try
        {
            ScopedAllocationFailure failure(failIndex);
            operation();
        }
        catch (const std::bad_alloc&)
        {
            sawBadAlloc = true;
        }
        catch (...)
        {}
    }

    return sawBadAlloc;
}

} // namespace manager_internal_test_alloc

void* operator new(std::size_t size)
{
    return manager_internal_test_alloc::allocate(size);
}

void* operator new[](std::size_t size)
{
    return manager_internal_test_alloc::allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    return manager_internal_test_alloc::allocate(
        size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return manager_internal_test_alloc::allocate(
        size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return manager_internal_test_alloc::allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return manager_internal_test_alloc::allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void operator delete(void* ptr) noexcept
{
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept
{
    std::free(ptr);
}

void operator delete(void* ptr, std::align_val_t) noexcept
{
    std::free(ptr);
}

void operator delete[](void* ptr, std::align_val_t) noexcept
{
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept
{
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept
{
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept
{
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept
{
    std::free(ptr);
}

using namespace pldm;
using namespace pldm::fw_update;
using namespace std::chrono;

namespace
{

class AsyncDbusObjectServer
{
  public:
    explicit AsyncDbusObjectServer(const char* serviceName)
    {
        connection = std::make_shared<sdbusplus::asio::connection>(
            io, sdbusplus::bus::new_bus());
        connection->request_name(serviceName);
        server = std::make_unique<sdbusplus::asio::object_server>(connection);
        ioThread = std::thread([this] { io.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ~AsyncDbusObjectServer()
    {
        io.stop();
        if (ioThread.joinable())
        {
            ioThread.join();
        }
    }

    boost::asio::io_context io;
    std::shared_ptr<sdbusplus::asio::connection> connection;
    std::unique_ptr<sdbusplus::asio::object_server> server;

  private:
    std::thread ioThread;
};

} // namespace

class ManagerInternalTest : public testing::Test
{
  protected:
    ManagerInternalTest() :
        event(sdeventplus::Event::get_default()), instanceIdDb(),
        reqHandler(nullptr, event, instanceIdDb, false, seconds(1), 0,
                   milliseconds(1))
    {}

    void SetUp() override
    {
        pldm::utils::ManagerTestDBusHandler::reset();
    }

    void TearDown() override
    {
        pldm::utils::ManagerTestDBusHandler::reset();
    }

    sdeventplus::Event event;
    TestInstanceIdDb instanceIdDb;
    requester::Handler<requester::Request> reqHandler;
};

TEST_F(ManagerInternalTest, inlineManagerPathsAreCallable)
{
    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    dbus::MctpInterfaces mctpInterfaces;
    EXPECT_NO_THROW({ manager.getMctpInterfaces(mctpInterfaces); });

    MctpInfos mctpInfos{};
    EXPECT_NO_THROW({ manager.handleMctpEndpoints(mctpInfos, {}); });
    EXPECT_NO_THROW({ manager.handleRemovedMctpEndpoints(mctpInfos); });

    const pldm::eid eid = 9;
    const UUID uuid = "00112233445566778899AABBCCDDEEFF";
    EXPECT_NO_THROW({ manager.createInventory(eid, uuid, mctpInterfaces); });
    EXPECT_NO_THROW({ manager.updateInventory(eid, uuid, mctpInterfaces); });

    MctpInfo info{eid,
                  uuid,
                  "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe",
                  0,
                  std::nullopt,
                  "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe",
                  std::nullopt};
    EXPECT_NO_THROW({
        manager.updateMctpEndpointAvailability(info, true);
        manager.onlineMctpEndpoint(uuid, eid);
        manager.offlineMctpEndpoint(uuid, eid);
    });

    EXPECT_FALSE(manager.getActiveEidByName("does-not-exist").has_value());

    manager.updateManager.setRequestedApplyTime(
        sdbusplus::xyz::openbmc_project::Software::server::ApplyTime::
            RequestedApplyTimes::OnReset);
    EXPECT_FALSE(manager.updateManager.isApplyTimeImmediate());
    manager.updateManager.setRequestedApplyTime(
        sdbusplus::xyz::openbmc_project::Software::server::ApplyTime::
            RequestedApplyTimes::Immediate);
    EXPECT_TRUE(manager.updateManager.isApplyTimeImmediate());
}

TEST_F(ManagerInternalTest, refreshSingleEndpointCallbackPathIsCallable)
{
    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    ASSERT_TRUE(manager.updateManager.refreshSingleEndpointCallback);
    auto co = manager.updateManager.refreshSingleEndpointCallback(1, true);
    auto rc = stdexec::sync_wait(std::move(co));
    ASSERT_TRUE(rc.has_value());
}

TEST_F(ManagerInternalTest, constructorHandlesInvalidConfigPath)
{
    const std::filesystem::path badConfigPath{
        "./fw_update_jsons/does_not_exist.json"};
    EXPECT_NO_THROW({
        Manager manager(nullptr, event, reqHandler, instanceIdDb, badConfigPath,
                        true);
    });
}

TEST_F(ManagerInternalTest, constructorBadAllocCoverageInFreshTu)
{
    if (pldm::test::runningOnValgrind())
    {
        GTEST_SKIP() << "allocation-failure coverage runs in the normal pass";
    }

    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    bool sawBadAlloc = false;

    for (std::size_t failIndex = 1; failIndex <= 32 && !sawBadAlloc;
         ++failIndex)
    {
        pldm::utils::ManagerTestDBusHandler::reset();
        try
        {
            manager_internal_test_alloc::ScopedAllocationFailure failure(
                failIndex);
            [[maybe_unused]] Manager manager(nullptr, event, reqHandler,
                                             instanceIdDb, configPath, true);
        }
        catch (const std::bad_alloc&)
        {
            sawBadAlloc = true;
        }
        catch (...)
        {}
    }

    EXPECT_TRUE(sawBadAlloc);
}

TEST_F(ManagerInternalTest, getMctpInterfacesLoadsUuidFromManagedObjects)
{
    constexpr auto* endpointPath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/8";
    pldm::utils::ManagerTestDBusHandler::setSubtreeResponse(
        {{endpointPath, pldm::utils::MapperServiceMap{
                            {pldm::MCTPService,
                             std::vector<std::string>{pldm::MCTPInterface}}}}});

    AsyncDbusObjectServer mctp(pldm::MCTPService);
    mctp.server->add_manager(pldm::MCTPPath);
    auto endpointIface =
        mctp.server->add_interface(endpointPath, pldm::MCTPInterface);
    endpointIface->register_property("EID", uint8_t{8});
    endpointIface->initialize();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto uuidIface =
        mctp.server->add_interface(endpointPath, pldm::EndpointUUID);
    uuidIface->register_property(
        "UUID", std::string("00112233-4455-6677-8899-aabbccddeeff"));
    uuidIface->initialize();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    dbus::MctpInterfaces mctpInterfaces;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        manager.getMctpInterfaces(mctpInterfaces);
        if (!mctpInterfaces.empty())
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (mctpInterfaces.contains("00112233-4455-6677-8899-aabbccddeeff"))
    {
        const auto& interfaces =
            mctpInterfaces.at("00112233-4455-6677-8899-aabbccddeeff");
        EXPECT_TRUE(interfaces.contains(pldm::MCTPInterface));
        EXPECT_TRUE(interfaces.contains(pldm::EndpointUUID));
    }
}

TEST_F(ManagerInternalTest, getMctpInterfacesBadAllocCoverage)
{
    if (pldm::test::runningOnValgrind())
    {
        GTEST_SKIP() << "allocation-failure coverage runs in the normal pass";
    }

    constexpr auto* endpointPath1 =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/alloc_coverage_1";
    constexpr auto* endpointPath2 =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/alloc_coverage_2";
    constexpr auto* service = "au.com.codeconstruct.MCTP.BadAllocCoverage";
    constexpr auto* uuid1 = "00112233-4455-6677-8899-aabbccddee31";
    constexpr auto* uuid2 = "00112233-4455-6677-8899-aabbccddee32";
    constexpr auto* extraIface =
        "xyz.openbmc_project.Inventory.Decorator.AllocCoverage";

    pldm::utils::ManagerTestDBusHandler::setSubtreeResponse(
        {{endpointPath1,
          pldm::utils::MapperServiceMap{
              {service, std::vector<std::string>{pldm::MCTPInterface}}}},
         {endpointPath2,
          pldm::utils::MapperServiceMap{
              {service, std::vector<std::string>{pldm::MCTPInterface}}}}});

    AsyncDbusObjectServer mctp(service);
    mctp.server->add_manager(pldm::MCTPPath);

    auto endpointIface1 =
        mctp.server->add_interface(endpointPath1, pldm::MCTPInterface);
    endpointIface1->register_property("EID", uint8_t{31});
    endpointIface1->initialize();
    auto uuidIface1 =
        mctp.server->add_interface(endpointPath1, pldm::EndpointUUID);
    uuidIface1->register_property("UUID", std::string(uuid1));
    uuidIface1->initialize();
    auto extraIface1 = mctp.server->add_interface(endpointPath1, extraIface);
    extraIface1->register_property("Value", std::string(96, 'A'));
    extraIface1->initialize();

    auto endpointIface2 =
        mctp.server->add_interface(endpointPath2, pldm::MCTPInterface);
    endpointIface2->register_property("EID", uint8_t{32});
    endpointIface2->initialize();
    auto uuidIface2 =
        mctp.server->add_interface(endpointPath2, pldm::EndpointUUID);
    uuidIface2->register_property("UUID", std::string(uuid2));
    uuidIface2->initialize();
    auto extraIface2 = mctp.server->add_interface(
        endpointPath2, std::string(extraIface) + "." + std::string(64, 'I'));
    extraIface2->register_property("Value", std::string(96, 'B'));
    extraIface2->initialize();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    dbus::MctpInterfaces baseline;
    ASSERT_NO_THROW(manager.getMctpInterfaces(baseline));
    ASSERT_TRUE(baseline.contains(uuid1));
    ASSERT_TRUE(baseline.contains(uuid2));

    bool sawBadAlloc = false;
    for (std::size_t failIndex = 1; failIndex <= 64 && !sawBadAlloc;
         ++failIndex)
    {
        try
        {
            manager_internal_test_alloc::ScopedAllocationFailure failure(
                failIndex);
            dbus::MctpInterfaces mctpInterfaces;
            manager.getMctpInterfaces(mctpInterfaces);
        }
        catch (const std::bad_alloc&)
        {
            sawBadAlloc = true;
        }
        catch (...)
        {}
    }

    EXPECT_TRUE(sawBadAlloc);
}

TEST_F(ManagerInternalTest, getMctpInterfacesContinuesAfterManagedObjectFailure)
{
    pldm::utils::ManagerTestDBusHandler::setSubtreeResponse(
        {{"/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
          pldm::utils::MapperServiceMap{
              {"au.com.codeconstruct.MCTP.Bad",
               std::vector<std::string>{pldm::MCTPInterface}},
              {pldm::MCTPService,
               std::vector<std::string>{pldm::MCTPInterface}}}}});

    AsyncDbusObjectServer badMctp("au.com.codeconstruct.MCTP.Bad");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    AsyncDbusObjectServer mctp(pldm::MCTPService);
    mctp.server->add_manager(pldm::MCTPPath);
    auto endpointIface = mctp.server->add_interface(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
        pldm::MCTPInterface);
    endpointIface->register_property("EID", uint8_t{9});
    endpointIface->initialize();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto uuidIface = mctp.server->add_interface(
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/9",
        pldm::EndpointUUID);
    uuidIface->register_property(
        "UUID", std::string("00112233-4455-6677-8899-aabbccddee99"));
    uuidIface->initialize();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    dbus::MctpInterfaces mctpInterfaces;
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        manager.getMctpInterfaces(mctpInterfaces);
        if (mctpInterfaces.contains("00112233-4455-6677-8899-aabbccddee99"))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    EXPECT_TRUE(
        mctpInterfaces.contains("00112233-4455-6677-8899-aabbccddee99"));
}

TEST_F(ManagerInternalTest, getMctpInterfacesReturnsEmptyWhenGetSubtreeThrows)
{
    pldm::utils::ManagerTestDBusHandler::setThrowGetSubtree(true);

    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    dbus::MctpInterfaces mctpInterfaces;
    EXPECT_NO_THROW(manager.getMctpInterfaces(mctpInterfaces));
    EXPECT_TRUE(mctpInterfaces.empty());
}

TEST_F(ManagerInternalTest,
       getMctpInterfacesReturnsEmptyForEmptySubtreeResponse)
{
    pldm::utils::ManagerTestDBusHandler::setSubtreeResponse({});

    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    dbus::MctpInterfaces mctpInterfaces;
    EXPECT_NO_THROW(manager.getMctpInterfaces(mctpInterfaces));
    EXPECT_TRUE(mctpInterfaces.empty());
}

TEST_F(ManagerInternalTest, getMctpInterfacesSkipsEntriesWithEmptyServiceMaps)
{
    constexpr auto* endpointPath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/18";
    constexpr auto* uuid = "00112233-4455-6677-8899-aabbccddee18";
    pldm::utils::ManagerTestDBusHandler::setSubtreeResponse(
        {{"/au/com/codeconstruct/mctp1/networks/1/endpoints/ignored", {}},
         {endpointPath, pldm::utils::MapperServiceMap{
                            {pldm::MCTPService,
                             std::vector<std::string>{pldm::MCTPInterface}}}}});

    AsyncDbusObjectServer mctp(pldm::MCTPService);
    mctp.server->add_manager(pldm::MCTPPath);
    auto endpointIface =
        mctp.server->add_interface(endpointPath, pldm::MCTPInterface);
    endpointIface->register_property("EID", uint8_t{18});
    endpointIface->initialize();
    auto uuidIface =
        mctp.server->add_interface(endpointPath, pldm::EndpointUUID);
    uuidIface->register_property("UUID", std::string(uuid));
    uuidIface->initialize();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    dbus::MctpInterfaces mctpInterfaces;
    EXPECT_NO_THROW(manager.getMctpInterfaces(mctpInterfaces));
    ASSERT_TRUE(mctpInterfaces.contains(uuid));
    EXPECT_TRUE(mctpInterfaces.at(uuid).contains(pldm::EndpointUUID));
}

TEST_F(ManagerInternalTest,
       handleMctpEndpointsPopulatesComponentNamesWhenInventoryMatches)
{
    constexpr auto* endpointPath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/11";
    constexpr auto* uuid = "00112233-4455-6677-8899-aabbccddeea1";
    pldm::utils::ManagerTestDBusHandler::setSubtreeResponse(
        {{endpointPath, pldm::utils::MapperServiceMap{
                            {pldm::MCTPService,
                             std::vector<std::string>{pldm::MCTPInterface}}}}});

    AsyncDbusObjectServer mctp(pldm::MCTPService);
    mctp.server->add_manager(pldm::MCTPPath);
    auto endpointIface =
        mctp.server->add_interface(endpointPath, pldm::MCTPInterface);
    endpointIface->register_property("EID", uint8_t{11});
    endpointIface->initialize();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto uuidIface =
        mctp.server->add_interface(endpointPath, pldm::EndpointUUID);
    uuidIface->register_property("UUID", std::string(uuid));
    uuidIface->initialize();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);
    manager.inventoryMgr.discoverFDsTaskHandle.emplace();
    manager.componentNameMapInfo.infos = MatchComponentNameMapInfo{
        {DBusIntfMatch{pldm::EndpointUUID,
                       {{"UUID", dbus::Value{std::string(uuid)}}}},
         ComponentIdNameMap{
             {static_cast<uint16_t>(3), "MatchedComponentName"}}}};

    MctpInfos mctpInfos{
        {11, uuid, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 0,
         std::nullopt, "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe",
         std::nullopt}};

    for (int attempt = 0; attempt < 20; ++attempt)
    {
        manager.handleMctpEndpoints(mctpInfos, {});
        if (manager.componentNameMap.contains(11))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    ASSERT_TRUE(manager.componentNameMap.contains(11));
    ASSERT_TRUE(
        manager.componentNameMap.at(11).contains(static_cast<uint16_t>(3)));
    EXPECT_EQ(manager.componentNameMap.at(11).at(static_cast<uint16_t>(3)),
              "MatchedComponentName");
}

TEST_F(ManagerInternalTest, getMctpInterfacesDeduplicatesRepeatedServices)
{
    constexpr auto* endpointPath1 =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/12";
    constexpr auto* endpointPath2 =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/13";
    constexpr auto* uuid1 = "00112233-4455-6677-8899-aabbccddeea2";
    constexpr auto* uuid2 = "00112233-4455-6677-8899-aabbccddeea3";

    pldm::utils::ManagerTestDBusHandler::setSubtreeResponse(
        {{endpointPath1,
          pldm::utils::MapperServiceMap{
              {pldm::MCTPService,
               std::vector<std::string>{pldm::MCTPInterface}}}},
         {endpointPath2, pldm::utils::MapperServiceMap{
                             {pldm::MCTPService, std::vector<std::string>{
                                                     pldm::MCTPInterface}}}}});

    AsyncDbusObjectServer mctp(pldm::MCTPService);
    mctp.server->add_manager(pldm::MCTPPath);

    auto endpointIface1 =
        mctp.server->add_interface(endpointPath1, pldm::MCTPInterface);
    endpointIface1->register_property("EID", uint8_t{12});
    endpointIface1->initialize();
    auto uuidIface1 =
        mctp.server->add_interface(endpointPath1, pldm::EndpointUUID);
    uuidIface1->register_property("UUID", std::string(uuid1));
    uuidIface1->initialize();

    auto endpointIface2 =
        mctp.server->add_interface(endpointPath2, pldm::MCTPInterface);
    endpointIface2->register_property("EID", uint8_t{13});
    endpointIface2->initialize();
    auto uuidIface2 =
        mctp.server->add_interface(endpointPath2, pldm::EndpointUUID);
    uuidIface2->register_property("UUID", std::string(uuid2));
    uuidIface2->initialize();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    dbus::MctpInterfaces mctpInterfaces;
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        manager.getMctpInterfaces(mctpInterfaces);
        if (mctpInterfaces.size() == 2)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    EXPECT_EQ(mctpInterfaces.size(), 2u);
    EXPECT_TRUE(mctpInterfaces.contains(uuid1));
    EXPECT_TRUE(mctpInterfaces.contains(uuid2));
}

TEST_F(ManagerInternalTest, getMctpInterfacesSkipsWhenUuidPropertyIsMissing)
{
    constexpr auto* endpointPath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/14";
    constexpr auto* service = "au.com.codeconstruct.MCTP.NoUuid";
    pldm::utils::ManagerTestDBusHandler::setSubtreeResponse(
        {{endpointPath,
          pldm::utils::MapperServiceMap{
              {service, std::vector<std::string>{pldm::MCTPInterface}}}}});

    AsyncDbusObjectServer mctp(service);
    mctp.server->add_manager(pldm::MCTPPath);
    auto endpointIface =
        mctp.server->add_interface(endpointPath, pldm::MCTPInterface);
    endpointIface->register_property("EID", uint8_t{14});
    endpointIface->initialize();
    auto uuidIface =
        mctp.server->add_interface(endpointPath, pldm::EndpointUUID);
    uuidIface->initialize();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    dbus::MctpInterfaces mctpInterfaces;
    // Post-migration getMctpInterfaces() skips endpoints whose UUID property is
    // absent (typed accessor) instead of throwing; the endpoint is dropped.
    EXPECT_NO_THROW(manager.getMctpInterfaces(mctpInterfaces));
    EXPECT_TRUE(mctpInterfaces.empty());
}

TEST_F(ManagerInternalTest, getMctpInterfacesSkipsWhenUuidPropertyHasWrongType)
{
    constexpr auto* endpointPath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/15";
    constexpr auto* service = "au.com.codeconstruct.MCTP.BadUuidType";
    pldm::utils::ManagerTestDBusHandler::setSubtreeResponse(
        {{endpointPath,
          pldm::utils::MapperServiceMap{
              {service, std::vector<std::string>{pldm::MCTPInterface}}}}});

    AsyncDbusObjectServer mctp(service);
    mctp.server->add_manager(pldm::MCTPPath);
    auto endpointIface =
        mctp.server->add_interface(endpointPath, pldm::MCTPInterface);
    endpointIface->register_property("EID", uint8_t{15});
    endpointIface->initialize();
    auto uuidIface =
        mctp.server->add_interface(endpointPath, pldm::EndpointUUID);
    uuidIface->register_property("UUID", uint64_t{15});
    uuidIface->initialize();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    dbus::MctpInterfaces mctpInterfaces;
    // Post-migration getMctpInterfaces() skips endpoints whose UUID property is
    // not a string (typed accessor returns nullptr) instead of throwing.
    EXPECT_NO_THROW(manager.getMctpInterfaces(mctpInterfaces));
    EXPECT_TRUE(mctpInterfaces.empty());
}

TEST_F(ManagerInternalTest, getMctpInterfacesSkipsManagedObjectsWithoutUuid)
{
    constexpr auto* endpointPath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/16";
    constexpr auto* service = "au.com.codeconstruct.MCTP.NoUuidInterface";
    pldm::utils::ManagerTestDBusHandler::setSubtreeResponse(
        {{endpointPath,
          pldm::utils::MapperServiceMap{
              {service, std::vector<std::string>{pldm::MCTPInterface}}}}});

    AsyncDbusObjectServer mctp(service);
    mctp.server->add_manager(pldm::MCTPPath);
    auto endpointIface =
        mctp.server->add_interface(endpointPath, pldm::MCTPInterface);
    endpointIface->register_property("EID", uint8_t{16});
    endpointIface->initialize();
    auto otherIface = mctp.server->add_interface(
        endpointPath, "xyz.openbmc_project.Example.Interface");
    otherIface->register_property("Value", std::string("ignored"));
    otherIface->initialize();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    dbus::MctpInterfaces mctpInterfaces;
    EXPECT_NO_THROW(manager.getMctpInterfaces(mctpInterfaces));
    EXPECT_TRUE(mctpInterfaces.empty());
}

TEST_F(ManagerInternalTest, getMctpInterfacesSkipsServicesWithNoManagedObjects)
{
    constexpr auto* endpointPath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/21";
    constexpr auto* emptyService = "au.com.codeconstruct.MCTP.EmptyManaged";
    constexpr auto* validService = "au.com.codeconstruct.MCTP.ValidManaged";
    constexpr auto* uuid = "00112233-4455-6677-8899-aabbccddee21";
    pldm::utils::ManagerTestDBusHandler::setSubtreeResponse(
        {{endpointPath,
          pldm::utils::MapperServiceMap{
              {emptyService, std::vector<std::string>{pldm::MCTPInterface}},
              {validService, std::vector<std::string>{pldm::MCTPInterface}}}}});

    AsyncDbusObjectServer emptyManaged(emptyService);
    emptyManaged.server->add_manager(pldm::MCTPPath);

    AsyncDbusObjectServer validManaged(validService);
    validManaged.server->add_manager(pldm::MCTPPath);
    auto endpointIface =
        validManaged.server->add_interface(endpointPath, pldm::MCTPInterface);
    endpointIface->register_property("EID", uint8_t{21});
    endpointIface->initialize();
    auto uuidIface =
        validManaged.server->add_interface(endpointPath, pldm::EndpointUUID);
    uuidIface->register_property("UUID", std::string(uuid));
    uuidIface->initialize();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    dbus::MctpInterfaces mctpInterfaces;
    EXPECT_NO_THROW(manager.getMctpInterfaces(mctpInterfaces));
    ASSERT_TRUE(mctpInterfaces.contains(uuid));
    EXPECT_TRUE(mctpInterfaces.at(uuid).contains(pldm::EndpointUUID));
}

TEST_F(ManagerInternalTest,
       getMctpInterfacesSkipsManagedObjectsWithNoInterfaces)
{
    constexpr auto* endpointPath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/22";
    constexpr auto* emptyPath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/empty";
    constexpr auto* service = "au.com.codeconstruct.MCTP.EmptyInterfaces";
    constexpr auto* uuid = "00112233-4455-6677-8899-aabbccddee22";
    pldm::utils::ManagerTestDBusHandler::setSubtreeResponse(
        {{endpointPath,
          pldm::utils::MapperServiceMap{
              {service, std::vector<std::string>{pldm::MCTPInterface}}}}});

    AsyncDbusObjectServer mctp(service);
    mctp.server->add_manager(pldm::MCTPPath);
    auto emptyIface = mctp.server->add_interface(
        emptyPath, "xyz.openbmc_project.Test.EmptyInterface");
    emptyIface->initialize();
    auto endpointIface =
        mctp.server->add_interface(endpointPath, pldm::MCTPInterface);
    endpointIface->register_property("EID", uint8_t{22});
    endpointIface->initialize();
    auto uuidIface =
        mctp.server->add_interface(endpointPath, pldm::EndpointUUID);
    uuidIface->register_property("UUID", std::string(uuid));
    uuidIface->initialize();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    dbus::MctpInterfaces mctpInterfaces;
    EXPECT_NO_THROW(manager.getMctpInterfaces(mctpInterfaces));
    ASSERT_TRUE(mctpInterfaces.contains(uuid));
    EXPECT_TRUE(mctpInterfaces.at(uuid).contains(pldm::EndpointUUID));
}

TEST_F(ManagerInternalTest, constructorHandlesMissingConfigFileCoverage)
{
    const std::filesystem::path configPath{
        "./fw_update_jsons/does_not_exist_for_coverage.json"};

    EXPECT_NO_THROW({
        [[maybe_unused]] Manager manager(nullptr, event, reqHandler,
                                         instanceIdDb, configPath, true);
    });
}

TEST_F(ManagerInternalTest, constructorHandlesMalformedConfigCoverage)
{
    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_malformed.json"};

    EXPECT_NO_THROW({
        [[maybe_unused]] Manager manager(nullptr, event, reqHandler,
                                         instanceIdDb, configPath, true);
    });
}

TEST_F(ManagerInternalTest,
       getMctpInterfacesSkipsNonUuidInterfacesAcrossManagedObjects)
{
    constexpr auto* endpointPath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/19";
    constexpr auto* ignoredPath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/20";
    constexpr auto* uuid = "00112233-4455-6677-8899-aabbccddee19";
    pldm::utils::ManagerTestDBusHandler::setSubtreeResponse(
        {{endpointPath, pldm::utils::MapperServiceMap{
                            {pldm::MCTPService,
                             std::vector<std::string>{pldm::MCTPInterface}}}}});

    AsyncDbusObjectServer mctp(pldm::MCTPService);
    mctp.server->add_manager(pldm::MCTPPath);

    auto endpointIface =
        mctp.server->add_interface(endpointPath, pldm::MCTPInterface);
    endpointIface->register_property("EID", uint8_t{19});
    endpointIface->initialize();
    auto uuidIface =
        mctp.server->add_interface(endpointPath, pldm::EndpointUUID);
    uuidIface->register_property("UUID", std::string(uuid));
    uuidIface->initialize();
    auto otherIface = mctp.server->add_interface(
        endpointPath, "xyz.openbmc_project.Inventory.Decorator.Asset");
    otherIface->register_property("Manufacturer", std::string("ACME"));
    otherIface->initialize();

    auto ignoredIface = mctp.server->add_interface(
        ignoredPath, "xyz.openbmc_project.Inventory.Decorator.Asset");
    ignoredIface->register_property("Manufacturer", std::string("Ignored"));
    ignoredIface->initialize();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    dbus::MctpInterfaces mctpInterfaces;
    EXPECT_NO_THROW(manager.getMctpInterfaces(mctpInterfaces));
    ASSERT_TRUE(mctpInterfaces.contains(uuid));
    EXPECT_TRUE(mctpInterfaces.at(uuid).contains(pldm::EndpointUUID));
    EXPECT_TRUE(mctpInterfaces.at(uuid).contains(pldm::MCTPInterface));
    EXPECT_TRUE(mctpInterfaces.at(uuid).contains(
        "xyz.openbmc_project.Inventory.Decorator.Asset"));
}

TEST_F(ManagerInternalTest, getMctpInterfacesHandlesGetSubtreeFailure)
{
    pldm::utils::ManagerTestDBusHandler::setThrowGetSubtree(true);

    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    dbus::MctpInterfaces mctpInterfaces;
    EXPECT_NO_THROW(manager.getMctpInterfaces(mctpInterfaces));
    EXPECT_TRUE(mctpInterfaces.empty());
}

TEST_F(ManagerInternalTest,
       getMctpInterfacesContinuesAfterGetManagedObjectsFailure)
{
    constexpr auto* validEndpointPath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/18";
    constexpr auto* uuid = "00112233-4455-6677-8899-aabbccddeea8";
    constexpr auto* invalidService = "xyz.openbmc_project.Test.DoesNotExist";
    pldm::utils::ManagerTestDBusHandler::setSubtreeResponse(
        {{validEndpointPath,
          pldm::utils::MapperServiceMap{
              {pldm::MCTPService,
               std::vector<std::string>{pldm::MCTPInterface}}}},
         {"/au/com/codeconstruct/mctp1/networks/1/endpoints/invalid",
          pldm::utils::MapperServiceMap{
              {invalidService,
               std::vector<std::string>{pldm::MCTPInterface}}}}});

    AsyncDbusObjectServer mctp(pldm::MCTPService);
    mctp.server->add_manager(pldm::MCTPPath);
    auto endpointIface =
        mctp.server->add_interface(validEndpointPath, pldm::MCTPInterface);
    endpointIface->register_property("EID", uint8_t{18});
    endpointIface->initialize();
    auto uuidIface =
        mctp.server->add_interface(validEndpointPath, pldm::EndpointUUID);
    uuidIface->register_property("UUID", std::string(uuid));
    uuidIface->initialize();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    dbus::MctpInterfaces mctpInterfaces;
    EXPECT_NO_THROW(manager.getMctpInterfaces(mctpInterfaces));
    ASSERT_TRUE(mctpInterfaces.contains(uuid));
    EXPECT_TRUE(mctpInterfaces.at(uuid).contains(pldm::EndpointUUID));
    EXPECT_TRUE(mctpInterfaces.at(uuid).contains(pldm::MCTPInterface));
}

TEST_F(ManagerInternalTest, getMctpInterfacesOverwritesDuplicateUuidEntries)
{
    constexpr auto* endpointPath1 =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/23";
    constexpr auto* endpointPath2 =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/24";
    constexpr auto* uuid = "00112233-4455-6677-8899-aabbccddee24";
    constexpr auto* extraIntf1 = "xyz.openbmc_project.Test.Interface.One";
    constexpr auto* extraIntf2 = "xyz.openbmc_project.Test.Interface.Two";

    pldm::utils::ManagerTestDBusHandler::setSubtreeResponse(
        {{endpointPath1,
          pldm::utils::MapperServiceMap{
              {pldm::MCTPService,
               std::vector<std::string>{pldm::MCTPInterface}}}},
         {endpointPath2, pldm::utils::MapperServiceMap{
                             {pldm::MCTPService, std::vector<std::string>{
                                                     pldm::MCTPInterface}}}}});

    AsyncDbusObjectServer mctp(pldm::MCTPService);
    mctp.server->add_manager(pldm::MCTPPath);

    auto endpointIface1 =
        mctp.server->add_interface(endpointPath1, pldm::MCTPInterface);
    endpointIface1->register_property("EID", uint8_t{23});
    endpointIface1->initialize();
    auto uuidIface1 =
        mctp.server->add_interface(endpointPath1, pldm::EndpointUUID);
    uuidIface1->register_property("UUID", std::string(uuid));
    uuidIface1->initialize();
    auto extraIface1 = mctp.server->add_interface(endpointPath1, extraIntf1);
    extraIface1->register_property("Value", std::string("first"));
    extraIface1->initialize();

    auto endpointIface2 =
        mctp.server->add_interface(endpointPath2, pldm::MCTPInterface);
    endpointIface2->register_property("EID", uint8_t{24});
    endpointIface2->initialize();
    auto uuidIface2 =
        mctp.server->add_interface(endpointPath2, pldm::EndpointUUID);
    uuidIface2->register_property("UUID", std::string(uuid));
    uuidIface2->initialize();
    auto extraIface2 = mctp.server->add_interface(endpointPath2, extraIntf2);
    extraIface2->register_property("Value", std::string("second"));
    extraIface2->initialize();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    dbus::MctpInterfaces mctpInterfaces;
    EXPECT_NO_THROW(manager.getMctpInterfaces(mctpInterfaces));
    ASSERT_EQ(mctpInterfaces.size(), 1u);
    ASSERT_TRUE(mctpInterfaces.contains(uuid));
    EXPECT_TRUE(mctpInterfaces.at(uuid).contains(pldm::EndpointUUID));
    EXPECT_TRUE(mctpInterfaces.at(uuid).contains(pldm::MCTPInterface));
    EXPECT_FALSE(mctpInterfaces.at(uuid).contains(extraIntf1));
    EXPECT_TRUE(mctpInterfaces.at(uuid).contains(extraIntf2));
}

TEST_F(ManagerInternalTest,
       getMctpInterfacesPreservesPreviousUuidWhenLaterEntryHasNoUuid)
{
    constexpr auto* validEndpointPath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/25";
    constexpr auto* invalidEndpointPath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/26";
    constexpr auto* uuid = "00112233-4455-6677-8899-aabbccddee25";
    constexpr auto* service = "au.com.codeconstruct.MCTP.MixedEntries";

    pldm::utils::ManagerTestDBusHandler::setSubtreeResponse(
        {{validEndpointPath,
          pldm::utils::MapperServiceMap{
              {service, std::vector<std::string>{pldm::MCTPInterface}}}},
         {invalidEndpointPath,
          pldm::utils::MapperServiceMap{
              {service, std::vector<std::string>{pldm::MCTPInterface}}}}});

    AsyncDbusObjectServer mctp(service);
    mctp.server->add_manager(pldm::MCTPPath);

    auto validEndpointIface =
        mctp.server->add_interface(validEndpointPath, pldm::MCTPInterface);
    validEndpointIface->register_property("EID", uint8_t{25});
    validEndpointIface->initialize();
    auto validUuidIface =
        mctp.server->add_interface(validEndpointPath, pldm::EndpointUUID);
    validUuidIface->register_property("UUID", std::string(uuid));
    validUuidIface->initialize();

    auto invalidEndpointIface =
        mctp.server->add_interface(invalidEndpointPath, pldm::MCTPInterface);
    invalidEndpointIface->register_property("EID", uint8_t{26});
    invalidEndpointIface->initialize();
    auto invalidOtherIface = mctp.server->add_interface(
        invalidEndpointPath, "xyz.openbmc_project.Test.Interface");
    invalidOtherIface->register_property("Value", std::string("ignored"));
    invalidOtherIface->initialize();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    dbus::MctpInterfaces mctpInterfaces;
    EXPECT_NO_THROW(manager.getMctpInterfaces(mctpInterfaces));
    ASSERT_EQ(mctpInterfaces.size(), 1u);
    ASSERT_TRUE(mctpInterfaces.contains(uuid));
    EXPECT_TRUE(mctpInterfaces.at(uuid).contains(pldm::EndpointUUID));
    EXPECT_TRUE(mctpInterfaces.at(uuid).contains(pldm::MCTPInterface));
}

TEST_F(ManagerInternalTest,
       getMctpInterfacesRetainsLargeManagedObjectDataAcrossRepeatedCalls)
{
    constexpr auto* endpointPath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/alloc_long_path_27";
    constexpr auto* service = "au.com.codeconstruct.MCTP.AllocCoverage";
    constexpr auto* uuid = "00112233-4455-6677-8899-aabbccddee27";

    pldm::utils::ManagerTestDBusHandler::setSubtreeResponse(
        {{endpointPath,
          pldm::utils::MapperServiceMap{
              {service, std::vector<std::string>{pldm::MCTPInterface}}}}});

    AsyncDbusObjectServer mctp(service);
    mctp.server->add_manager(pldm::MCTPPath);
    auto endpointIface =
        mctp.server->add_interface(endpointPath, pldm::MCTPInterface);
    endpointIface->register_property("EID", uint8_t{27});
    endpointIface->initialize();
    auto uuidIface =
        mctp.server->add_interface(endpointPath, pldm::EndpointUUID);
    uuidIface->register_property("UUID", uuid);
    uuidIface->initialize();
    auto extraIface = mctp.server->add_interface(
        endpointPath, "xyz.openbmc_project.Inventory.Decorator.Asset");
    extraIface->register_property("Manufacturer", std::string(96, 'M'));
    extraIface->initialize();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    dbus::MctpInterfaces mctpInterfaces;
    EXPECT_NO_THROW(manager.getMctpInterfaces(mctpInterfaces));
    ASSERT_TRUE(mctpInterfaces.contains(uuid));
    EXPECT_TRUE(mctpInterfaces.at(uuid).contains(pldm::EndpointUUID));
    EXPECT_TRUE(mctpInterfaces.at(uuid).contains(pldm::MCTPInterface));
    EXPECT_TRUE(mctpInterfaces.at(uuid).contains(
        "xyz.openbmc_project.Inventory.Decorator.Asset"));

    dbus::MctpInterfaces retriedInterfaces;
    EXPECT_NO_THROW(manager.getMctpInterfaces(retriedInterfaces));
    ASSERT_TRUE(retriedInterfaces.contains(uuid));
    EXPECT_EQ(retriedInterfaces.at(uuid), mctpInterfaces.at(uuid));
}

TEST_F(ManagerInternalTest,
       getMctpInterfacesRetainsMultipleServicesAcrossRepeatedCalls)
{
    constexpr auto* endpointPath1 =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/alloc_28";
    constexpr auto* endpointPath2 =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/alloc_29";
    constexpr auto* service1 = "au.com.codeconstruct.MCTP.AllocCoverage.One";
    constexpr auto* service2 = "au.com.codeconstruct.MCTP.AllocCoverage.Two";
    constexpr auto* uuid1 = "00112233-4455-6677-8899-aabbccddee28";
    constexpr auto* uuid2 = "00112233-4455-6677-8899-aabbccddee29";

    pldm::utils::ManagerTestDBusHandler::setSubtreeResponse(
        {{endpointPath1,
          pldm::utils::MapperServiceMap{
              {service1, std::vector<std::string>{pldm::MCTPInterface}}}},
         {endpointPath2,
          pldm::utils::MapperServiceMap{
              {service2, std::vector<std::string>{pldm::MCTPInterface}}}}});

    AsyncDbusObjectServer first(service1);
    first.server->add_manager(pldm::MCTPPath);
    auto endpointIface1 =
        first.server->add_interface(endpointPath1, pldm::MCTPInterface);
    endpointIface1->register_property("EID", uint8_t{28});
    endpointIface1->initialize();
    auto uuidIface1 =
        first.server->add_interface(endpointPath1, pldm::EndpointUUID);
    uuidIface1->register_property("UUID", uuid1);
    uuidIface1->initialize();

    AsyncDbusObjectServer second(service2);
    second.server->add_manager(pldm::MCTPPath);
    auto endpointIface2 =
        second.server->add_interface(endpointPath2, pldm::MCTPInterface);
    endpointIface2->register_property("EID", uint8_t{29});
    endpointIface2->initialize();
    auto uuidIface2 =
        second.server->add_interface(endpointPath2, pldm::EndpointUUID);
    uuidIface2->register_property("UUID", uuid2);
    uuidIface2->initialize();
    auto extraIface2 = second.server->add_interface(
        endpointPath2,
        "xyz.openbmc_project.Test.Interface." + std::string(64, 'I'));
    extraIface2->register_property("Value", std::string(88, 'V'));
    extraIface2->initialize();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    dbus::MctpInterfaces mctpInterfaces;
    EXPECT_NO_THROW(manager.getMctpInterfaces(mctpInterfaces));
    ASSERT_TRUE(mctpInterfaces.contains(uuid1));
    ASSERT_TRUE(mctpInterfaces.contains(uuid2));
    EXPECT_TRUE(mctpInterfaces.at(uuid1).contains(pldm::EndpointUUID));
    EXPECT_TRUE(mctpInterfaces.at(uuid2).contains(pldm::EndpointUUID));
    EXPECT_TRUE(mctpInterfaces.at(uuid2).contains(
        "xyz.openbmc_project.Test.Interface." + std::string(64, 'I')));

    dbus::MctpInterfaces retriedInterfaces;
    EXPECT_NO_THROW(manager.getMctpInterfaces(retriedInterfaces));
    ASSERT_TRUE(retriedInterfaces.contains(uuid1));
    ASSERT_TRUE(retriedInterfaces.contains(uuid2));
    EXPECT_EQ(retriedInterfaces.at(uuid1), mctpInterfaces.at(uuid1));
    EXPECT_EQ(retriedInterfaces.at(uuid2), mctpInterfaces.at(uuid2));
}

TEST_F(ManagerInternalTest,
       handleMctpEndpointsLeavesComponentNameMapEmptyWhenNoInventoryMatch)
{
    constexpr auto* endpointPath =
        "/au/com/codeconstruct/mctp1/networks/1/endpoints/17";
    constexpr auto* uuid = "00112233-4455-6677-8899-aabbccddeea7";
    pldm::utils::ManagerTestDBusHandler::setSubtreeResponse(
        {{endpointPath, pldm::utils::MapperServiceMap{
                            {pldm::MCTPService,
                             std::vector<std::string>{pldm::MCTPInterface}}}}});

    AsyncDbusObjectServer mctp(pldm::MCTPService);
    mctp.server->add_manager(pldm::MCTPPath);
    auto endpointIface =
        mctp.server->add_interface(endpointPath, pldm::MCTPInterface);
    endpointIface->register_property("EID", uint8_t{17});
    endpointIface->initialize();
    auto uuidIface =
        mctp.server->add_interface(endpointPath, pldm::EndpointUUID);
    uuidIface->register_property("UUID", std::string(uuid));
    uuidIface->initialize();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);
    manager.inventoryMgr.discoverFDsTaskHandle.emplace();
    manager.componentNameMapInfo.infos = MatchComponentNameMapInfo{
        {DBusIntfMatch{pldm::EndpointUUID,
                       {{"UUID", dbus::Value{std::string("mismatched-uuid")}}}},
         ComponentIdNameMap{{static_cast<uint16_t>(7), "ShouldNotMatch"}}}};

    MctpInfos mctpInfos{
        {17, uuid, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 0,
         std::nullopt, "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe",
         std::nullopt}};

    manager.handleMctpEndpoints(mctpInfos, {});

    EXPECT_FALSE(manager.componentNameMap.contains(17));
}

TEST_F(ManagerInternalTest, createAndUpdateInventoryCoverComponentMapBranches)
{
    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    const pldm::eid eid = 7;
    const UUID uuid = "00112233445566778899AABBCCDDEEFF";
    dbus::MctpInterfaces mctpInterfaces{};

    manager.componentInfoMap[eid] = {
        {std::make_pair(static_cast<uint16_t>(1), static_cast<uint16_t>(2)),
         std::make_tuple(static_cast<uint8_t>(1), std::string("v1.2.3"),
                         static_cast<uint16_t>(0))}};
    manager.componentNameMap.clear();

    EXPECT_NO_THROW({ manager.createInventory(eid, uuid, mctpInterfaces); });
    EXPECT_TRUE(manager.componentNameMap.contains(eid));
    EXPECT_FALSE(manager.componentNameMap[eid].empty());

    EXPECT_NO_THROW({ manager.updateInventory(eid, uuid, mctpInterfaces); });
}

TEST_F(ManagerInternalTest, updateFwInventoryAndHandleRequestAreCallable)
{
    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    const pldm::eid eid = 5;
    EXPECT_NO_THROW({ manager.updateFWInventory(eid); });

    const pldm_msg req{};
    EXPECT_NO_THROW({
        [[maybe_unused]] auto resp = manager.handleRequest(
            eid, static_cast<Command>(0), &req, sizeof(req));
    });

    MctpInfos removed{
        {eid, "00112233445566778899AABBCCDDEEFF",
         "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 0, std::nullopt,
         "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", std::nullopt}};
    EXPECT_NO_THROW({ manager.handleRemovedMctpEndpoints(removed); });
}

TEST_F(ManagerInternalTest,
       createInventoryRetainsExistingComponentNamesWhenAlreadyPresent)
{
    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    const pldm::eid eid = 21;
    const UUID uuid = "00112233445566778899AABBCCDDEEFF";
    dbus::MctpInterfaces mctpInterfaces{};

    manager.componentInfoMap[eid] = {
        {std::make_pair(static_cast<uint16_t>(2), static_cast<uint16_t>(3)),
         std::make_tuple(static_cast<uint8_t>(1), std::string("v9.9.9"),
                         static_cast<uint16_t>(0))}};
    manager.componentNameMap[eid] = {
        {static_cast<uint16_t>(3), "ExistingComponentName"}};

    EXPECT_NO_THROW({ manager.createInventory(eid, uuid, mctpInterfaces); });
    ASSERT_TRUE(manager.componentNameMap.contains(eid));
    EXPECT_EQ(manager.componentNameMap[eid][static_cast<uint16_t>(3)],
              "ExistingComponentName");
}

TEST_F(ManagerInternalTest,
       handleMctpEndpointsNonEmptyInputWhenDiscoveryAlreadyPending)
{
    const std::filesystem::path configPath{
        "./fw_update_jsons/fw_update_config_single_entry.json"};
    Manager manager(nullptr, event, reqHandler, instanceIdDb, configPath, true);

    manager.inventoryMgr.discoverFDsTaskHandle.emplace();

    const UUID uuid = "00112233445566778899AABBCCDDEEFF";
    MctpInfos mctpInfos{
        {9, uuid, "xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 0,
         std::nullopt, "xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe",
         std::nullopt}};

    // Populate MctpInterfaces with the endpoint UUID so the signal cache
    // path is exercised (no fallback D-Bus scan).
    dbus::MctpInterfaces mctpIfMap{{uuid, {}}};
    EXPECT_NO_THROW({ manager.handleMctpEndpoints(mctpInfos, mctpIfMap); });
    EXPECT_TRUE(manager.inventoryMgr.queuedMctpInfos.size() >= 1);
}

// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
