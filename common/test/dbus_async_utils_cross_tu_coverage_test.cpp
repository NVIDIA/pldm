#include "../../test/test_valgrind_utils.hpp"
#include "../utils.hpp"

#include <boost/system/errc.hpp>
#include <boost/system/error_code.hpp>

#include <coroutine>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

namespace async_utils_cross_tu
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

struct ScopedAllocationPause
{
    ScopedAllocationPause() :
        previousFailAllocations(failAllocations),
        previousFailAtAllocation(failAtAllocation),
        previousAllocationCount(allocationCount)
    {
        failAllocations = false;
    }

    ~ScopedAllocationPause()
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

class FakeAsioConnection
{
  public:
    pldm::utils::PropertyValue nextPropertyValue{std::string{}};
    boost::system::error_code nextPropertyError{};
    pldm::utils::MapperServiceMap nextServiceMap{};
    boost::system::error_code nextServiceMapError{};
    pldm::utils::GetSubTreeResponse nextSubTree{};
    boost::system::error_code nextSubTreeError{};
    bool throwOnPropertyCall = false;
    bool throwOnServiceMapCall = false;
    bool throwOnSubTreeCall = false;

    template <typename Callback, typename... Args>
    void async_method_call(Callback&& cb, Args&&...)
    {
        if constexpr (std::is_invocable_v<Callback, boost::system::error_code,
                                          pldm::utils::PropertyValue>)
        {
            if (throwOnPropertyCall)
            {
                throw std::runtime_error("property async call failure");
            }
            pldm::utils::PropertyValue valueCopy;
            {
                ScopedAllocationPause pause;
                valueCopy = nextPropertyValue;
            }
            cb(nextPropertyError, std::move(valueCopy));
        }
        else if constexpr (std::is_invocable_v<Callback,
                                               boost::system::error_code,
                                               pldm::utils::MapperServiceMap>)
        {
            if (throwOnServiceMapCall)
            {
                throw std::runtime_error("service map async call failure");
            }
            pldm::utils::MapperServiceMap valueCopy;
            {
                ScopedAllocationPause pause;
                valueCopy = nextServiceMap;
            }
            cb(nextServiceMapError, std::move(valueCopy));
        }
        else if constexpr (std::is_invocable_v<Callback,
                                               boost::system::error_code,
                                               pldm::utils::GetSubTreeResponse>)
        {
            if (throwOnSubTreeCall)
            {
                throw std::runtime_error("subtree async call failure");
            }
            pldm::utils::GetSubTreeResponse valueCopy;
            {
                ScopedAllocationPause pause;
                valueCopy = nextSubTree;
            }
            cb(nextSubTreeError, std::move(valueCopy));
        }
    }
};

} // namespace async_utils_cross_tu

void* operator new(std::size_t size)
{
    return async_utils_cross_tu::allocate(size);
}

void* operator new[](std::size_t size)
{
    return async_utils_cross_tu::allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    return async_utils_cross_tu::allocate(size,
                                          static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return async_utils_cross_tu::allocate(size,
                                          static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return async_utils_cross_tu::allocate(size);
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
        return async_utils_cross_tu::allocate(size);
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

namespace pldm::utils
{

class DBusAsyncUtilsCrossTuMockHandler : public DBusHandler
{
  public:
    static std::shared_ptr<async_utils_cross_tu::FakeAsioConnection>&
        fakeConnection()
    {
        static auto conn =
            std::make_shared<async_utils_cross_tu::FakeAsioConnection>();
        return conn;
    }

    static auto& getAsioConnection()
    {
        return fakeConnection();
    }
};

} // namespace pldm::utils

#define DBusHandler DBusAsyncUtilsCrossTuMockHandler
#include "../dBusAsyncUtils.hpp"
#undef DBusHandler

namespace
{

using Associations =
    std::vector<std::tuple<std::string, std::string, std::string>>;
using ObjectPaths = std::vector<sdbusplus::message::object_path>;

class DBusAsyncUtilsCrossTuTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        auto& conn =
            *pldm::utils::DBusAsyncUtilsCrossTuMockHandler::fakeConnection();
        conn.nextPropertyValue = std::string{};
        conn.nextPropertyError.clear();
        conn.nextServiceMap.clear();
        conn.nextServiceMapError.clear();
        conn.nextSubTree.clear();
        conn.nextSubTreeError.clear();
        conn.throwOnPropertyCall = false;
        conn.throwOnServiceMapCall = false;
        conn.throwOnSubTreeCall = false;
    }

    auto& connection()
    {
        return *pldm::utils::DBusAsyncUtilsCrossTuMockHandler::fakeConnection();
    }
};

TEST_F(DBusAsyncUtilsCrossTuTest, PropertyAwaitableCoversSupportedTypesInNewTu)
{
    constexpr auto path = "/xyz/openbmc_project/test/object";
    constexpr auto iface = "xyz.openbmc_project.Test.Interface";
    auto& conn = connection();

    conn.nextPropertyValue = std::string("service-name");
    pldm::utils::coGetDbusProperty<std::string> stringProp(path, "Name", iface);
    EXPECT_FALSE(stringProp.await_ready());
    EXPECT_TRUE(stringProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(stringProp.await_resume(), "service-name");

    conn.nextPropertyValue = uint64_t(42);
    pldm::utils::coGetDbusProperty<uint64_t> numericProp(path, "Bus", iface);
    EXPECT_TRUE(numericProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(numericProp.await_resume(), uint64_t(42));

    conn.nextPropertyValue = true;
    pldm::utils::coGetDbusProperty<bool> boolProp(path, "Present", iface);
    EXPECT_TRUE(boolProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(boolProp.await_resume());

    conn.nextPropertyValue = static_cast<uint8_t>(7);
    pldm::utils::coGetDbusProperty<uint8_t> u8Prop(path, "U8", iface);
    EXPECT_TRUE(u8Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(u8Prop.await_resume(), static_cast<uint8_t>(7));

    conn.nextPropertyValue = static_cast<int16_t>(-3);
    pldm::utils::coGetDbusProperty<int16_t> i16Prop(path, "I16", iface);
    EXPECT_TRUE(i16Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(i16Prop.await_resume(), static_cast<int16_t>(-3));

    conn.nextPropertyValue = static_cast<uint16_t>(17);
    pldm::utils::coGetDbusProperty<uint16_t> u16Prop(path, "U16", iface);
    EXPECT_TRUE(u16Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(u16Prop.await_resume(), static_cast<uint16_t>(17));

    conn.nextPropertyValue = static_cast<int32_t>(-31);
    pldm::utils::coGetDbusProperty<int32_t> i32Prop(path, "I32", iface);
    EXPECT_TRUE(i32Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(i32Prop.await_resume(), static_cast<int32_t>(-31));

    conn.nextPropertyValue = static_cast<uint32_t>(41);
    pldm::utils::coGetDbusProperty<uint32_t> u32Prop(path, "U32", iface);
    EXPECT_TRUE(u32Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(u32Prop.await_resume(), static_cast<uint32_t>(41));

    conn.nextPropertyValue = static_cast<int64_t>(-53);
    pldm::utils::coGetDbusProperty<int64_t> i64Prop(path, "I64", iface);
    EXPECT_TRUE(i64Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(i64Prop.await_resume(), static_cast<int64_t>(-53));

    conn.nextPropertyValue = 42.5;
    pldm::utils::coGetDbusProperty<double> doubleProp(path, "Double", iface);
    EXPECT_TRUE(doubleProp.await_suspend(std::noop_coroutine()));
    EXPECT_DOUBLE_EQ(doubleProp.await_resume(), 42.5);

    const std::vector<std::string> parents{"/xyz/openbmc_project/parent0",
                                           "/xyz/openbmc_project/parent1"};
    conn.nextPropertyValue = parents;
    pldm::utils::coGetDbusProperty<std::vector<std::string>> parentsProp(
        path, "Parents", iface);
    EXPECT_TRUE(parentsProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(parentsProp.await_resume(), parents);

    const Associations associations{
        {"chassis", "all_states", "/xyz/openbmc_project/inventory/chassis0"}};
    conn.nextPropertyValue = associations;
    pldm::utils::coGetDbusProperty<Associations> associationsProp(
        path, "Associations", iface);
    EXPECT_TRUE(associationsProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(associationsProp.await_resume(), associations);

    const std::vector<uint8_t> rawData{0x11, 0x22, 0x33};
    conn.nextPropertyValue = rawData;
    pldm::utils::coGetDbusProperty<std::vector<uint8_t>> rawDataProp(
        path, "RawData", iface);
    EXPECT_TRUE(rawDataProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(rawDataProp.await_resume(), rawData);

    const std::vector<uint64_t> counters{1, 10, 100};
    conn.nextPropertyValue = counters;
    pldm::utils::coGetDbusProperty<std::vector<uint64_t>> countersProp(
        path, "Counters", iface);
    EXPECT_TRUE(countersProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(countersProp.await_resume(), counters);

    const ObjectPaths objectPaths{
        sdbusplus::message::object_path("/xyz/openbmc_project/object0"),
        sdbusplus::message::object_path("/xyz/openbmc_project/object1")};
    conn.nextPropertyValue = objectPaths;
    pldm::utils::coGetDbusProperty<ObjectPaths> objectPathsProp(
        path, "ObjectPaths", iface);
    EXPECT_TRUE(objectPathsProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(objectPathsProp.await_resume(), objectPaths);
}

TEST_F(DBusAsyncUtilsCrossTuTest,
       PropertyAwaitableCoversErrorAndVariantMismatchPathsInNewTu)
{
    constexpr auto path = "/xyz/openbmc_project/test/object";
    constexpr auto iface = "xyz.openbmc_project.Test.Interface";
    auto& conn = connection();

    conn.nextPropertyError =
        boost::system::errc::make_error_code(boost::system::errc::io_error);

    pldm::utils::coGetDbusProperty<std::string> stringProp(path, "Name", iface);
    EXPECT_TRUE(stringProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(stringProp.await_resume().empty());

    pldm::utils::coGetDbusProperty<uint64_t> numericProp(path, "Bus", iface);
    EXPECT_TRUE(numericProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(numericProp.await_resume(), uint64_t(0));

    pldm::utils::coGetDbusProperty<std::vector<std::string>> parentsProp(
        path, "Parents", iface);
    EXPECT_TRUE(parentsProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(parentsProp.await_resume().empty());

    conn.nextPropertyError.clear();
    conn.nextPropertyValue = uint64_t(99);
    pldm::utils::coGetDbusProperty<std::string> badString(path, "Name", iface);
    EXPECT_THROW(badString.await_suspend(std::noop_coroutine()),
                 std::bad_variant_access);

    conn.nextPropertyValue = std::string("wrong");
    pldm::utils::coGetDbusProperty<uint64_t> badNumeric(path, "Bus", iface);
    EXPECT_THROW(badNumeric.await_suspend(std::noop_coroutine()),
                 std::bad_variant_access);

    conn.nextPropertyValue = std::vector<uint8_t>{0x11, 0x22};
    pldm::utils::coGetDbusProperty<std::vector<uint64_t>> badCounters(
        path, "Counters", iface);
    EXPECT_THROW(badCounters.await_suspend(std::noop_coroutine()),
                 std::bad_variant_access);

    conn.throwOnPropertyCall = true;
    pldm::utils::coGetDbusProperty<std::string> throwingProp(
        path, "Name", iface);
    EXPECT_THROW(throwingProp.await_suspend(std::noop_coroutine()),
                 std::runtime_error);
}

TEST_F(DBusAsyncUtilsCrossTuTest,
       ServiceMapAndSubTreeAwaitablesCoverSuccessAndErrorPathsInNewTu)
{
    auto& conn = connection();
    const std::string path = "/xyz/openbmc_project/test/object";
    const pldm::dbus::Interfaces ifaces{"xyz.openbmc_project.Test.Interface"};

    const pldm::utils::MapperServiceMap expectedServiceMap{
        {"xyz.openbmc_project.Test.Service", ifaces}};
    conn.nextServiceMap = expectedServiceMap;

    pldm::utils::coGetServiceMap serviceMap(path, ifaces);
    EXPECT_FALSE(serviceMap.await_ready());
    EXPECT_TRUE(serviceMap.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(serviceMap.await_resume(), expectedServiceMap);

    conn.nextServiceMapError = boost::system::errc::make_error_code(
        boost::system::errc::no_such_file_or_directory);
    pldm::utils::coGetServiceMap serviceMapError(path, ifaces);
    EXPECT_TRUE(serviceMapError.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(serviceMapError.await_resume().empty());

    conn.nextServiceMapError.clear();
    conn.throwOnServiceMapCall = true;
    pldm::utils::coGetServiceMap throwingServiceMap(path, ifaces);
    EXPECT_DEATH(
        { throwingServiceMap.await_suspend(std::noop_coroutine()); },
        "service map async call failure");

    const pldm::utils::GetSubTreeResponse expectedSubTree{
        {"/xyz/openbmc_project/test/object", expectedServiceMap}};
    conn.throwOnServiceMapCall = false;
    conn.nextSubTree = expectedSubTree;

    pldm::utils::coGetSubTree subTree("/xyz/openbmc_project/test", 1, ifaces);
    EXPECT_FALSE(subTree.await_ready());
    EXPECT_TRUE(subTree.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(subTree.await_resume(), expectedSubTree);

    conn.nextSubTreeError = boost::system::errc::make_error_code(
        boost::system::errc::host_unreachable);
    pldm::utils::coGetSubTree subTreeError("/xyz/openbmc_project/test", 1,
                                           ifaces);
    EXPECT_TRUE(subTreeError.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(subTreeError.await_resume().empty());

    conn.nextSubTreeError.clear();
    conn.throwOnSubTreeCall = true;
    pldm::utils::coGetSubTree throwingSubTree("/xyz/openbmc_project/test", 1,
                                              ifaces);
    EXPECT_DEATH(
        { throwingSubTree.await_suspend(std::noop_coroutine()); },
        "subtree async call failure");
}

TEST_F(DBusAsyncUtilsCrossTuTest,
       ConstructorAndBadAllocCoverageExercisesFreshInstantiationPaths)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(24, 'p');
    const std::string iface =
        "xyz.openbmc_project.Test." + std::string(18, 'I');
    const std::string customService =
        "xyz.openbmc_project.Custom." + std::string(18, 'S');
    auto& conn = connection();

    pldm::utils::coGetDbusProperty<std::vector<std::string>> heapProp(
        path, "Names", iface);
    EXPECT_EQ(heapProp.service, pldm::utils::entityManagerService);
    EXPECT_EQ(heapProp.objectPath, path);
    EXPECT_EQ(heapProp.interface, iface);
    EXPECT_EQ(heapProp.property, "Names");
    EXPECT_TRUE(heapProp.await_resume().empty());

    pldm::utils::coGetDbusProperty<std::string> customProp(
        path, "Name", iface, customService);
    EXPECT_EQ(customProp.service, customService);
    EXPECT_EQ(customProp.objectPath, path);
    EXPECT_EQ(customProp.interface, iface);
    EXPECT_EQ(customProp.property, "Name");

    EXPECT_TRUE(async_utils_cross_tu::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<std::vector<std::string>> prop(
            path, "Names", iface, customService);
        (void)prop;
    }));

    EXPECT_TRUE(async_utils_cross_tu::exerciseAllBadAlloc([&] {
        conn.nextPropertyValue = std::vector<std::string>{"one", "two"};
        pldm::utils::coGetDbusProperty<std::vector<std::string>> prop(
            path, "Names", iface, customService);
        (void)prop.await_suspend(std::noop_coroutine());
    }));

    EXPECT_TRUE(async_utils_cross_tu::exerciseAllBadAlloc([&] {
        const pldm::dbus::Interfaces ifaces{
            "xyz.openbmc_project.Test.Interface"};
        pldm::utils::coGetServiceMap serviceMap(path, ifaces);
        (void)serviceMap;
    }));

    EXPECT_TRUE(async_utils_cross_tu::exerciseAllBadAlloc([&] {
        const pldm::dbus::Interfaces ifaces{
            "xyz.openbmc_project.Test.Interface"};
        pldm::utils::coGetSubTree subTree(path, 1, ifaces);
        (void)subTree;
    }));
}

TEST_F(DBusAsyncUtilsCrossTuTest,
       ContainerTypeCoverageAndBadAllocPathsInFreshTu)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(32, 'c');
    const std::string iface =
        "xyz.openbmc_project.Test." + std::string(20, 'A');
    const std::string customService =
        "xyz.openbmc_project.Custom." + std::string(20, 'B');
    auto& conn = connection();

    const Associations associations{
        {"chassis", "all_sensors", "/xyz/openbmc_project/inventory/chassis0"},
        {"chassis", "all_sensors", "/xyz/openbmc_project/inventory/chassis1"}};
    const ObjectPaths objectPaths{
        sdbusplus::message::object_path("/xyz/openbmc_project/object0"),
        sdbusplus::message::object_path("/xyz/openbmc_project/object1")};
    const std::vector<uint64_t> counters{7, 8, 9};
    const std::vector<uint8_t> rawBytes{0x10, 0x20, 0x30};

    conn.nextPropertyValue = associations;
    pldm::utils::coGetDbusProperty<Associations> associationsProp(
        path, "Associations", iface, customService);
    EXPECT_TRUE(associationsProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(associationsProp.await_resume(), associations);

    conn.nextPropertyValue = objectPaths;
    pldm::utils::coGetDbusProperty<ObjectPaths> objectPathsProp(
        path, "ObjectPaths", iface, customService);
    EXPECT_TRUE(objectPathsProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(objectPathsProp.await_resume(), objectPaths);

    conn.nextPropertyValue = counters;
    pldm::utils::coGetDbusProperty<std::vector<uint64_t>> countersProp(
        path, "Counters", iface, customService);
    EXPECT_TRUE(countersProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(countersProp.await_resume(), counters);

    conn.nextPropertyValue = rawBytes;
    pldm::utils::coGetDbusProperty<std::vector<uint8_t>> bytesProp(
        path, "Bytes", iface, customService);
    EXPECT_TRUE(bytesProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(bytesProp.await_resume(), rawBytes);

    conn.nextPropertyError =
        boost::system::errc::make_error_code(boost::system::errc::io_error);

    pldm::utils::coGetDbusProperty<Associations> associationsError(
        path, "Associations", iface, customService);
    EXPECT_TRUE(associationsError.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(associationsError.await_resume().empty());

    pldm::utils::coGetDbusProperty<ObjectPaths> objectPathsError(
        path, "ObjectPaths", iface, customService);
    EXPECT_TRUE(objectPathsError.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(objectPathsError.await_resume().empty());

    pldm::utils::coGetDbusProperty<std::vector<uint64_t>> countersError(
        path, "Counters", iface, customService);
    EXPECT_TRUE(countersError.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(countersError.await_resume().empty());

    pldm::utils::coGetDbusProperty<std::vector<uint8_t>> bytesError(
        path, "Bytes", iface, customService);
    EXPECT_TRUE(bytesError.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(bytesError.await_resume().empty());

    conn.nextPropertyError.clear();
    conn.nextPropertyValue = objectPaths;
    pldm::utils::coGetDbusProperty<Associations> badAssociations(
        path, "Associations", iface, customService);
    EXPECT_THROW(badAssociations.await_suspend(std::noop_coroutine()),
                 std::bad_variant_access);

    conn.nextPropertyValue = associations;
    pldm::utils::coGetDbusProperty<ObjectPaths> badObjectPaths(
        path, "ObjectPaths", iface, customService);
    EXPECT_THROW(badObjectPaths.await_suspend(std::noop_coroutine()),
                 std::bad_variant_access);

    {
        async_utils_cross_tu::ScopedAllocationPause pause;
        conn.nextPropertyValue = associations;
    }
    EXPECT_TRUE(async_utils_cross_tu::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<Associations> prop(path, "Associations",
                                                          iface, customService);
        (void)prop;
    }));
    EXPECT_TRUE(async_utils_cross_tu::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<Associations> prop(path, "Associations",
                                                          iface, customService);
        (void)prop.await_suspend(std::noop_coroutine());
    }));

    {
        async_utils_cross_tu::ScopedAllocationPause pause;
        conn.nextPropertyValue = objectPaths;
    }
    EXPECT_TRUE(async_utils_cross_tu::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<ObjectPaths> prop(path, "ObjectPaths",
                                                         iface, customService);
        (void)prop;
    }));
    EXPECT_TRUE(async_utils_cross_tu::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<ObjectPaths> prop(path, "ObjectPaths",
                                                         iface, customService);
        (void)prop.await_suspend(std::noop_coroutine());
    }));

    {
        async_utils_cross_tu::ScopedAllocationPause pause;
        conn.nextPropertyValue = counters;
    }
    EXPECT_TRUE(async_utils_cross_tu::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<std::vector<uint64_t>> prop(
            path, "Counters", iface, customService);
        (void)prop;
    }));
    EXPECT_TRUE(async_utils_cross_tu::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<std::vector<uint64_t>> prop(
            path, "Counters", iface, customService);
        (void)prop.await_suspend(std::noop_coroutine());
    }));

    {
        async_utils_cross_tu::ScopedAllocationPause pause;
        conn.nextPropertyValue = rawBytes;
    }
    EXPECT_TRUE(async_utils_cross_tu::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<std::vector<uint8_t>> prop(
            path, "Bytes", iface, customService);
        (void)prop;
    }));
    EXPECT_TRUE(async_utils_cross_tu::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<std::vector<uint8_t>> prop(
            path, "Bytes", iface, customService);
        (void)prop.await_suspend(std::noop_coroutine());
    }));
}

} // namespace
