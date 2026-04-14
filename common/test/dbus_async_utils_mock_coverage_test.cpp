#include "../../test/test_valgrind_utils.hpp"

#include <coroutine>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#define MOCK_DBUS_ASYNC_UTILS
#define coGetDbusProperty MockCoverageCoGetDbusProperty
#define coGetServiceMap MockCoverageCoGetServiceMap
#define coGetSubTree MockCoverageCoGetSubTree
#include "../dBusAsyncUtils.hpp"
#undef coGetSubTree
#undef coGetServiceMap
#undef coGetDbusProperty
#undef MOCK_DBUS_ASYNC_UTILS

namespace async_utils_mock_test
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
    if (pldm::test::runningOnValgrind())
    {
        return true;
    }

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

} // namespace async_utils_mock_test

void* operator new(std::size_t size)
{
    return async_utils_mock_test::allocate(size);
}

void* operator new[](std::size_t size)
{
    return async_utils_mock_test::allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    return async_utils_mock_test::allocate(size,
                                           static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return async_utils_mock_test::allocate(size,
                                           static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return async_utils_mock_test::allocate(size);
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
        return async_utils_mock_test::allocate(size);
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

namespace
{

using Associations =
    std::vector<std::tuple<std::string, std::string, std::string>>;
using ObjectPaths = std::vector<sdbusplus::message::object_path>;

template <typename T>
void expectMockPropertyExhaustiveBadAlloc(
    const std::string& objectPath, const std::string& property,
    const std::string& interface, const std::string& service,
    const T& storedValue, std::size_t maxFailAt = 512)
{
    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::MockCoverageCoGetDbusProperty<T> op(
                objectPath, property, interface, service);
            (void)storedValue;
            (void)op;
        },
        maxFailAt));
}

template <typename T>
void expectMockPropertyAwaitableDefaults(
    const std::string& objectPath, const std::string& property,
    const std::string& interface, const T& expected)
{
    pldm::utils::MockCoverageCoGetDbusProperty<T> op(objectPath, property,
                                                     interface);
    EXPECT_TRUE(op.await_ready());
    EXPECT_TRUE(op.await_suspend(std::coroutine_handle<>{}));
    EXPECT_EQ(op.await_resume(), expected);
}

template <typename T>
void expectMockPropertyCtorRetainsCustomService(
    const std::string& objectPath, const std::string& property,
    const std::string& interface, const std::string& service, const T& stored)
{
    pldm::utils::MockCoverageCoGetDbusProperty<T> op(objectPath, property,
                                                     interface, service);
    op.ret = stored;

    EXPECT_EQ(op.service, service);
    EXPECT_EQ(op.objectPath, objectPath);
    EXPECT_EQ(op.interface, interface);
    EXPECT_EQ(op.property, property);
    EXPECT_TRUE(op.await_ready());
    EXPECT_TRUE(op.await_suspend(std::coroutine_handle<>{}));
    EXPECT_EQ(op.await_resume(), stored);
}

TEST(DBusAsyncUtilsMockCoverage, coGetDbusPropertyCoversMockModeTypes)
{
    const std::string objectPath = "/xyz/openbmc_project/test";
    const std::string interface = "xyz.openbmc_project.Test";

    expectMockPropertyAwaitableDefaults<std::string>(objectPath, "Name",
                                                     interface, "");
    expectMockPropertyAwaitableDefaults<bool>(objectPath, "Present", interface,
                                              false);
    expectMockPropertyAwaitableDefaults<uint8_t>(objectPath, "U8", interface,
                                                 uint8_t(0));
    expectMockPropertyAwaitableDefaults<int16_t>(objectPath, "I16", interface,
                                                 int16_t(0));
    expectMockPropertyAwaitableDefaults<uint16_t>(objectPath, "U16", interface,
                                                  uint16_t(0));
    expectMockPropertyAwaitableDefaults<int32_t>(objectPath, "I32", interface,
                                                 int32_t(0));
    expectMockPropertyAwaitableDefaults<uint32_t>(objectPath, "U32", interface,
                                                  uint32_t(0));
    expectMockPropertyAwaitableDefaults<int64_t>(objectPath, "I64", interface,
                                                 int64_t(0));
    expectMockPropertyAwaitableDefaults<uint64_t>(objectPath, "U64", interface,
                                                  uint64_t(0));
    expectMockPropertyAwaitableDefaults<double>(objectPath, "Double", interface,
                                                0.0);
    expectMockPropertyAwaitableDefaults<std::vector<std::string>>(
        objectPath, "Names", interface, {});
    expectMockPropertyAwaitableDefaults<Associations>(
        objectPath, "Associations", interface, {});
    expectMockPropertyAwaitableDefaults<std::vector<uint8_t>>(
        objectPath, "RawData", interface, {});
    expectMockPropertyAwaitableDefaults<std::vector<uint64_t>>(
        objectPath, "Counters", interface, {});
    expectMockPropertyAwaitableDefaults<ObjectPaths>(objectPath, "Paths",
                                                     interface, {});
}

TEST(DBusAsyncUtilsMockCoverage,
     coGetDbusPropertyConstructorBoundaryMatrixInMockMode)
{
    const std::vector<std::size_t> sizes{0, 1, 15, 16, 31, 32, 96};

    for (std::size_t idx = 0; idx < sizes.size(); ++idx)
    {
        const std::string objectPath =
            "/xyz/openbmc_project/test/" + std::string(sizes[idx], 'o');
        const std::string property =
            "Property" + std::string(sizes[(idx + 1) % sizes.size()], 'P');
        const std::string interface =
            "xyz.openbmc_project.Test.Interface." +
            std::string(sizes[(idx + 2) % sizes.size()], 'I');
        const std::string service =
            (idx % 2 == 0)
                ? std::string{}
                : "xyz.openbmc_project.Test.Service." +
                      std::string(sizes[(idx + 3) % sizes.size()], 'S');

        pldm::utils::MockCoverageCoGetDbusProperty<std::string> customServiceOp(
            objectPath, property, interface, service);
        customServiceOp.ret = property + "#" + std::to_string(idx);
        EXPECT_EQ(customServiceOp.objectPath, objectPath);
        EXPECT_EQ(customServiceOp.interface, interface);
        EXPECT_EQ(customServiceOp.property, property);
        EXPECT_EQ(customServiceOp.service, service);
        EXPECT_TRUE(customServiceOp.await_ready());
        EXPECT_TRUE(customServiceOp.await_suspend(std::coroutine_handle<>{}));
        EXPECT_EQ(customServiceOp.await_resume(),
                  property + "#" + std::to_string(idx));

        pldm::utils::MockCoverageCoGetDbusProperty<uint64_t> defaultServiceOp(
            objectPath, property + "U64", interface);
        defaultServiceOp.ret = static_cast<uint64_t>(idx + 1);
        EXPECT_EQ(defaultServiceOp.service, pldm::utils::entityManagerService);
        EXPECT_TRUE(defaultServiceOp.await_ready());
        EXPECT_TRUE(defaultServiceOp.await_suspend(std::coroutine_handle<>{}));
        EXPECT_EQ(defaultServiceOp.await_resume(),
                  static_cast<uint64_t>(idx + 1));
    }
}

TEST(DBusAsyncUtilsMockCoverage, coGetServiceMapAwaitableWorksInMockMode)
{
    const std::string objectPath = "/xyz/openbmc_project/test";
    const pldm::dbus::Interfaces ifaceList{"xyz.openbmc_project.Test"};

    pldm::utils::MockCoverageCoGetServiceMap op(objectPath, ifaceList);
    EXPECT_TRUE(op.await_ready());
    EXPECT_TRUE(op.await_suspend(std::coroutine_handle<>{}));
    EXPECT_TRUE(op.await_resume().empty());
}

TEST(DBusAsyncUtilsMockCoverage,
     coGetServiceMapAndSubTreeConstructorBoundaryMatrixInMockMode)
{
    const std::vector<std::size_t> sizes{0, 1, 15, 16, 31, 32, 96};

    for (std::size_t idx = 0; idx < sizes.size(); ++idx)
    {
        const std::string objectPath =
            "/xyz/openbmc_project/test/" + std::string(sizes[idx], 'm');
        pldm::dbus::Interfaces interfaces;
        if (idx != 0)
        {
            interfaces.emplace_back("xyz.openbmc_project.Test.Interface." +
                                    std::string(sizes[idx], 'A'));
        }
        interfaces.emplace_back(
            "xyz.openbmc_project.Test.Interface." +
            std::string(sizes[(idx + 1) % sizes.size()], 'B'));
        if (idx % 2 == 0)
        {
            interfaces.emplace_back(
                "xyz.openbmc_project.Test.Interface.Extra." +
                std::string(sizes[(idx + 2) % sizes.size()], 'C'));
        }

        pldm::utils::MockCoverageCoGetServiceMap serviceMapOp(
            objectPath, interfaces);
        serviceMapOp.ret = {
            {"xyz.openbmc_project.Test.Service." + std::to_string(idx),
             interfaces}};
        EXPECT_EQ(serviceMapOp.objectPath, objectPath);
        EXPECT_EQ(serviceMapOp.ifaceList, interfaces);
        EXPECT_TRUE(serviceMapOp.await_ready());
        EXPECT_TRUE(serviceMapOp.await_suspend(std::coroutine_handle<>{}));
        EXPECT_EQ(serviceMapOp.await_resume(), serviceMapOp.ret);

        pldm::utils::MockCoverageCoGetSubTree subTreeOp(
            objectPath, static_cast<int>(idx), interfaces);
        subTreeOp.ret = {{objectPath + "/object", serviceMapOp.ret}};
        EXPECT_EQ(subTreeOp.objectPath, objectPath);
        EXPECT_EQ(subTreeOp.depth, static_cast<int>(idx));
        EXPECT_EQ(subTreeOp.ifaceList, interfaces);
        EXPECT_TRUE(subTreeOp.await_ready());
        EXPECT_TRUE(subTreeOp.await_suspend(std::coroutine_handle<>{}));
        EXPECT_EQ(subTreeOp.await_resume(), subTreeOp.ret);
    }
}

TEST(DBusAsyncUtilsMockCoverage,
     coGetDbusPropertyRetainsCtorArgumentsAndCustomServiceInMockMode)
{
    const std::string objectPath =
        "/xyz/openbmc_project/test/" + std::string(40, 'p');
    const std::string property = "Property" + std::string(32, 'N');
    const std::string interface =
        "xyz.openbmc_project.Test." + std::string(24, 'I');
    const std::string service =
        "xyz.openbmc_project.Test." + std::string(20, 'S');

    pldm::utils::MockCoverageCoGetDbusProperty<std::string> op(
        objectPath, property, interface, service);
    op.ret = "mock-value";

    EXPECT_EQ(op.service, service);
    EXPECT_EQ(op.objectPath, objectPath);
    EXPECT_EQ(op.interface, interface);
    EXPECT_EQ(op.property, property);
    EXPECT_TRUE(op.await_ready());
    EXPECT_TRUE(op.await_suspend(std::coroutine_handle<>{}));
    EXPECT_EQ(op.await_resume(), "mock-value");
}

TEST(DBusAsyncUtilsMockCoverage,
     coGetDbusPropertyRetainsCustomServiceAcrossRemainingTypes)
{
    const std::string objectPath =
        "/xyz/openbmc_project/test/" + std::string(28, 'p');
    const std::string interface =
        "xyz.openbmc_project.Test." + std::string(16, 'I');
    const std::string service =
        "xyz.openbmc_project.Test." + std::string(18, 'S');

    expectMockPropertyCtorRetainsCustomService<bool>(objectPath, "Present",
                                                     interface, service, true);
    expectMockPropertyCtorRetainsCustomService<uint8_t>(
        objectPath, "U8", interface, service, uint8_t(9));
    expectMockPropertyCtorRetainsCustomService<int16_t>(
        objectPath, "I16", interface, service, int16_t(-7));
    expectMockPropertyCtorRetainsCustomService<uint16_t>(
        objectPath, "U16", interface, service, uint16_t(11));
    expectMockPropertyCtorRetainsCustomService<int32_t>(
        objectPath, "I32", interface, service, int32_t(-17));
    expectMockPropertyCtorRetainsCustomService<uint32_t>(
        objectPath, "U32", interface, service, uint32_t(19));
    expectMockPropertyCtorRetainsCustomService<int64_t>(
        objectPath, "I64", interface, service, int64_t(-23));
    expectMockPropertyCtorRetainsCustomService<uint64_t>(
        objectPath, "U64", interface, service, uint64_t(29));
    expectMockPropertyCtorRetainsCustomService<double>(
        objectPath, "Double", interface, service, 42.5);
    expectMockPropertyCtorRetainsCustomService<std::vector<std::string>>(
        objectPath, "Names", interface, service, {"alpha", "beta"});
    expectMockPropertyCtorRetainsCustomService<Associations>(
        objectPath, "Associations", interface, service,
        {{"forward", "reverse", "/xyz/openbmc_project/inventory/item0"}});
    expectMockPropertyCtorRetainsCustomService<std::vector<uint8_t>>(
        objectPath, "RawData", interface, service, {0x11, 0x22, 0x33});
    expectMockPropertyCtorRetainsCustomService<std::vector<uint64_t>>(
        objectPath, "Counters", interface, service, {1, 10, 100});
    expectMockPropertyCtorRetainsCustomService<ObjectPaths>(
        objectPath, "Paths", interface, service,
        {sdbusplus::message::object_path("/xyz/openbmc_project/object0"),
         sdbusplus::message::object_path("/xyz/openbmc_project/object1")});
}

TEST(DBusAsyncUtilsMockCoverage,
     coGetDbusPropertyRetainsShortCustomServiceAcrossHeapBackedTypes)
{
    const std::string objectPath = "/x";
    const std::string interface = "i";
    const std::string service = "svc";

    expectMockPropertyCtorRetainsCustomService<std::string>(
        objectPath, "Name", interface, service, "v");
    expectMockPropertyCtorRetainsCustomService<std::vector<std::string>>(
        objectPath, "Names", interface, service, {"a", "b"});
    expectMockPropertyCtorRetainsCustomService<Associations>(
        objectPath, "Associations", interface, service, {{"f", "r", "/p"}});
    expectMockPropertyCtorRetainsCustomService<std::vector<uint8_t>>(
        objectPath, "RawData", interface, service, {0x01, 0x02});
    expectMockPropertyCtorRetainsCustomService<std::vector<uint64_t>>(
        objectPath, "Counters", interface, service, {1, 2});
    expectMockPropertyCtorRetainsCustomService<ObjectPaths>(
        objectPath, "Paths", interface, service,
        {sdbusplus::message::object_path("/p0"),
         sdbusplus::message::object_path("/p1")});
}

TEST(DBusAsyncUtilsMockCoverage,
     coGetDbusPropertyRetainsDefaultServiceAcrossHeapBackedTypesInMockMode)
{
    const std::string objectPath = "/xyz/openbmc_project/test/mock";
    const std::string interface = "xyz.openbmc_project.Test.Mock";

    pldm::utils::MockCoverageCoGetDbusProperty<std::vector<std::string>>
        namesProp(objectPath, "Names", interface);
    namesProp.ret = {"alpha", "beta"};
    EXPECT_EQ(namesProp.service, pldm::utils::entityManagerService);
    EXPECT_EQ(namesProp.await_resume(),
              (std::vector<std::string>{"alpha", "beta"}));

    pldm::utils::MockCoverageCoGetDbusProperty<Associations> associationsProp(
        objectPath, "Associations", interface);
    associationsProp.ret = {{"forward", "reverse", "/xyz/openbmc_project/p0"}};
    EXPECT_EQ(associationsProp.service, pldm::utils::entityManagerService);
    EXPECT_EQ(
        associationsProp.await_resume(),
        (Associations{{"forward", "reverse", "/xyz/openbmc_project/p0"}}));

    pldm::utils::MockCoverageCoGetDbusProperty<std::vector<uint8_t>>
        rawDataProp(objectPath, "RawData", interface);
    rawDataProp.ret = {0x10, 0x20, 0x30};
    EXPECT_EQ(rawDataProp.service, pldm::utils::entityManagerService);
    EXPECT_EQ(rawDataProp.await_resume(),
              (std::vector<uint8_t>{0x10, 0x20, 0x30}));

    pldm::utils::MockCoverageCoGetDbusProperty<std::vector<uint64_t>>
        countersProp(objectPath, "Counters", interface);
    countersProp.ret = {1, 10, 100};
    EXPECT_EQ(countersProp.service, pldm::utils::entityManagerService);
    EXPECT_EQ(countersProp.await_resume(), (std::vector<uint64_t>{1, 10, 100}));

    pldm::utils::MockCoverageCoGetDbusProperty<ObjectPaths> objectPathsProp(
        objectPath, "Paths", interface);
    objectPathsProp.ret = {
        sdbusplus::message::object_path("/xyz/openbmc_project/object0"),
        sdbusplus::message::object_path("/xyz/openbmc_project/object1")};
    EXPECT_EQ(objectPathsProp.service, pldm::utils::entityManagerService);
    EXPECT_EQ(
        objectPathsProp.await_resume(),
        (ObjectPaths{
            sdbusplus::message::object_path("/xyz/openbmc_project/object0"),
            sdbusplus::message::object_path("/xyz/openbmc_project/object1")}));
}

TEST(DBusAsyncUtilsMockCoverage,
     coGetServiceMapRetainsCtorArgumentsAndStoredResultInMockMode)
{
    const std::string objectPath =
        "/xyz/openbmc_project/test/" + std::string(36, 'm');
    const pldm::dbus::Interfaces ifaceList{
        "xyz.openbmc_project.Test.Interface",
        "xyz.openbmc_project.Test." + std::string(18, 'I')};

    pldm::utils::MockCoverageCoGetServiceMap op(objectPath, ifaceList);
    op.ret = {{"xyz.openbmc_project.Test.Service." + std::string(12, 'A'),
               ifaceList}};

    EXPECT_EQ(op.objectPath, objectPath);
    EXPECT_EQ(op.ifaceList, ifaceList);
    EXPECT_TRUE(op.await_ready());
    EXPECT_TRUE(op.await_suspend(std::coroutine_handle<>{}));
    ASSERT_EQ(op.await_resume().size(), 1u);
    EXPECT_EQ(op.await_resume().front().first,
              "xyz.openbmc_project.Test.Service." + std::string(12, 'A'));
}

TEST(DBusAsyncUtilsMockCoverage, coGetSubTreeAwaitableWorksInMockMode)
{
    const std::string objectPath = "/xyz/openbmc_project/test";
    const pldm::dbus::Interfaces ifaceList{"xyz.openbmc_project.Test"};

    pldm::utils::MockCoverageCoGetSubTree op(objectPath, 0, ifaceList);
    EXPECT_TRUE(op.await_ready());
    EXPECT_TRUE(op.await_suspend(std::coroutine_handle<>{}));
    EXPECT_TRUE(op.await_resume().empty());
}

TEST(DBusAsyncUtilsMockCoverage,
     coGetSubTreeRetainsCtorArgumentsAndStoredResultInMockMode)
{
    const std::string objectPath =
        "/xyz/openbmc_project/test/" + std::string(42, 't');
    const pldm::dbus::Interfaces ifaceList{
        "xyz.openbmc_project.Test.Interface",
        "xyz.openbmc_project.Test." + std::string(16, 'J')};

    pldm::utils::MockCoverageCoGetSubTree op(objectPath, 3, ifaceList);
    op.ret = {{"/xyz/openbmc_project/test/object/" + std::string(10, 'x'),
               {{"xyz.openbmc_project.Test.Service", ifaceList}}}};

    EXPECT_EQ(op.objectPath, objectPath);
    EXPECT_EQ(op.depth, 3);
    EXPECT_EQ(op.ifaceList, ifaceList);
    EXPECT_TRUE(op.await_ready());
    EXPECT_TRUE(op.await_suspend(std::coroutine_handle<>{}));
    ASSERT_EQ(op.await_resume().size(), 1u);
    EXPECT_EQ(op.await_resume().front().first,
              "/xyz/openbmc_project/test/object/" + std::string(10, 'x'));
}

TEST(DBusAsyncUtilsMockCoverage,
     coGetServiceMapHandlesLargeStoredResultsAndEmptyInterfacesInMockMode)
{
    const std::string objectPath =
        "/xyz/openbmc_project/test/" + std::string(72, 's');
    const pldm::dbus::Interfaces ifaceList{};
    pldm::utils::MockCoverageCoGetServiceMap op(objectPath, ifaceList);
    op.ret = {{"xyz.openbmc_project.Test.Service." + std::string(28, 'A'),
               {"xyz.openbmc_project.Test.Interface." + std::string(24, 'I')}},
              {"xyz.openbmc_project.Test.Service." + std::string(30, 'B'),
               {"xyz.openbmc_project.Test.Interface." + std::string(26, 'J')}}};

    EXPECT_TRUE(op.await_ready());
    EXPECT_TRUE(op.await_suspend(std::coroutine_handle<>{}));
    auto copy = op.await_resume();
    ASSERT_EQ(copy.size(), 2u);
    EXPECT_EQ(copy, op.ret);
}

TEST(DBusAsyncUtilsMockCoverage,
     coGetSubTreeHandlesLargeStoredResultsAndZeroDepthInMockMode)
{
    const std::string objectPath =
        "/xyz/openbmc_project/test/" + std::string(72, 'q');
    const pldm::dbus::Interfaces ifaceList{};
    pldm::utils::MockCoverageCoGetSubTree op(objectPath, 0, ifaceList);
    op.ret = {
        {"/xyz/openbmc_project/test/object/" + std::string(24, 'x'),
         {{"xyz.openbmc_project.Test.Service." + std::string(18, 'A'),
           {"xyz.openbmc_project.Test.Interface." + std::string(16, 'I')}}}},
        {"/xyz/openbmc_project/test/object/" + std::string(26, 'y'),
         {{"xyz.openbmc_project.Test.Service." + std::string(20, 'B'),
           {"xyz.openbmc_project.Test.Interface." + std::string(18, 'J')}}}}};

    EXPECT_TRUE(op.await_ready());
    EXPECT_TRUE(op.await_suspend(std::coroutine_handle<>{}));
    auto copy = op.await_resume();
    ASSERT_EQ(copy.size(), 2u);
    EXPECT_EQ(copy, op.ret);
}

TEST(DBusAsyncUtilsMockCoverage,
     coGetDbusPropertyExhaustiveBadAllocCoverageInMockMode)
{
    const std::string objectPath =
        "/xyz/openbmc_project/test/" + std::string(120, 'p');
    const std::string property = "Property" + std::string(108, 'N');
    const std::string interface =
        "xyz.openbmc_project.Test." + std::string(104, 'I');
    const std::string service =
        "xyz.openbmc_project.Test." + std::string(112, 'S');

    expectMockPropertyExhaustiveBadAlloc<std::string>(
        objectPath, property + "Name", interface, service,
        std::string(144, 'v'));
    expectMockPropertyExhaustiveBadAlloc<std::vector<std::string>>(
        objectPath, property + "Names", interface, service,
        {std::string(64, 'a'), std::string(68, 'b')});
    expectMockPropertyExhaustiveBadAlloc<Associations>(
        objectPath, property + "Associations", interface, service,
        {{"forward-" + std::string(40, 'f'), "reverse-" + std::string(42, 'r'),
          "/xyz/openbmc_project/inventory/" + std::string(72, 'p')}});
    expectMockPropertyExhaustiveBadAlloc<std::vector<uint8_t>>(
        objectPath, property + "RawData", interface, service,
        std::vector<uint8_t>(96, 0x5A));
    expectMockPropertyExhaustiveBadAlloc<std::vector<uint64_t>>(
        objectPath, property + "Counters", interface, service,
        {1, 10, 100, 1000, 10000, 100000});
    expectMockPropertyExhaustiveBadAlloc<ObjectPaths>(
        objectPath, property + "Paths", interface, service,
        {sdbusplus::message::object_path(
             "/xyz/openbmc_project/object/" + std::string(52, 'x')),
         sdbusplus::message::object_path(
             "/xyz/openbmc_project/object/" + std::string(56, 'y'))});
}

TEST(
    DBusAsyncUtilsMockCoverage,
    coGetDbusPropertyExhaustiveBadAllocCoverageForRemainingScalarTypesInMockMode)
{
    const std::string objectPath =
        "/xyz/openbmc_project/test/" + std::string(112, 's');
    const std::string property = "Scalar" + std::string(104, 'P');
    const std::string interface =
        "xyz.openbmc_project.Test." + std::string(108, 'I');
    const std::string service =
        "xyz.openbmc_project.Test." + std::string(116, 'S');

    expectMockPropertyExhaustiveBadAlloc<bool>(objectPath, property + "Bool",
                                               interface, service, true);
    expectMockPropertyExhaustiveBadAlloc<uint8_t>(
        objectPath, property + "U8", interface, service,
        static_cast<uint8_t>(7));
    expectMockPropertyExhaustiveBadAlloc<int16_t>(
        objectPath, property + "I16", interface, service,
        static_cast<int16_t>(-17));
    expectMockPropertyExhaustiveBadAlloc<uint16_t>(
        objectPath, property + "U16", interface, service,
        static_cast<uint16_t>(23));
    expectMockPropertyExhaustiveBadAlloc<int32_t>(
        objectPath, property + "I32", interface, service,
        static_cast<int32_t>(-101));
    expectMockPropertyExhaustiveBadAlloc<uint32_t>(
        objectPath, property + "U32", interface, service,
        static_cast<uint32_t>(101));
    expectMockPropertyExhaustiveBadAlloc<int64_t>(
        objectPath, property + "I64", interface, service,
        static_cast<int64_t>(-1001));
    expectMockPropertyExhaustiveBadAlloc<double>(
        objectPath, property + "Double", interface, service, 12.5);
}

TEST(DBusAsyncUtilsMockCoverage,
     coGetDbusPropertyDefaultServiceCtorExhaustiveBadAllocCoverageInMockMode)
{
    const std::string objectPath =
        "/xyz/openbmc_project/test/" + std::string(120, 'd');
    const std::string property = "Default" + std::string(108, 'P');
    const std::string interface =
        "xyz.openbmc_project.Test." + std::string(104, 'I');

    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc([&] {
        pldm::utils::MockCoverageCoGetDbusProperty<std::string> op(
            objectPath, property + "Name", interface);
        (void)op;
    }));
    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc([&] {
        pldm::utils::MockCoverageCoGetDbusProperty<std::vector<std::string>> op(
            objectPath, property + "Names", interface);
        (void)op;
    }));
    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc([&] {
        pldm::utils::MockCoverageCoGetDbusProperty<Associations> op(
            objectPath, property + "Associations", interface);
        (void)op;
    }));
    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc([&] {
        pldm::utils::MockCoverageCoGetDbusProperty<ObjectPaths> op(
            objectPath, property + "Paths", interface);
        (void)op;
    }));
}

TEST(DBusAsyncUtilsMockCoverage,
     coGetDbusPropertyRetainsDefaultServiceAcrossRemainingScalarTypesInMockMode)
{
    const std::string objectPath = "/xyz/openbmc_project/test/mock/scalars";
    const std::string interface = "xyz.openbmc_project.Test.Mock.Scalar";

    pldm::utils::MockCoverageCoGetDbusProperty<std::string> nameProp(
        objectPath, "Name", interface);
    nameProp.ret = "mock-default";
    EXPECT_EQ(nameProp.service, pldm::utils::entityManagerService);
    EXPECT_EQ(nameProp.await_resume(), "mock-default");

    pldm::utils::MockCoverageCoGetDbusProperty<bool> boolProp(
        objectPath, "Present", interface);
    boolProp.ret = true;
    EXPECT_EQ(boolProp.service, pldm::utils::entityManagerService);
    EXPECT_TRUE(boolProp.await_resume());

    pldm::utils::MockCoverageCoGetDbusProperty<uint8_t> u8Prop(
        objectPath, "U8", interface);
    u8Prop.ret = static_cast<uint8_t>(7);
    EXPECT_EQ(u8Prop.service, pldm::utils::entityManagerService);
    EXPECT_EQ(u8Prop.await_resume(), static_cast<uint8_t>(7));

    pldm::utils::MockCoverageCoGetDbusProperty<int16_t> i16Prop(
        objectPath, "I16", interface);
    i16Prop.ret = static_cast<int16_t>(-17);
    EXPECT_EQ(i16Prop.service, pldm::utils::entityManagerService);
    EXPECT_EQ(i16Prop.await_resume(), static_cast<int16_t>(-17));

    pldm::utils::MockCoverageCoGetDbusProperty<uint16_t> u16Prop(
        objectPath, "U16", interface);
    u16Prop.ret = static_cast<uint16_t>(23);
    EXPECT_EQ(u16Prop.service, pldm::utils::entityManagerService);
    EXPECT_EQ(u16Prop.await_resume(), static_cast<uint16_t>(23));

    pldm::utils::MockCoverageCoGetDbusProperty<int32_t> i32Prop(
        objectPath, "I32", interface);
    i32Prop.ret = static_cast<int32_t>(-101);
    EXPECT_EQ(i32Prop.service, pldm::utils::entityManagerService);
    EXPECT_EQ(i32Prop.await_resume(), static_cast<int32_t>(-101));

    pldm::utils::MockCoverageCoGetDbusProperty<uint32_t> u32Prop(
        objectPath, "U32", interface);
    u32Prop.ret = static_cast<uint32_t>(101);
    EXPECT_EQ(u32Prop.service, pldm::utils::entityManagerService);
    EXPECT_EQ(u32Prop.await_resume(), static_cast<uint32_t>(101));

    pldm::utils::MockCoverageCoGetDbusProperty<int64_t> i64Prop(
        objectPath, "I64", interface);
    i64Prop.ret = static_cast<int64_t>(-1001);
    EXPECT_EQ(i64Prop.service, pldm::utils::entityManagerService);
    EXPECT_EQ(i64Prop.await_resume(), static_cast<int64_t>(-1001));

    pldm::utils::MockCoverageCoGetDbusProperty<uint64_t> u64Prop(
        objectPath, "U64", interface);
    u64Prop.ret = static_cast<uint64_t>(1001);
    EXPECT_EQ(u64Prop.service, pldm::utils::entityManagerService);
    EXPECT_EQ(u64Prop.await_resume(), static_cast<uint64_t>(1001));

    pldm::utils::MockCoverageCoGetDbusProperty<double> doubleProp(
        objectPath, "Double", interface);
    doubleProp.ret = 12.5;
    EXPECT_EQ(doubleProp.service, pldm::utils::entityManagerService);
    EXPECT_DOUBLE_EQ(doubleProp.await_resume(), 12.5);
}

TEST(
    DBusAsyncUtilsMockCoverage,
    coGetDbusPropertyDefaultServiceCtorExhaustiveBadAllocCoverageForAdditionalTypesInMockMode)
{
    const std::string objectPath =
        "/xyz/openbmc_project/test/" + std::string(136, 'm');
    const std::string property = "Additional" + std::string(112, 'Q');
    const std::string interface =
        "xyz.openbmc_project.Test." + std::string(110, 'J');

    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc([&] {
        pldm::utils::MockCoverageCoGetDbusProperty<bool> op(
            objectPath, property + "Bool", interface);
        (void)op;
    }));
    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc([&] {
        pldm::utils::MockCoverageCoGetDbusProperty<uint8_t> op(
            objectPath, property + "U8", interface);
        (void)op;
    }));
    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc([&] {
        pldm::utils::MockCoverageCoGetDbusProperty<int16_t> op(
            objectPath, property + "I16", interface);
        (void)op;
    }));
    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc([&] {
        pldm::utils::MockCoverageCoGetDbusProperty<uint16_t> op(
            objectPath, property + "U16", interface);
        (void)op;
    }));
    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc([&] {
        pldm::utils::MockCoverageCoGetDbusProperty<int32_t> op(
            objectPath, property + "I32", interface);
        (void)op;
    }));
    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc([&] {
        pldm::utils::MockCoverageCoGetDbusProperty<uint32_t> op(
            objectPath, property + "U32", interface);
        (void)op;
    }));
    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc([&] {
        pldm::utils::MockCoverageCoGetDbusProperty<int64_t> op(
            objectPath, property + "I64", interface);
        (void)op;
    }));
    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc([&] {
        pldm::utils::MockCoverageCoGetDbusProperty<uint64_t> op(
            objectPath, property + "U64", interface);
        (void)op;
    }));
    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc([&] {
        pldm::utils::MockCoverageCoGetDbusProperty<double> op(
            objectPath, property + "Double", interface);
        (void)op;
    }));
    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc([&] {
        pldm::utils::MockCoverageCoGetDbusProperty<std::vector<uint8_t>> op(
            objectPath, property + "RawData", interface);
        (void)op;
    }));
    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc([&] {
        pldm::utils::MockCoverageCoGetDbusProperty<std::vector<uint64_t>> op(
            objectPath, property + "Counters", interface);
        (void)op;
    }));
}

TEST(DBusAsyncUtilsMockCoverage,
     coGetDbusPropertyDeepBadAllocCoverageInMockMode)
{
    const std::string objectPath =
        "/xyz/openbmc_project/test/" + std::string(180, 'd');
    const std::string property = "Deep" + std::string(152, 'P');
    const std::string interface =
        "xyz.openbmc_project.Test." + std::string(148, 'I');
    const std::string service =
        "xyz.openbmc_project.Test." + std::string(160, 'S');

    expectMockPropertyExhaustiveBadAlloc<std::string>(
        objectPath, property + "Name", interface, service,
        std::string(196, 'v'), 2048);
    expectMockPropertyExhaustiveBadAlloc<Associations>(
        objectPath, property + "Associations", interface, service,
        {{"forward-" + std::string(48, 'f'), "reverse-" + std::string(52, 'r'),
          "/xyz/openbmc_project/inventory/" + std::string(124, 'p')}},
        2048);
    expectMockPropertyExhaustiveBadAlloc<ObjectPaths>(
        objectPath, property + "Paths", interface, service,
        {sdbusplus::message::object_path(
             "/xyz/openbmc_project/object/" + std::string(120, 'x')),
         sdbusplus::message::object_path(
             "/xyz/openbmc_project/object/" + std::string(128, 'y'))},
        2048);
}

TEST(DBusAsyncUtilsMockCoverage,
     coGetDbusPropertyDefaultServiceCtorDeepBadAllocCoverageInMockMode)
{
    const std::string objectPath =
        "/xyz/openbmc_project/test/" + std::string(188, 'm');
    const std::string property = "MockDefault" + std::string(156, 'Q');
    const std::string interface =
        "xyz.openbmc_project.Test." + std::string(150, 'J');

    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::MockCoverageCoGetDbusProperty<std::string> op(
                objectPath, property + "Name", interface);
            (void)op;
        },
        2048));
    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::MockCoverageCoGetDbusProperty<Associations> op(
                objectPath, property + "Associations", interface);
            (void)op;
        },
        2048));
    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::MockCoverageCoGetDbusProperty<ObjectPaths> op(
                objectPath, property + "Paths", interface);
            (void)op;
        },
        2048));
}

TEST(DBusAsyncUtilsMockCoverage,
     coGetDbusPropertyRemainingCtorDeepBadAllocCoverageInMockMode)
{
    const std::string objectPath =
        "/xyz/openbmc_project/test/" + std::string(196, 'p');
    const std::string property = "MockCtor" + std::string(160, 'R');
    const std::string interface =
        "xyz.openbmc_project.Test." + std::string(146, 'K');
    const std::string service =
        "xyz.openbmc_project.Test.Service." + std::string(150, 'S');

    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::MockCoverageCoGetDbusProperty<bool> op(
                objectPath, property + "Bool", interface, service);
            (void)op;
        },
        2048));
    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::MockCoverageCoGetDbusProperty<uint64_t> op(
                objectPath, property + "Uint64", interface, service);
            (void)op;
        },
        2048));
    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::MockCoverageCoGetDbusProperty<double> op(
                objectPath, property + "Double", interface);
            (void)op;
        },
        2048));
    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::MockCoverageCoGetDbusProperty<std::vector<uint8_t>> op(
                objectPath, property + "Bytes", interface, service);
            (void)op;
        },
        2048));
    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::MockCoverageCoGetDbusProperty<std::vector<uint64_t>>
                op(objectPath, property + "Counters", interface);
            (void)op;
        },
        2048));
    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::MockCoverageCoGetDbusProperty<std::vector<std::string>>
                op(objectPath, property + "Names", interface, service);
            (void)op;
        },
        2048));
}

TEST(DBusAsyncUtilsMockCoverage,
     coGetServiceMapAndSubTreeDeepBadAllocCoverageInMockMode)
{
    const std::string objectPath =
        "/xyz/openbmc_project/test/" + std::string(168, 's');
    const pldm::dbus::Interfaces ifaceList{
        "xyz.openbmc_project.Test.Interface." + std::string(90, 'A'),
        "xyz.openbmc_project.Test.Interface." + std::string(94, 'B')};
    const pldm::utils::MapperServiceMap serviceMap{
        {"xyz.openbmc_project.Test.Service." + std::string(86, 'C'), ifaceList},
        {"xyz.openbmc_project.Test.Service." + std::string(90, 'D'),
         {"xyz.openbmc_project.Test.Interface." + std::string(82, 'E')}}};
    const pldm::utils::GetSubTreeResponse subTree{
        {"/xyz/openbmc_project/test/object/" + std::string(96, 'x'),
         {{"xyz.openbmc_project.Test.Service." + std::string(84, 'F'),
           ifaceList}}},
        {"/xyz/openbmc_project/test/object/" + std::string(100, 'y'),
         {{"xyz.openbmc_project.Test.Service." + std::string(88, 'G'),
           {"xyz.openbmc_project.Test.Interface." + std::string(80, 'H'),
            "xyz.openbmc_project.Test.Interface." + std::string(84, 'I')}}}}};

    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::MockCoverageCoGetServiceMap op(objectPath, ifaceList);
            op.ret = serviceMap;
        },
        2048));
    EXPECT_TRUE(async_utils_mock_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::MockCoverageCoGetSubTree op(objectPath, 5, ifaceList);
            op.ret = subTree;
        },
        2048));
}

} // namespace
