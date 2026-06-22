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

namespace async_utils_test
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
bool exerciseBadAlloc(Operation&& operation, std::size_t maxFailAt = 128)
{
    if (pldm::test::runningOnValgrind())
    {
        return true;
    }

    for (std::size_t failIndex = 1; failIndex <= maxFailAt; ++failIndex)
    {
        try
        {
            ScopedAllocationFailure failure(failIndex);
            operation();
        }
        catch (const std::bad_alloc&)
        {
            return true;
        }
    }

    return false;
}

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

template <typename Setup, typename Operation>
bool exerciseAllBadAllocAfterSetup(Setup&& setup, Operation&& operation,
                                   std::size_t maxFailAt = 256)
{
    if (pldm::test::runningOnValgrind())
    {
        return true;
    }

    bool sawBadAlloc = false;

    for (std::size_t failIndex = 1; failIndex <= maxFailAt; ++failIndex)
    {
        auto state = setup();
        try
        {
            ScopedAllocationFailure failure(failIndex);
            operation(state);
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

} // namespace async_utils_test

void* operator new(std::size_t size)
{
    return async_utils_test::allocate(size);
}

void* operator new[](std::size_t size)
{
    return async_utils_test::allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    return async_utils_test::allocate(size,
                                      static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return async_utils_test::allocate(size,
                                      static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return async_utils_test::allocate(size);
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
        return async_utils_test::allocate(size);
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

class DBusAsyncUtilsMockHandler : public DBusHandler
{
  public:
    static std::shared_ptr<async_utils_test::FakeAsioConnection>&
        fakeConnection()
    {
        static auto conn =
            std::make_shared<async_utils_test::FakeAsioConnection>();
        return conn;
    }

    static auto& getAsioConnection()
    {
        return fakeConnection();
    }
};

} // namespace pldm::utils

#define DBusHandler DBusAsyncUtilsMockHandler
#include "../dBusAsyncUtils.hpp"
#undef DBusHandler

namespace
{

using Associations =
    std::vector<std::tuple<std::string, std::string, std::string>>;
using ObjectPaths = std::vector<sdbusplus::object_path>;

class DBusAsyncUtilsTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        auto& conn = *pldm::utils::DBusAsyncUtilsMockHandler::fakeConnection();
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

    async_utils_test::FakeAsioConnection& connection()
    {
        return *pldm::utils::DBusAsyncUtilsMockHandler::fakeConnection();
    }
};

template <typename T>
void expectPropertyLifecycleExhaustiveBadAlloc(
    const std::string& path, const std::string& property,
    const std::string& iface, const std::string& service, const T& value,
    std::size_t maxFailAt = 512)
{
    auto& conn = *pldm::utils::DBusAsyncUtilsMockHandler::fakeConnection();

    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::coGetDbusProperty<T> prop(path, property, iface,
                                                   service);
            (void)prop;
        },
        maxFailAt));

    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc(
        [&] {
            conn.nextPropertyValue = value;
            pldm::utils::coGetDbusProperty<T> prop(path, property, iface,
                                                   service);
            (void)prop.await_suspend(std::noop_coroutine());
        },
        maxFailAt));
}

template <typename T>
void expectPropertyErrorPathExhaustiveBadAlloc(
    const std::string& path, const std::string& property,
    const std::string& iface, const std::string& service,
    std::size_t maxFailAt = 512)
{
    auto& conn = *pldm::utils::DBusAsyncUtilsMockHandler::fakeConnection();

    EXPECT_TRUE(async_utils_test::exerciseAllBadAllocAfterSetup(
        [&] {
            conn.nextPropertyError = boost::system::errc::make_error_code(
                boost::system::errc::network_unreachable);
            return 0;
        },
        [&](int) {
            pldm::utils::coGetDbusProperty<T> prop(path, property, iface,
                                                   service);
            (void)prop.await_suspend(std::noop_coroutine());
        },
        maxFailAt));

    conn.nextPropertyError.clear();
}

TEST_F(DBusAsyncUtilsTest, coGetDbusPropertyCoversSupportedTypes)
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
    EXPECT_FALSE(numericProp.await_ready());
    EXPECT_TRUE(numericProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(numericProp.await_resume(), uint64_t(42));

    conn.nextPropertyValue = true;
    pldm::utils::coGetDbusProperty<bool> boolProp(path, "Present", iface);
    EXPECT_FALSE(boolProp.await_ready());
    EXPECT_TRUE(boolProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(boolProp.await_resume());

    const std::vector<std::string> parents{"/xyz/openbmc_project/parent0",
                                           "/xyz/openbmc_project/parent1"};
    conn.nextPropertyValue = parents;
    pldm::utils::coGetDbusProperty<std::vector<std::string>> parentsProp(
        path, "Parents", iface);
    EXPECT_FALSE(parentsProp.await_ready());
    EXPECT_TRUE(parentsProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(parentsProp.await_resume(), parents);

    const Associations associations{
        {"chassis", "all_states", "/xyz/openbmc_project/inventory/chassis0"}};
    conn.nextPropertyValue = associations;
    pldm::utils::coGetDbusProperty<Associations> associationsProp(
        path, "Associations", iface);
    EXPECT_FALSE(associationsProp.await_ready());
    EXPECT_TRUE(associationsProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(associationsProp.await_resume(), associations);

    const std::vector<uint8_t> rawData{0x11, 0x22, 0x33};
    conn.nextPropertyValue = rawData;
    pldm::utils::coGetDbusProperty<std::vector<uint8_t>> rawDataProp(
        path, "RawData", iface);
    EXPECT_FALSE(rawDataProp.await_ready());
    EXPECT_TRUE(rawDataProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(rawDataProp.await_resume(), rawData);

    const std::vector<uint64_t> counters{1, 10, 100};
    conn.nextPropertyValue = counters;
    pldm::utils::coGetDbusProperty<std::vector<uint64_t>> countersProp(
        path, "Counters", iface);
    EXPECT_FALSE(countersProp.await_ready());
    EXPECT_TRUE(countersProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(countersProp.await_resume(), counters);

    const ObjectPaths objectPaths{
        sdbusplus::object_path("/xyz/openbmc_project/object0"),
        sdbusplus::object_path("/xyz/openbmc_project/object1")};
    conn.nextPropertyValue = objectPaths;
    pldm::utils::coGetDbusProperty<ObjectPaths> objectPathsProp(
        path, "ObjectPaths", iface);
    EXPECT_FALSE(objectPathsProp.await_ready());
    EXPECT_TRUE(objectPathsProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(objectPathsProp.await_resume(), objectPaths);
}

TEST_F(DBusAsyncUtilsTest, coGetDbusPropertyReturnsDefaultsOnError)
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

    pldm::utils::coGetDbusProperty<bool> boolProp(path, "Present", iface);
    EXPECT_TRUE(boolProp.await_suspend(std::noop_coroutine()));
    EXPECT_FALSE(boolProp.await_resume());

    pldm::utils::coGetDbusProperty<std::vector<std::string>> parentsProp(
        path, "Parents", iface);
    EXPECT_TRUE(parentsProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(parentsProp.await_resume().empty());

    pldm::utils::coGetDbusProperty<Associations> associationsProp(
        path, "Associations", iface);
    EXPECT_TRUE(associationsProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(associationsProp.await_resume().empty());

    pldm::utils::coGetDbusProperty<std::vector<uint8_t>> rawDataProp(
        path, "RawData", iface);
    EXPECT_TRUE(rawDataProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(rawDataProp.await_resume().empty());

    pldm::utils::coGetDbusProperty<std::vector<uint64_t>> countersProp(
        path, "Counters", iface);
    EXPECT_TRUE(countersProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(countersProp.await_resume().empty());

    pldm::utils::coGetDbusProperty<ObjectPaths> objectPathsProp(
        path, "ObjectPaths", iface);
    EXPECT_TRUE(objectPathsProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(objectPathsProp.await_resume().empty());
}

TEST_F(DBusAsyncUtilsTest,
       coGetDbusPropertyReturnsDefaultOnVariantMismatchAcrossSupportedTypes)
{
    // coGetDbusProperty::await_suspend catches std::bad_variant_access
    // inside the Properties.Get callback (see common/dBusAsyncUtils.hpp),
    // logs the mismatch, and resolves await_resume with the default-
    // constructed value of the requested type. This matches the
    // error_code failure path so callers do not need to wrap co_await in
    // try/catch.
    constexpr auto path = "/xyz/openbmc_project/test/object";
    constexpr auto iface = "xyz.openbmc_project.Test.Interface";
    auto& conn = connection();

    conn.nextPropertyValue = uint64_t(99);
    pldm::utils::coGetDbusProperty<std::string> stringProp(path, "Name", iface);
    EXPECT_TRUE(stringProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(stringProp.await_resume().empty());

    conn.nextPropertyValue = std::string("wrong");
    pldm::utils::coGetDbusProperty<uint64_t> numericProp(path, "Bus", iface);
    EXPECT_TRUE(numericProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(numericProp.await_resume(), uint64_t(0));

    conn.nextPropertyValue = std::string("true");
    pldm::utils::coGetDbusProperty<bool> boolProp(path, "Present", iface);
    EXPECT_TRUE(boolProp.await_suspend(std::noop_coroutine()));
    EXPECT_FALSE(boolProp.await_resume());

    conn.nextPropertyValue = true;
    pldm::utils::coGetDbusProperty<std::vector<std::string>> parentsProp(
        path, "Parents", iface);
    EXPECT_TRUE(parentsProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(parentsProp.await_resume().empty());

    conn.nextPropertyValue = std::string("not-associations");
    pldm::utils::coGetDbusProperty<Associations> associationsProp(
        path, "Associations", iface);
    EXPECT_TRUE(associationsProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(associationsProp.await_resume().empty());

    conn.nextPropertyValue = std::vector<uint64_t>{1, 2};
    pldm::utils::coGetDbusProperty<std::vector<uint8_t>> rawDataProp(
        path, "RawData", iface);
    EXPECT_TRUE(rawDataProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(rawDataProp.await_resume().empty());

    conn.nextPropertyValue = std::vector<uint8_t>{0x11, 0x22};
    pldm::utils::coGetDbusProperty<std::vector<uint64_t>> countersProp(
        path, "Counters", iface);
    EXPECT_TRUE(countersProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(countersProp.await_resume().empty());

    conn.nextPropertyValue = std::vector<std::string>{"not", "paths"};
    pldm::utils::coGetDbusProperty<ObjectPaths> objectPathsProp(
        path, "ObjectPaths", iface);
    EXPECT_TRUE(objectPathsProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(objectPathsProp.await_resume().empty());
}

TEST_F(DBusAsyncUtilsTest, coGetDbusPropertyAssociationsAwaitSuspendCoverage)
{
    constexpr auto path = "/xyz/openbmc_project/test/object";
    constexpr auto iface = "xyz.openbmc_project.Test.Interface";
    auto& conn = connection();

    const Associations expectedAssociations{
        {"forward", "reverse", "/xyz/openbmc_project/inventory/item0"},
        {"parent", "child", "/xyz/openbmc_project/inventory/item1"}};
    conn.nextPropertyValue = expectedAssociations;

    pldm::utils::coGetDbusProperty<Associations> associationsProp(
        path, "Associations", iface);
    EXPECT_TRUE(associationsProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(associationsProp.await_resume(), expectedAssociations);

    conn.nextPropertyError =
        boost::system::errc::make_error_code(boost::system::errc::io_error);
    pldm::utils::coGetDbusProperty<Associations> errorProp(
        path, "Associations", iface);
    EXPECT_TRUE(errorProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(errorProp.await_resume().empty());

    // Variant mismatch: the awaitable catches bad_variant_access inside
    // the Properties.Get callback and resolves with an empty
    // Associations, mirroring the error_code failure path above.
    conn.nextPropertyError.clear();
    conn.nextPropertyValue = std::string("wrong");
    pldm::utils::coGetDbusProperty<Associations> mismatchProp(
        path, "Associations", iface);
    EXPECT_TRUE(mismatchProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(mismatchProp.await_resume().empty());
}

TEST_F(DBusAsyncUtilsTest,
       coGetDbusPropertyHeapBackedDefaultServiceCtorCoverage)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(32, 'p');
    const std::string iface =
        "xyz.openbmc_project.Test." + std::string(20, 'I');

    pldm::utils::coGetDbusProperty<std::vector<std::string>> namesProp(
        path, "Names", iface);
    EXPECT_EQ(namesProp.service, pldm::utils::entityManagerService);
    EXPECT_EQ(namesProp.objectPath, path);
    EXPECT_EQ(namesProp.interface, iface);
    EXPECT_EQ(namesProp.property, "Names");
    EXPECT_TRUE(namesProp.await_resume().empty());

    pldm::utils::coGetDbusProperty<Associations> associationsProp(
        path, "Associations", iface);
    EXPECT_EQ(associationsProp.service, pldm::utils::entityManagerService);
    EXPECT_EQ(associationsProp.objectPath, path);
    EXPECT_EQ(associationsProp.interface, iface);
    EXPECT_EQ(associationsProp.property, "Associations");
    EXPECT_TRUE(associationsProp.await_resume().empty());

    pldm::utils::coGetDbusProperty<std::vector<uint8_t>> rawDataProp(
        path, "RawData", iface);
    EXPECT_EQ(rawDataProp.service, pldm::utils::entityManagerService);
    EXPECT_EQ(rawDataProp.objectPath, path);
    EXPECT_EQ(rawDataProp.interface, iface);
    EXPECT_EQ(rawDataProp.property, "RawData");
    EXPECT_TRUE(rawDataProp.await_resume().empty());

    pldm::utils::coGetDbusProperty<std::vector<uint64_t>> countersProp(
        path, "Counters", iface);
    EXPECT_EQ(countersProp.service, pldm::utils::entityManagerService);
    EXPECT_EQ(countersProp.objectPath, path);
    EXPECT_EQ(countersProp.interface, iface);
    EXPECT_EQ(countersProp.property, "Counters");
    EXPECT_TRUE(countersProp.await_resume().empty());

    pldm::utils::coGetDbusProperty<ObjectPaths> objectPathsProp(
        path, "Paths", iface);
    EXPECT_EQ(objectPathsProp.service, pldm::utils::entityManagerService);
    EXPECT_EQ(objectPathsProp.objectPath, path);
    EXPECT_EQ(objectPathsProp.interface, iface);
    EXPECT_EQ(objectPathsProp.property, "Paths");
    EXPECT_TRUE(objectPathsProp.await_resume().empty());
}

TEST_F(DBusAsyncUtilsTest, coGetDbusPropertyCoversRemainingPrimitiveTypes)
{
    constexpr auto path = "/xyz/openbmc_project/test/object";
    constexpr auto iface = "xyz.openbmc_project.Test.Interface";
    auto& conn = connection();

    conn.nextPropertyValue = static_cast<uint8_t>(7);
    pldm::utils::coGetDbusProperty<uint8_t> u8Prop(path, "U8", iface);
    EXPECT_FALSE(u8Prop.await_ready());
    EXPECT_TRUE(u8Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(u8Prop.await_resume(), static_cast<uint8_t>(7));

    conn.nextPropertyValue = static_cast<int16_t>(-3);
    pldm::utils::coGetDbusProperty<int16_t> i16Prop(path, "I16", iface);
    EXPECT_FALSE(i16Prop.await_ready());
    EXPECT_TRUE(i16Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(i16Prop.await_resume(), static_cast<int16_t>(-3));

    conn.nextPropertyValue = static_cast<uint16_t>(17);
    pldm::utils::coGetDbusProperty<uint16_t> u16Prop(path, "U16", iface);
    EXPECT_FALSE(u16Prop.await_ready());
    EXPECT_TRUE(u16Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(u16Prop.await_resume(), static_cast<uint16_t>(17));

    conn.nextPropertyValue = static_cast<int32_t>(-31);
    pldm::utils::coGetDbusProperty<int32_t> i32Prop(path, "I32", iface);
    EXPECT_FALSE(i32Prop.await_ready());
    EXPECT_TRUE(i32Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(i32Prop.await_resume(), static_cast<int32_t>(-31));

    conn.nextPropertyValue = static_cast<uint32_t>(41);
    pldm::utils::coGetDbusProperty<uint32_t> u32Prop(path, "U32", iface);
    EXPECT_FALSE(u32Prop.await_ready());
    EXPECT_TRUE(u32Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(u32Prop.await_resume(), static_cast<uint32_t>(41));

    conn.nextPropertyValue = static_cast<int64_t>(-53);
    pldm::utils::coGetDbusProperty<int64_t> i64Prop(path, "I64", iface);
    EXPECT_FALSE(i64Prop.await_ready());
    EXPECT_TRUE(i64Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(i64Prop.await_resume(), static_cast<int64_t>(-53));

    conn.nextPropertyValue = 42.5;
    pldm::utils::coGetDbusProperty<double> doubleProp(path, "Double", iface);
    EXPECT_FALSE(doubleProp.await_ready());
    EXPECT_TRUE(doubleProp.await_suspend(std::noop_coroutine()));
    EXPECT_DOUBLE_EQ(doubleProp.await_resume(), 42.5);
}

TEST_F(DBusAsyncUtilsTest,
       coGetDbusPropertyReturnsRemainingPrimitiveDefaultsOnError)
{
    constexpr auto path = "/xyz/openbmc_project/test/object";
    constexpr auto iface = "xyz.openbmc_project.Test.Interface";
    auto& conn = connection();
    conn.nextPropertyError =
        boost::system::errc::make_error_code(boost::system::errc::io_error);

    pldm::utils::coGetDbusProperty<uint8_t> u8Prop(path, "U8", iface);
    EXPECT_TRUE(u8Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(u8Prop.await_resume(), static_cast<uint8_t>(0));

    pldm::utils::coGetDbusProperty<int16_t> i16Prop(path, "I16", iface);
    EXPECT_TRUE(i16Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(i16Prop.await_resume(), static_cast<int16_t>(0));

    pldm::utils::coGetDbusProperty<uint16_t> u16Prop(path, "U16", iface);
    EXPECT_TRUE(u16Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(u16Prop.await_resume(), static_cast<uint16_t>(0));

    pldm::utils::coGetDbusProperty<int32_t> i32Prop(path, "I32", iface);
    EXPECT_TRUE(i32Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(i32Prop.await_resume(), static_cast<int32_t>(0));

    pldm::utils::coGetDbusProperty<uint32_t> u32Prop(path, "U32", iface);
    EXPECT_TRUE(u32Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(u32Prop.await_resume(), static_cast<uint32_t>(0));

    pldm::utils::coGetDbusProperty<int64_t> i64Prop(path, "I64", iface);
    EXPECT_TRUE(i64Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(i64Prop.await_resume(), static_cast<int64_t>(0));

    pldm::utils::coGetDbusProperty<double> doubleProp(path, "Double", iface);
    EXPECT_TRUE(doubleProp.await_suspend(std::noop_coroutine()));
    EXPECT_DOUBLE_EQ(doubleProp.await_resume(), 0.0);
}

TEST_F(
    DBusAsyncUtilsTest,
    coGetDbusPropertyReturnsDefaultOnVariantMismatchForRemainingPrimitiveTypes)
{
    // See coGetDbusPropertyReturnsDefaultOnVariantMismatchAcrossSupportedTypes
    // for the contract: variant mismatch is caught inside the callback,
    // logged, and surfaces as a default-constructed value via await_resume.
    constexpr auto path = "/xyz/openbmc_project/test/object";
    constexpr auto iface = "xyz.openbmc_project.Test.Interface";
    auto& conn = connection();

    conn.nextPropertyValue = std::string("wrong");
    pldm::utils::coGetDbusProperty<uint8_t> u8Prop(path, "U8", iface);
    EXPECT_TRUE(u8Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(u8Prop.await_resume(), static_cast<uint8_t>(0));

    conn.nextPropertyValue = std::string("wrong");
    pldm::utils::coGetDbusProperty<int16_t> i16Prop(path, "I16", iface);
    EXPECT_TRUE(i16Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(i16Prop.await_resume(), static_cast<int16_t>(0));

    conn.nextPropertyValue = std::string("wrong");
    pldm::utils::coGetDbusProperty<uint16_t> u16Prop(path, "U16", iface);
    EXPECT_TRUE(u16Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(u16Prop.await_resume(), static_cast<uint16_t>(0));

    conn.nextPropertyValue = std::string("wrong");
    pldm::utils::coGetDbusProperty<int32_t> i32Prop(path, "I32", iface);
    EXPECT_TRUE(i32Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(i32Prop.await_resume(), static_cast<int32_t>(0));

    conn.nextPropertyValue = std::string("wrong");
    pldm::utils::coGetDbusProperty<uint32_t> u32Prop(path, "U32", iface);
    EXPECT_TRUE(u32Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(u32Prop.await_resume(), static_cast<uint32_t>(0));

    conn.nextPropertyValue = std::string("wrong");
    pldm::utils::coGetDbusProperty<int64_t> i64Prop(path, "I64", iface);
    EXPECT_TRUE(i64Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(i64Prop.await_resume(), static_cast<int64_t>(0));

    conn.nextPropertyValue = std::string("wrong");
    pldm::utils::coGetDbusProperty<double> doubleProp(path, "Double", iface);
    EXPECT_TRUE(doubleProp.await_suspend(std::noop_coroutine()));
    EXPECT_DOUBLE_EQ(doubleProp.await_resume(), 0.0);
}

TEST_F(DBusAsyncUtilsTest, coGetServiceMapCoversSuccessAndErrorPaths)
{
    auto& conn = connection();
    const std::string path = "/xyz/openbmc_project/test/object";
    const pldm::dbus::Interfaces ifaces{"xyz.openbmc_project.Test.Interface"};

    const pldm::utils::MapperServiceMap expected{
        {"xyz.openbmc_project.Test.Service", ifaces}};
    conn.nextServiceMap = expected;

    pldm::utils::coGetServiceMap serviceMap(path, ifaces);
    EXPECT_FALSE(serviceMap.await_ready());
    EXPECT_TRUE(serviceMap.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(serviceMap.await_resume(), expected);

    conn.nextServiceMapError = boost::system::errc::make_error_code(
        boost::system::errc::no_such_file_or_directory);
    pldm::utils::coGetServiceMap serviceMapError(path, ifaces);
    EXPECT_TRUE(serviceMapError.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(serviceMapError.await_resume().empty());
}

TEST_F(DBusAsyncUtilsTest, coGetSubTreeCoversSuccessAndErrorPaths)
{
    auto& conn = connection();
    const std::string path = "/xyz/openbmc_project/test";
    const pldm::dbus::Interfaces ifaces{"xyz.openbmc_project.Test.Interface"};

    const pldm::utils::GetSubTreeResponse expected{
        {"/xyz/openbmc_project/test/object",
         {{"xyz.openbmc_project.Test.Service", ifaces}}}};
    conn.nextSubTree = expected;

    pldm::utils::coGetSubTree subTree(path, 1, ifaces);
    EXPECT_FALSE(subTree.await_ready());
    EXPECT_TRUE(subTree.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(subTree.await_resume(), expected);

    conn.nextSubTreeError = boost::system::errc::make_error_code(
        boost::system::errc::host_unreachable);
    pldm::utils::coGetSubTree subTreeError(path, 1, ifaces);
    EXPECT_TRUE(subTreeError.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(subTreeError.await_resume().empty());
}

TEST_F(DBusAsyncUtilsTest, coGetDbusPropertyConstructorBoundaryMatrixCoverage)
{
    auto& conn = connection();
    const std::vector<std::size_t> sizes{0, 1, 15, 16, 31, 32, 96};

    for (std::size_t idx = 0; idx < sizes.size(); ++idx)
    {
        const std::string objectPath =
            "/xyz/openbmc_project/test/" + std::string(sizes[idx], 'p');
        const std::string interface =
            "xyz.openbmc_project.Test.Interface." +
            std::string(sizes[(idx + 1) % sizes.size()], 'I');
        const std::string property =
            "Property" + std::string(sizes[(idx + 2) % sizes.size()], 'R');
        const std::string service =
            (idx % 2 == 0)
                ? std::string{}
                : "xyz.openbmc_project.Test.Service." +
                      std::string(sizes[(idx + 3) % sizes.size()], 'S');

        conn.nextPropertyError.clear();
        conn.nextPropertyValue = property + "#" + std::to_string(idx);

        pldm::utils::coGetDbusProperty<std::string> customServiceProp(
            objectPath, property, interface, service);
        EXPECT_FALSE(customServiceProp.await_ready());
        EXPECT_TRUE(customServiceProp.await_suspend(std::noop_coroutine()));
        EXPECT_EQ(customServiceProp.await_resume(),
                  property + "#" + std::to_string(idx));

        conn.nextPropertyValue = static_cast<uint64_t>(100 + idx);
        pldm::utils::coGetDbusProperty<uint64_t> defaultServiceProp(
            objectPath, property + "U64", interface);
        EXPECT_FALSE(defaultServiceProp.await_ready());
        EXPECT_TRUE(defaultServiceProp.await_suspend(std::noop_coroutine()));
        EXPECT_EQ(defaultServiceProp.await_resume(),
                  static_cast<uint64_t>(100 + idx));
    }
}

TEST_F(DBusAsyncUtilsTest,
       coGetServiceMapAndSubTreeConstructorBoundaryMatrixCoverage)
{
    auto& conn = connection();
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

        pldm::utils::MapperServiceMap expectedServiceMap{
            {"xyz.openbmc_project.Test.Service." + std::to_string(idx),
             interfaces}};
        conn.nextServiceMapError.clear();
        conn.nextServiceMap = expectedServiceMap;

        pldm::utils::coGetServiceMap serviceMap(objectPath, interfaces);
        EXPECT_FALSE(serviceMap.await_ready());
        EXPECT_TRUE(serviceMap.await_suspend(std::noop_coroutine()));
        EXPECT_EQ(serviceMap.await_resume(), expectedServiceMap);

        const pldm::utils::GetSubTreeResponse expectedSubTree{
            {objectPath + "/object", expectedServiceMap}};
        conn.nextSubTreeError.clear();
        conn.nextSubTree = expectedSubTree;

        pldm::utils::coGetSubTree subTree(objectPath, static_cast<int>(idx),
                                          interfaces);
        EXPECT_FALSE(subTree.await_ready());
        EXPECT_TRUE(subTree.await_suspend(std::noop_coroutine()));
        EXPECT_EQ(subTree.await_resume(), expectedSubTree);

        conn.nextServiceMapError = boost::system::errc::make_error_code(
            boost::system::errc::network_reset);
        pldm::utils::coGetServiceMap serviceMapError(objectPath, interfaces);
        EXPECT_TRUE(serviceMapError.await_suspend(std::noop_coroutine()));
        EXPECT_TRUE(serviceMapError.await_resume().empty());

        conn.nextServiceMapError.clear();
        conn.nextSubTreeError = boost::system::errc::make_error_code(
            boost::system::errc::network_down);
        pldm::utils::coGetSubTree subTreeError(
            objectPath + "/retry", static_cast<int>(idx + 1), interfaces);
        EXPECT_TRUE(subTreeError.await_suspend(std::noop_coroutine()));
        EXPECT_TRUE(subTreeError.await_resume().empty());
        conn.nextSubTreeError.clear();
    }
}

// The static analyzer reports false leaks through GTest death-test matcher
// internals. Keep these runtime-only coverage tests out of analyzer builds.
#ifndef __clang_analyzer__
TEST_F(DBusAsyncUtilsTest,
       coGetServiceMapAwaitSuspendPropagatesAsyncCallExceptions)
{
    auto& conn = connection();
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(40, 'm');
    const pldm::dbus::Interfaces ifaces{
        "xyz.openbmc_project.Test.Interface." + std::string(24, 'I'),
        "xyz.openbmc_project.Test.Interface." + std::string(28, 'J')};

    conn.throwOnServiceMapCall = true;

    pldm::utils::coGetServiceMap serviceMap(path, ifaces);
    EXPECT_DEATH(
        { serviceMap.await_suspend(std::noop_coroutine()); },
        "service map async call failure");
}

TEST_F(DBusAsyncUtilsTest,
       coGetSubTreeAwaitSuspendPropagatesAsyncCallExceptions)
{
    auto& conn = connection();
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(42, 't');
    const pldm::dbus::Interfaces ifaces{
        "xyz.openbmc_project.Test.Interface." + std::string(26, 'P'),
        "xyz.openbmc_project.Test.Interface." + std::string(30, 'Q')};

    conn.throwOnSubTreeCall = true;

    pldm::utils::coGetSubTree subTree(path, 2, ifaces);
    EXPECT_DEATH(
        { subTree.await_suspend(std::noop_coroutine()); },
        "subtree async call failure");
}
#endif

TEST_F(DBusAsyncUtilsTest,
       coGetDbusPropertyAwaitSuspendPropagatesAsyncCallExceptions)
{
    constexpr auto path = "/xyz/openbmc_project/test/object";
    constexpr auto iface = "xyz.openbmc_project.Test.Interface";
    auto& conn = connection();
    conn.throwOnPropertyCall = true;

    pldm::utils::coGetDbusProperty<std::string> stringProp(path, "Name", iface);
    EXPECT_THROW(stringProp.await_suspend(std::noop_coroutine()),
                 std::runtime_error);

    pldm::utils::coGetDbusProperty<std::vector<std::string>> parentsProp(
        path, "Parents", iface);
    EXPECT_THROW(parentsProp.await_suspend(std::noop_coroutine()),
                 std::runtime_error);

    pldm::utils::coGetDbusProperty<Associations> associationsProp(
        path, "Associations", iface);
    EXPECT_THROW(associationsProp.await_suspend(std::noop_coroutine()),
                 std::runtime_error);

    pldm::utils::coGetDbusProperty<uint64_t> countersProp(
        path, "Counters", iface);
    EXPECT_THROW(countersProp.await_suspend(std::noop_coroutine()),
                 std::runtime_error);

    pldm::utils::coGetDbusProperty<bool> boolProp(path, "Present", iface);
    EXPECT_THROW(boolProp.await_suspend(std::noop_coroutine()),
                 std::runtime_error);

    pldm::utils::coGetDbusProperty<uint8_t> u8Prop(path, "U8", iface);
    EXPECT_THROW(u8Prop.await_suspend(std::noop_coroutine()),
                 std::runtime_error);

    pldm::utils::coGetDbusProperty<int16_t> i16Prop(path, "I16", iface);
    EXPECT_THROW(i16Prop.await_suspend(std::noop_coroutine()),
                 std::runtime_error);

    pldm::utils::coGetDbusProperty<uint16_t> u16Prop(path, "U16", iface);
    EXPECT_THROW(u16Prop.await_suspend(std::noop_coroutine()),
                 std::runtime_error);

    pldm::utils::coGetDbusProperty<int32_t> i32Prop(path, "I32", iface);
    EXPECT_THROW(i32Prop.await_suspend(std::noop_coroutine()),
                 std::runtime_error);

    pldm::utils::coGetDbusProperty<uint32_t> u32Prop(path, "U32", iface);
    EXPECT_THROW(u32Prop.await_suspend(std::noop_coroutine()),
                 std::runtime_error);

    pldm::utils::coGetDbusProperty<int64_t> i64Prop(path, "I64", iface);
    EXPECT_THROW(i64Prop.await_suspend(std::noop_coroutine()),
                 std::runtime_error);

    pldm::utils::coGetDbusProperty<double> doubleProp(path, "Double", iface);
    EXPECT_THROW(doubleProp.await_suspend(std::noop_coroutine()),
                 std::runtime_error);

    pldm::utils::coGetDbusProperty<std::vector<uint8_t>> rawDataProp(
        path, "RawData", iface);
    EXPECT_THROW(rawDataProp.await_suspend(std::noop_coroutine()),
                 std::runtime_error);

    pldm::utils::coGetDbusProperty<std::vector<uint64_t>> countersVecProp(
        path, "CounterValues", iface);
    EXPECT_THROW(countersVecProp.await_suspend(std::noop_coroutine()),
                 std::runtime_error);

    pldm::utils::coGetDbusProperty<ObjectPaths> objectPathsProp(
        path, "ObjectPaths", iface);
    EXPECT_THROW(objectPathsProp.await_suspend(std::noop_coroutine()),
                 std::runtime_error);
}

TEST_F(DBusAsyncUtilsTest,
       coGetDbusPropertyHandlesHeapBackedValuesAndCustomService)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(48, 'o');
    const std::string iface =
        "xyz.openbmc_project.Test." + std::string(40, 'I');
    const std::string service =
        "xyz.openbmc_project.Test." + std::string(36, 'S');
    auto& conn = connection();

    const std::string longString(96, 'n');
    conn.nextPropertyValue = longString;
    pldm::utils::coGetDbusProperty<std::string> stringProp(
        path, "Name", iface, service);
    EXPECT_TRUE(stringProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(stringProp.await_resume(), longString);

    const std::vector<std::string> longParents{
        "/xyz/openbmc_project/inventory/" + std::string(48, 'a'),
        "/xyz/openbmc_project/inventory/" + std::string(52, 'b')};
    conn.nextPropertyValue = longParents;
    pldm::utils::coGetDbusProperty<std::vector<std::string>> parentsProp(
        path, "Parents", iface, service);
    EXPECT_TRUE(parentsProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(parentsProp.await_resume(), longParents);

    const Associations longAssociations{
        {"forward-" + std::string(24, 'f'), "reverse-" + std::string(24, 'r'),
         "/xyz/openbmc_project/inventory/" + std::string(56, 'p')}};
    conn.nextPropertyValue = longAssociations;
    pldm::utils::coGetDbusProperty<Associations> associationsProp(
        path, "Associations", iface, service);
    EXPECT_TRUE(associationsProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(associationsProp.await_resume(), longAssociations);

    const std::vector<uint8_t> longRawData(64, 0x5A);
    conn.nextPropertyValue = longRawData;
    pldm::utils::coGetDbusProperty<std::vector<uint8_t>> rawDataProp(
        path, "RawData", iface, service);
    EXPECT_TRUE(rawDataProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(rawDataProp.await_resume(), longRawData);

    const std::vector<uint64_t> longCounters{1, 10, 100, 1000, 10000, 100000};
    conn.nextPropertyValue = longCounters;
    pldm::utils::coGetDbusProperty<std::vector<uint64_t>> countersProp(
        path, "Counters", iface, service);
    EXPECT_TRUE(countersProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(countersProp.await_resume(), longCounters);

    const ObjectPaths longObjectPaths{
        sdbusplus::object_path(
            "/xyz/openbmc_project/object/" + std::string(44, 'x')),
        sdbusplus::object_path(
            "/xyz/openbmc_project/object/" + std::string(46, 'y'))};
    conn.nextPropertyValue = longObjectPaths;
    pldm::utils::coGetDbusProperty<ObjectPaths> objectPathsProp(
        path, "ObjectPaths", iface, service);
    EXPECT_TRUE(objectPathsProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(objectPathsProp.await_resume(), longObjectPaths);
}

TEST_F(DBusAsyncUtilsTest,
       coGetDbusPropertyHandlesLongPropertyNamesAndEmptyHeapBackedValues)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(52, 'p');
    const std::string iface =
        "xyz.openbmc_project.Test." + std::string(44, 'I');
    const std::string property = "Property" + std::string(72, 'N');
    const std::string service =
        "xyz.openbmc_project.Test." + std::string(40, 'S');
    auto& conn = connection();

    conn.nextPropertyValue = std::string{};
    pldm::utils::coGetDbusProperty<std::string> stringProp(
        path, property, iface, service);
    EXPECT_TRUE(stringProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(stringProp.await_resume().empty());

    conn.nextPropertyValue = std::vector<std::string>{};
    pldm::utils::coGetDbusProperty<std::vector<std::string>> parentsProp(
        path, property + "Parents", iface, service);
    EXPECT_TRUE(parentsProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(parentsProp.await_resume().empty());

    conn.nextPropertyValue = Associations{};
    pldm::utils::coGetDbusProperty<Associations> associationsProp(
        path, property + "Associations", iface, service);
    EXPECT_TRUE(associationsProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(associationsProp.await_resume().empty());

    conn.nextPropertyValue = std::vector<uint8_t>{};
    pldm::utils::coGetDbusProperty<std::vector<uint8_t>> rawDataProp(
        path, property + "RawData", iface, service);
    EXPECT_TRUE(rawDataProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(rawDataProp.await_resume().empty());

    conn.nextPropertyValue = std::vector<uint64_t>{};
    pldm::utils::coGetDbusProperty<std::vector<uint64_t>> countersProp(
        path, property + "Counters", iface, service);
    EXPECT_TRUE(countersProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(countersProp.await_resume().empty());

    conn.nextPropertyValue = ObjectPaths{};
    pldm::utils::coGetDbusProperty<ObjectPaths> objectPathsProp(
        path, property + "ObjectPaths", iface, service);
    EXPECT_TRUE(objectPathsProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(objectPathsProp.await_resume().empty());
}

TEST_F(DBusAsyncUtilsTest,
       coGetDbusPropertyReturnsDefaultsOnErrorForLongCaptureStrings)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(60, 'x');
    const std::string iface =
        "xyz.openbmc_project.Test." + std::string(36, 'Y');
    const std::string property = "LongProperty" + std::string(64, 'Z');
    const std::string service =
        "xyz.openbmc_project.Test." + std::string(48, 'Q');
    auto& conn = connection();
    conn.nextPropertyError = boost::system::errc::make_error_code(
        boost::system::errc::host_unreachable);

    pldm::utils::coGetDbusProperty<std::string> stringProp(
        path, property, iface, service);
    EXPECT_TRUE(stringProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(stringProp.await_resume().empty());

    pldm::utils::coGetDbusProperty<uint64_t> numericProp(
        path, property + "Counter", iface, service);
    EXPECT_TRUE(numericProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(numericProp.await_resume(), uint64_t(0));

    pldm::utils::coGetDbusProperty<Associations> associationsProp(
        path, property + "Associations", iface, service);
    EXPECT_TRUE(associationsProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(associationsProp.await_resume().empty());

    pldm::utils::coGetDbusProperty<ObjectPaths> objectPathsProp(
        path, property + "Paths", iface, service);
    EXPECT_TRUE(objectPathsProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(objectPathsProp.await_resume().empty());
}

TEST_F(DBusAsyncUtilsTest,
       coGetDbusPropertyHeapBackedTypesWithShortCustomServiceCoverage)
{
    const std::string path = "/x";
    const std::string iface = "i";
    const std::string service = "svc";
    auto& conn = connection();

    conn.nextPropertyValue = std::string("v");
    pldm::utils::coGetDbusProperty<std::string> stringProp(
        path, "Name", iface, service);
    EXPECT_EQ(stringProp.service, service);
    EXPECT_TRUE(stringProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(stringProp.await_resume(), "v");

    const std::vector<std::string> parents{"a", "b"};
    conn.nextPropertyValue = parents;
    pldm::utils::coGetDbusProperty<std::vector<std::string>> parentsProp(
        path, "Parents", iface, service);
    EXPECT_EQ(parentsProp.service, service);
    EXPECT_TRUE(parentsProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(parentsProp.await_resume(), parents);

    const Associations associations{{"f", "r", "/p"}};
    conn.nextPropertyValue = associations;
    pldm::utils::coGetDbusProperty<Associations> associationsProp(
        path, "Associations", iface, service);
    EXPECT_EQ(associationsProp.service, service);
    EXPECT_TRUE(associationsProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(associationsProp.await_resume(), associations);

    const std::vector<uint8_t> rawData{0x01, 0x02};
    conn.nextPropertyValue = rawData;
    pldm::utils::coGetDbusProperty<std::vector<uint8_t>> rawDataProp(
        path, "RawData", iface, service);
    EXPECT_EQ(rawDataProp.service, service);
    EXPECT_TRUE(rawDataProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(rawDataProp.await_resume(), rawData);

    const std::vector<uint64_t> counters{1, 2};
    conn.nextPropertyValue = counters;
    pldm::utils::coGetDbusProperty<std::vector<uint64_t>> countersProp(
        path, "Counters", iface, service);
    EXPECT_EQ(countersProp.service, service);
    EXPECT_TRUE(countersProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(countersProp.await_resume(), counters);

    const ObjectPaths objectPaths{sdbusplus::object_path("/p0"),
                                  sdbusplus::object_path("/p1")};
    conn.nextPropertyValue = objectPaths;
    pldm::utils::coGetDbusProperty<ObjectPaths> objectPathsProp(
        path, "ObjectPaths", iface, service);
    EXPECT_EQ(objectPathsProp.service, service);
    EXPECT_TRUE(objectPathsProp.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(objectPathsProp.await_resume(), objectPaths);
}

TEST_F(DBusAsyncUtilsTest,
       coGetDbusPropertyHeapBackedTypesReturnDefaultsOnShortServiceErrors)
{
    const std::string path = "/x";
    const std::string iface = "i";
    const std::string service = "svc";
    auto& conn = connection();
    conn.nextPropertyError = boost::system::errc::make_error_code(
        boost::system::errc::host_unreachable);

    pldm::utils::coGetDbusProperty<std::string> stringProp(
        path, "Name", iface, service);
    EXPECT_TRUE(stringProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(stringProp.await_resume().empty());

    pldm::utils::coGetDbusProperty<std::vector<std::string>> parentsProp(
        path, "Parents", iface, service);
    EXPECT_TRUE(parentsProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(parentsProp.await_resume().empty());

    pldm::utils::coGetDbusProperty<Associations> associationsProp(
        path, "Associations", iface, service);
    EXPECT_TRUE(associationsProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(associationsProp.await_resume().empty());

    pldm::utils::coGetDbusProperty<std::vector<uint8_t>> rawDataProp(
        path, "RawData", iface, service);
    EXPECT_TRUE(rawDataProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(rawDataProp.await_resume().empty());

    pldm::utils::coGetDbusProperty<std::vector<uint64_t>> countersProp(
        path, "Counters", iface, service);
    EXPECT_TRUE(countersProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(countersProp.await_resume().empty());

    pldm::utils::coGetDbusProperty<ObjectPaths> objectPathsProp(
        path, "ObjectPaths", iface, service);
    EXPECT_TRUE(objectPathsProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(objectPathsProp.await_resume().empty());
}

TEST_F(DBusAsyncUtilsTest,
       coGetDbusPropertyHeapBackedTypesReturnDefaultOnShortServiceMismatches)
{
    // Same default-on-mismatch contract as the long-service-name tests
    // above, exercising the short-string heap-allocated paths.
    const std::string path = "/x";
    const std::string iface = "i";
    const std::string service = "svc";
    auto& conn = connection();

    conn.nextPropertyValue = uint64_t{9};
    pldm::utils::coGetDbusProperty<std::string> stringProp(
        path, "Name", iface, service);
    EXPECT_TRUE(stringProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(stringProp.await_resume().empty());

    conn.nextPropertyValue = std::vector<uint64_t>{1, 2};
    pldm::utils::coGetDbusProperty<std::vector<std::string>> parentsProp(
        path, "Parents", iface, service);
    EXPECT_TRUE(parentsProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(parentsProp.await_resume().empty());

    conn.nextPropertyValue = std::vector<std::string>{"not", "assoc"};
    pldm::utils::coGetDbusProperty<Associations> associationsProp(
        path, "Associations", iface, service);
    EXPECT_TRUE(associationsProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(associationsProp.await_resume().empty());

    conn.nextPropertyValue = std::vector<uint64_t>{7, 8};
    pldm::utils::coGetDbusProperty<std::vector<uint8_t>> rawDataProp(
        path, "RawData", iface, service);
    EXPECT_TRUE(rawDataProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(rawDataProp.await_resume().empty());

    conn.nextPropertyValue = std::string("bad");
    pldm::utils::coGetDbusProperty<std::vector<uint64_t>> countersProp(
        path, "Counters", iface, service);
    EXPECT_TRUE(countersProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(countersProp.await_resume().empty());

    conn.nextPropertyValue = std::vector<std::string>{"not", "paths"};
    pldm::utils::coGetDbusProperty<ObjectPaths> objectPathsProp(
        path, "ObjectPaths", iface, service);
    EXPECT_TRUE(objectPathsProp.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(objectPathsProp.await_resume().empty());
}

TEST_F(DBusAsyncUtilsTest, coGetServiceMapHandlesLargeMapsAndLongPaths)
{
    auto& conn = connection();
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(56, 'm');
    const pldm::dbus::Interfaces ifaces{
        "xyz.openbmc_project.Test.Interface",
        "xyz.openbmc_project.Test." + std::string(28, 'I')};

    const pldm::utils::MapperServiceMap expected{
        {"xyz.openbmc_project.Test.Service." + std::string(22, 'A'), ifaces},
        {"xyz.openbmc_project.Test.Service." + std::string(24, 'B'),
         {"xyz.openbmc_project.Test.Alt"}}};
    conn.nextServiceMap = expected;

    pldm::utils::coGetServiceMap serviceMap(path, ifaces);
    EXPECT_TRUE(serviceMap.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(serviceMap.await_resume(), expected);

    conn.nextServiceMap = {};
    pldm::utils::coGetServiceMap emptyMap(path, pldm::dbus::Interfaces{});
    EXPECT_TRUE(emptyMap.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(emptyMap.await_resume().empty());
}

TEST_F(DBusAsyncUtilsTest, coGetSubTreeHandlesLargeResponsesAndLongPaths)
{
    auto& conn = connection();
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(58, 't');
    const pldm::dbus::Interfaces ifaces{
        "xyz.openbmc_project.Test.Interface",
        "xyz.openbmc_project.Test." + std::string(26, 'J')};

    const pldm::utils::GetSubTreeResponse expected{
        {"/xyz/openbmc_project/test/object/" + std::string(18, 'a'),
         {{"xyz.openbmc_project.Test.Service." + std::string(12, 'A'),
           ifaces}}},
        {"/xyz/openbmc_project/test/object/" + std::string(20, 'b'),
         {{"xyz.openbmc_project.Test.Service." + std::string(14, 'B'),
           {"xyz.openbmc_project.Test.Interface.Secondary"}}}}};
    conn.nextSubTree = expected;

    pldm::utils::coGetSubTree subTree(path, 2, ifaces);
    EXPECT_TRUE(subTree.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(subTree.await_resume(), expected);

    conn.nextSubTree = {};
    pldm::utils::coGetSubTree emptySubTree(path, 0, pldm::dbus::Interfaces{});
    EXPECT_TRUE(emptySubTree.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(emptySubTree.await_resume().empty());
}

TEST_F(DBusAsyncUtilsTest, coGetDbusPropertyCtorBadAllocCoverage)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(96, 'p');
    const std::string property = "Property" + std::string(96, 'r');
    const std::string iface =
        "xyz.openbmc_project.Test." + std::string(96, 'i');
    const std::string service =
        "xyz.openbmc_project.Test." + std::string(96, 's');

    EXPECT_TRUE(async_utils_test::exerciseBadAlloc([&] {
        pldm::utils::coGetDbusProperty<std::string> prop(path, property, iface,
                                                         service);
        (void)prop;
    }));
}

TEST_F(DBusAsyncUtilsTest,
       coGetDbusPropertyCtorBadAllocCoverageForRemainingHeapTypes)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(88, 'p');
    const std::string property = "Property" + std::string(88, 'r');
    const std::string iface =
        "xyz.openbmc_project.Test." + std::string(88, 'i');
    const std::string service =
        "xyz.openbmc_project.Test." + std::string(88, 's');

    EXPECT_TRUE(async_utils_test::exerciseBadAlloc([&] {
        pldm::utils::coGetDbusProperty<std::vector<std::string>> prop(
            path, property + "Parents", iface, service);
        (void)prop;
    }));

    EXPECT_TRUE(async_utils_test::exerciseBadAlloc([&] {
        pldm::utils::coGetDbusProperty<Associations> prop(
            path, property + "Associations", iface, service);
        (void)prop;
    }));

    EXPECT_TRUE(async_utils_test::exerciseBadAlloc([&] {
        pldm::utils::coGetDbusProperty<std::vector<uint8_t>> prop(
            path, property + "RawData", iface, service);
        (void)prop;
    }));

    EXPECT_TRUE(async_utils_test::exerciseBadAlloc([&] {
        pldm::utils::coGetDbusProperty<std::vector<uint64_t>> prop(
            path, property + "Counters", iface, service);
        (void)prop;
    }));

    EXPECT_TRUE(async_utils_test::exerciseBadAlloc([&] {
        pldm::utils::coGetDbusProperty<ObjectPaths> prop(
            path, property + "ObjectPaths", iface, service);
        (void)prop;
    }));
}

TEST_F(DBusAsyncUtilsTest,
       coGetDbusPropertyAwaitSuspendAndResumeBadAllocCoverage)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(96, 'p');
    const std::string property = "Property" + std::string(96, 'r');
    const std::string iface =
        "xyz.openbmc_project.Test." + std::string(96, 'i');
    auto& conn = connection();
    const std::string longValue(192, 'v');
    conn.nextPropertyValue = longValue;

    EXPECT_TRUE(async_utils_test::exerciseBadAlloc([&] {
        pldm::utils::coGetDbusProperty<std::string> prop(path, property, iface);
        (void)prop.await_suspend(std::noop_coroutine());
    }));

    EXPECT_TRUE(async_utils_test::exerciseBadAlloc([&] {
        pldm::utils::coGetDbusProperty<std::string> prop(path, property, iface);
        prop.ret = longValue;
        auto copy = prop.await_resume();
        (void)copy;
    }));
}

TEST_F(DBusAsyncUtilsTest,
       coGetDbusPropertyAwaitSuspendAndResumeBadAllocCoverageForHeapTypes)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(84, 'p');
    const std::string property = "Property" + std::string(84, 'r');
    const std::string iface =
        "xyz.openbmc_project.Test." + std::string(84, 'i');
    auto& conn = connection();

    const std::vector<std::string> names{std::string(48, 'a'),
                                         std::string(52, 'b')};
    conn.nextPropertyValue = names;
    EXPECT_TRUE(async_utils_test::exerciseBadAlloc([&] {
        pldm::utils::coGetDbusProperty<std::vector<std::string>> prop(
            path, property + "Names", iface);
        (void)prop.await_suspend(std::noop_coroutine());
    }));
    EXPECT_TRUE(async_utils_test::exerciseBadAlloc([&] {
        pldm::utils::coGetDbusProperty<std::vector<std::string>> prop(
            path, property + "Names", iface);
        prop.ret = names;
        auto copy = prop.await_resume();
        (void)copy;
    }));

    const Associations associations{
        {"forward-" + std::string(40, 'f'), "reverse-" + std::string(40, 'r'),
         "/xyz/openbmc_project/inventory/" + std::string(56, 'p')}};
    conn.nextPropertyValue = associations;
    EXPECT_TRUE(async_utils_test::exerciseBadAlloc([&] {
        pldm::utils::coGetDbusProperty<Associations> prop(
            path, property + "Associations", iface);
        (void)prop.await_suspend(std::noop_coroutine());
    }));
    EXPECT_TRUE(async_utils_test::exerciseBadAlloc([&] {
        pldm::utils::coGetDbusProperty<Associations> prop(
            path, property + "Associations", iface);
        prop.ret = associations;
        auto copy = prop.await_resume();
        (void)copy;
    }));

    const std::vector<uint8_t> rawData(80, 0x5A);
    conn.nextPropertyValue = rawData;
    EXPECT_TRUE(async_utils_test::exerciseBadAlloc([&] {
        pldm::utils::coGetDbusProperty<std::vector<uint8_t>> prop(
            path, property + "RawData", iface);
        (void)prop.await_suspend(std::noop_coroutine());
    }));
    EXPECT_TRUE(async_utils_test::exerciseBadAlloc([&] {
        pldm::utils::coGetDbusProperty<std::vector<uint8_t>> prop(
            path, property + "RawData", iface);
        prop.ret = rawData;
        auto copy = prop.await_resume();
        (void)copy;
    }));

    const std::vector<uint64_t> counters{1, 10, 100, 1000, 10000, 100000};
    conn.nextPropertyValue = counters;
    EXPECT_TRUE(async_utils_test::exerciseBadAlloc([&] {
        pldm::utils::coGetDbusProperty<std::vector<uint64_t>> prop(
            path, property + "Counters", iface);
        (void)prop.await_suspend(std::noop_coroutine());
    }));
    EXPECT_TRUE(async_utils_test::exerciseBadAlloc([&] {
        pldm::utils::coGetDbusProperty<std::vector<uint64_t>> prop(
            path, property + "Counters", iface);
        prop.ret = counters;
        auto copy = prop.await_resume();
        (void)copy;
    }));

    const ObjectPaths objectPaths{
        sdbusplus::object_path(
            "/xyz/openbmc_project/object/" + std::string(40, 'x')),
        sdbusplus::object_path(
            "/xyz/openbmc_project/object/" + std::string(44, 'y'))};
    conn.nextPropertyValue = objectPaths;
    EXPECT_TRUE(async_utils_test::exerciseBadAlloc([&] {
        pldm::utils::coGetDbusProperty<ObjectPaths> prop(
            path, property + "ObjectPaths", iface);
        (void)prop.await_suspend(std::noop_coroutine());
    }));
    EXPECT_TRUE(async_utils_test::exerciseBadAlloc([&] {
        pldm::utils::coGetDbusProperty<ObjectPaths> prop(
            path, property + "ObjectPaths", iface);
        prop.ret = objectPaths;
        auto copy = prop.await_resume();
        (void)copy;
    }));
}

TEST_F(DBusAsyncUtilsTest,
       coGetDbusPropertyExhaustiveBadAllocCoverageForHeapTypes)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(120, 'p');
    const std::string property = "Property" + std::string(112, 'r');
    const std::string iface =
        "xyz.openbmc_project.Test." + std::string(108, 'i');
    const std::string service =
        "xyz.openbmc_project.Test." + std::string(116, 's');

    const std::string longString(192, 'v');
    expectPropertyLifecycleExhaustiveBadAlloc(path, property + "Name", iface,
                                              service, longString);

    const std::vector<std::string> names{
        "/xyz/openbmc_project/inventory/" + std::string(72, 'a'),
        "/xyz/openbmc_project/inventory/" + std::string(76, 'b')};
    expectPropertyLifecycleExhaustiveBadAlloc(path, property + "Parents", iface,
                                              service, names);

    const Associations associations{
        {"forward-" + std::string(44, 'f'), "reverse-" + std::string(46, 'r'),
         "/xyz/openbmc_project/inventory/" + std::string(80, 'p')}};
    expectPropertyLifecycleExhaustiveBadAlloc(path, property + "Associations",
                                              iface, service, associations);

    const std::vector<uint8_t> rawData(112, 0x5A);
    expectPropertyLifecycleExhaustiveBadAlloc(path, property + "RawData", iface,
                                              service, rawData);

    const std::vector<uint64_t> counters{1,     10,     100,    1000,
                                         10000, 100000, 1000000};
    expectPropertyLifecycleExhaustiveBadAlloc(path, property + "Counters",
                                              iface, service, counters);

    const ObjectPaths objectPaths{
        sdbusplus::object_path(
            "/xyz/openbmc_project/object/" + std::string(72, 'x')),
        sdbusplus::object_path(
            "/xyz/openbmc_project/object/" + std::string(76, 'y'))};
    expectPropertyLifecycleExhaustiveBadAlloc(path, property + "ObjectPaths",
                                              iface, service, objectPaths);
}

TEST_F(DBusAsyncUtilsTest,
       coGetDbusPropertyExhaustiveBadAllocCoverageForUnsignedLong)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(104, 'u');
    const std::string property = "UnsignedLong" + std::string(96, 'n');
    const std::string iface =
        "xyz.openbmc_project.Test." + std::string(100, 'I');
    const std::string service =
        "xyz.openbmc_project.Test." + std::string(108, 'S');

    expectPropertyLifecycleExhaustiveBadAlloc(path, property, iface, service,
                                              uint64_t{0x12345678});
}

TEST_F(DBusAsyncUtilsTest,
       coGetDbusPropertyExhaustiveBadAllocCoverageForRemainingScalarTypes)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(108, 's');
    const std::string property = "Scalar" + std::string(100, 'p');
    const std::string iface =
        "xyz.openbmc_project.Test." + std::string(104, 'I');
    const std::string service =
        "xyz.openbmc_project.Test." + std::string(112, 'S');

    expectPropertyLifecycleExhaustiveBadAlloc(path, property + "Bool", iface,
                                              service, true);
    expectPropertyLifecycleExhaustiveBadAlloc(path, property + "U8", iface,
                                              service, static_cast<uint8_t>(7));
    expectPropertyLifecycleExhaustiveBadAlloc(
        path, property + "I16", iface, service, static_cast<int16_t>(-17));
    expectPropertyLifecycleExhaustiveBadAlloc(
        path, property + "U16", iface, service, static_cast<uint16_t>(23));
    expectPropertyLifecycleExhaustiveBadAlloc(
        path, property + "I32", iface, service, static_cast<int32_t>(-101));
    expectPropertyLifecycleExhaustiveBadAlloc(
        path, property + "U32", iface, service, static_cast<uint32_t>(101));
    expectPropertyLifecycleExhaustiveBadAlloc(
        path, property + "I64", iface, service, static_cast<int64_t>(-1001));
    expectPropertyLifecycleExhaustiveBadAlloc(path, property + "Double", iface,
                                              service, 12.5);
}

TEST_F(DBusAsyncUtilsTest,
       coGetDbusPropertyErrorPathExhaustiveBadAllocCoverageForHeapTypes)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(128, 'e');
    const std::string property = "Error" + std::string(120, 'P');
    const std::string iface =
        "xyz.openbmc_project.Test." + std::string(112, 'E');
    const std::string service =
        "xyz.openbmc_project.Test." + std::string(124, 'S');

    expectPropertyErrorPathExhaustiveBadAlloc<std::string>(
        path, property + "Name", iface, service);
    expectPropertyErrorPathExhaustiveBadAlloc<std::vector<std::string>>(
        path, property + "Names", iface, service);
    expectPropertyErrorPathExhaustiveBadAlloc<Associations>(
        path, property + "Associations", iface, service);
    expectPropertyErrorPathExhaustiveBadAlloc<std::vector<uint8_t>>(
        path, property + "RawData", iface, service);
    expectPropertyErrorPathExhaustiveBadAlloc<std::vector<uint64_t>>(
        path, property + "Counters", iface, service);
    expectPropertyErrorPathExhaustiveBadAlloc<ObjectPaths>(
        path, property + "Paths", iface, service);
}

TEST_F(DBusAsyncUtilsTest,
       coGetDbusPropertyErrorPathExhaustiveBadAllocCoverageForScalarTypes)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(124, 's');
    const std::string property = "ScalarError" + std::string(116, 'P');
    const std::string iface =
        "xyz.openbmc_project.Test." + std::string(110, 'I');
    const std::string service =
        "xyz.openbmc_project.Test." + std::string(122, 'S');

    expectPropertyErrorPathExhaustiveBadAlloc<bool>(path, property + "Bool",
                                                    iface, service);
    expectPropertyErrorPathExhaustiveBadAlloc<uint8_t>(path, property + "U8",
                                                       iface, service);
    expectPropertyErrorPathExhaustiveBadAlloc<int16_t>(path, property + "I16",
                                                       iface, service);
    expectPropertyErrorPathExhaustiveBadAlloc<uint16_t>(path, property + "U16",
                                                        iface, service);
    expectPropertyErrorPathExhaustiveBadAlloc<int32_t>(path, property + "I32",
                                                       iface, service);
    expectPropertyErrorPathExhaustiveBadAlloc<uint32_t>(path, property + "U32",
                                                        iface, service);
    expectPropertyErrorPathExhaustiveBadAlloc<int64_t>(path, property + "I64",
                                                       iface, service);
    expectPropertyErrorPathExhaustiveBadAlloc<uint64_t>(path, property + "U64",
                                                        iface, service);
    expectPropertyErrorPathExhaustiveBadAlloc<double>(path, property + "Double",
                                                      iface, service);
}

TEST_F(DBusAsyncUtilsTest,
       coGetDbusPropertyDefaultServiceCtorExhaustiveBadAllocCoverage)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(124, 'd');
    const std::string property = "Default" + std::string(116, 'P');
    const std::string iface =
        "xyz.openbmc_project.Test." + std::string(112, 'I');

    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<std::string> prop(
            path, property + "Name", iface);
        (void)prop;
    }));
    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<std::vector<std::string>> prop(
            path, property + "Names", iface);
        (void)prop;
    }));
    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<Associations> prop(
            path, property + "Associations", iface);
        (void)prop;
    }));
    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<ObjectPaths> prop(
            path, property + "Paths", iface);
        (void)prop;
    }));
}

TEST_F(DBusAsyncUtilsTest, coGetServiceMapAwaitResumeLargeResultCoverage)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(96, 'm');
    const pldm::dbus::Interfaces ifaces{
        "xyz.openbmc_project.Test.Interface." + std::string(48, 'I'),
        "xyz.openbmc_project.Test.Interface." + std::string(52, 'J')};
    const pldm::utils::MapperServiceMap expected{
        {"xyz.openbmc_project.Test.Service." + std::string(48, 'A'), ifaces},
        {"xyz.openbmc_project.Test.Service." + std::string(52, 'B'),
         {"xyz.openbmc_project.Test.Interface." + std::string(44, 'K')}}};
    pldm::utils::coGetServiceMap serviceMap(path, ifaces);
    serviceMap.ret = expected;
    auto copy = serviceMap.await_resume();
    EXPECT_EQ(copy, expected);
}

TEST_F(DBusAsyncUtilsTest, coGetSubTreeAwaitResumeLargeResultCoverage)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(96, 't');
    const pldm::dbus::Interfaces ifaces{
        "xyz.openbmc_project.Test.Interface." + std::string(46, 'P'),
        "xyz.openbmc_project.Test.Interface." + std::string(50, 'Q')};
    const pldm::utils::GetSubTreeResponse expected{
        {"/xyz/openbmc_project/test/object/" + std::string(56, 'a'),
         {{"xyz.openbmc_project.Test.Service." + std::string(32, 'A'),
           ifaces}}},
        {"/xyz/openbmc_project/test/object/" + std::string(60, 'b'),
         {{"xyz.openbmc_project.Test.Service." + std::string(36, 'B'),
           {"xyz.openbmc_project.Test.Interface." + std::string(40, 'R')}}}}};
    pldm::utils::coGetSubTree subTree(path, 3, ifaces);
    subTree.ret = expected;
    auto copy = subTree.await_resume();
    EXPECT_EQ(copy, expected);
}

TEST_F(DBusAsyncUtilsTest, coGetServiceMapCtorAndAwaitCoverage)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(96, 'm');
    const pldm::dbus::Interfaces ifaces{
        "xyz.openbmc_project.Test.Interface." + std::string(48, 'I'),
        "xyz.openbmc_project.Test.Interface." + std::string(52, 'J')};
    auto& conn = connection();
    conn.nextServiceMap = {
        {"xyz.openbmc_project.Test.Service." + std::string(40, 'A'), ifaces},
        {"xyz.openbmc_project.Test.Service." + std::string(44, 'B'),
         {"xyz.openbmc_project.Test.Interface." + std::string(36, 'K')}}};

    pldm::utils::coGetServiceMap op(path, ifaces);
    EXPECT_FALSE(op.await_ready());
    EXPECT_TRUE(op.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(op.await_resume(), conn.nextServiceMap);

    op.ret = {};
    EXPECT_TRUE(op.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(op.await_resume(), conn.nextServiceMap);
}

TEST_F(DBusAsyncUtilsTest, coGetSubTreeCtorAndAwaitCoverage)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(96, 't');
    const pldm::dbus::Interfaces ifaces{
        "xyz.openbmc_project.Test.Interface." + std::string(46, 'P'),
        "xyz.openbmc_project.Test.Interface." + std::string(50, 'Q')};
    auto& conn = connection();
    conn.nextSubTree = {
        {"/xyz/openbmc_project/test/object/" + std::string(56, 'a'),
         {{"xyz.openbmc_project.Test.Service." + std::string(32, 'A'),
           ifaces}}},
        {"/xyz/openbmc_project/test/object/" + std::string(60, 'b'),
         {{"xyz.openbmc_project.Test.Service." + std::string(36, 'B'),
           {"xyz.openbmc_project.Test.Interface." + std::string(40, 'R')}}}}};

    pldm::utils::coGetSubTree op(path, 3, ifaces);
    EXPECT_FALSE(op.await_ready());
    EXPECT_TRUE(op.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(op.await_resume(), conn.nextSubTree);

    op.ret = {};
    EXPECT_TRUE(op.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(op.await_resume(), conn.nextSubTree);
}

// Death tests intentionally abort under injected allocation failures; the
// runtime test pass covers them and the analyzer cannot reason about them.
#ifndef __clang_analyzer__
TEST_F(DBusAsyncUtilsTest, coGetServiceMapAwaitSuspendDiesOnBadAllocDuringCopy)
{
    if (pldm::test::runningOnValgrind())
    {
        GTEST_SKIP() << "allocation-failure death coverage runs in the normal "
                        "pass";
    }

    const std::string path = "/xyz/openbmc_project/test/object";
    const pldm::dbus::Interfaces ifaces{
        "xyz.openbmc_project.Test.Interface." + std::string(48, 'I'),
        "xyz.openbmc_project.Test.Interface." + std::string(52, 'J')};
    auto& conn = connection();
    conn.nextServiceMap = {
        {"xyz.openbmc_project.Test.Service." + std::string(40, 'A'), ifaces},
        {"xyz.openbmc_project.Test.Service." + std::string(44, 'B'),
         {"xyz.openbmc_project.Test.Interface." + std::string(36, 'K')}}};

    pldm::utils::coGetServiceMap op(path, ifaces);
    EXPECT_DEATH(
        {
            async_utils_test::ScopedAllocationFailure failure(1);
            (void)op.await_suspend(std::noop_coroutine());
        },
        "bad_alloc");
}

TEST_F(DBusAsyncUtilsTest, coGetSubTreeAwaitSuspendDiesOnBadAllocDuringCopy)
{
    if (pldm::test::runningOnValgrind())
    {
        GTEST_SKIP() << "allocation-failure death coverage runs in the normal "
                        "pass";
    }

    const std::string path = "/xyz/openbmc_project/test";
    const pldm::dbus::Interfaces ifaces{
        "xyz.openbmc_project.Test.Interface." + std::string(46, 'P'),
        "xyz.openbmc_project.Test.Interface." + std::string(50, 'Q')};
    auto& conn = connection();
    conn.nextSubTree = {
        {"/xyz/openbmc_project/test/object/" + std::string(56, 'a'),
         {{"xyz.openbmc_project.Test.Service." + std::string(32, 'A'),
           ifaces}}},
        {"/xyz/openbmc_project/test/object/" + std::string(60, 'b'),
         {{"xyz.openbmc_project.Test.Service." + std::string(36, 'B'),
           {"xyz.openbmc_project.Test.Interface." + std::string(40, 'R')}}}}};

    pldm::utils::coGetSubTree op(path, 3, ifaces);
    EXPECT_DEATH(
        {
            async_utils_test::ScopedAllocationFailure failure(1);
            (void)op.await_suspend(std::noop_coroutine());
        },
        "bad_alloc");
}

TEST_F(DBusAsyncUtilsTest, coGetServiceMapAwaitResumeDiesOnBadAllocOnCopy)
{
    if (pldm::test::runningOnValgrind())
    {
        GTEST_SKIP() << "allocation-failure death coverage runs in the normal "
                        "pass";
    }

    const std::string path = "/xyz/openbmc_project/test/object";
    const pldm::dbus::Interfaces ifaces{
        "xyz.openbmc_project.Test.Interface." + std::string(48, 'S'),
        "xyz.openbmc_project.Test.Interface." + std::string(52, 'T')};
    const pldm::utils::MapperServiceMap expected{
        {"xyz.openbmc_project.Test.Service." + std::string(40, 'C'), ifaces},
        {"xyz.openbmc_project.Test.Service." + std::string(44, 'D'),
         {"xyz.openbmc_project.Test.Interface." + std::string(36, 'U')}}};

    pldm::utils::coGetServiceMap op(path, ifaces);
    op.ret = expected;

    EXPECT_DEATH(
        {
            async_utils_test::ScopedAllocationFailure failure(1);
            auto copy = op.await_resume();
            (void)copy;
        },
        "bad_alloc");
}

TEST_F(DBusAsyncUtilsTest, coGetSubTreeAwaitResumeDiesOnBadAllocOnCopy)
{
    if (pldm::test::runningOnValgrind())
    {
        GTEST_SKIP() << "allocation-failure death coverage runs in the normal "
                        "pass";
    }

    const std::string path = "/xyz/openbmc_project/test";
    const pldm::dbus::Interfaces ifaces{
        "xyz.openbmc_project.Test.Interface." + std::string(46, 'V'),
        "xyz.openbmc_project.Test.Interface." + std::string(50, 'W')};
    const pldm::utils::GetSubTreeResponse expected{
        {"/xyz/openbmc_project/test/object/" + std::string(56, 'c'),
         {{"xyz.openbmc_project.Test.Service." + std::string(32, 'E'),
           ifaces}}},
        {"/xyz/openbmc_project/test/object/" + std::string(60, 'd'),
         {{"xyz.openbmc_project.Test.Service." + std::string(36, 'F'),
           {"xyz.openbmc_project.Test.Interface." + std::string(40, 'X')}}}}};

    pldm::utils::coGetSubTree op(path, 4, ifaces);
    op.ret = expected;

    EXPECT_DEATH(
        {
            async_utils_test::ScopedAllocationFailure failure(1);
            auto copy = op.await_resume();
            (void)copy;
        },
        "bad_alloc");
}
#endif

TEST_F(DBusAsyncUtilsTest, coGetServiceMapAndSubTreeReturnEmptyOnLongErrorPaths)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(72, 'e');
    const pldm::dbus::Interfaces ifaces{
        "xyz.openbmc_project.Test.Interface." + std::string(28, 'I'),
        "xyz.openbmc_project.Test.Interface." + std::string(32, 'J')};
    auto& conn = connection();

    conn.nextServiceMapError = boost::system::errc::make_error_code(
        boost::system::errc::network_unreachable);
    pldm::utils::coGetServiceMap serviceMap(path, ifaces);
    EXPECT_TRUE(serviceMap.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(serviceMap.await_resume().empty());

    conn.nextSubTreeError = boost::system::errc::make_error_code(
        boost::system::errc::host_unreachable);
    pldm::utils::coGetSubTree subTree(path, 4, ifaces);
    EXPECT_TRUE(subTree.await_suspend(std::noop_coroutine()));
    EXPECT_TRUE(subTree.await_resume().empty());
}

TEST_F(DBusAsyncUtilsTest, coGetDbusPropertyCoversRemainingScalarTypes)
{
    constexpr auto path = "/xyz/openbmc_project/test/object";
    constexpr auto iface = "xyz.openbmc_project.Test.Interface";
    auto& conn = connection();

    conn.nextPropertyValue = static_cast<uint8_t>(7);
    pldm::utils::coGetDbusProperty<uint8_t> u8Prop(path, "U8", iface);
    EXPECT_TRUE(u8Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(u8Prop.await_resume(), static_cast<uint8_t>(7));

    conn.nextPropertyValue = static_cast<int16_t>(-17);
    pldm::utils::coGetDbusProperty<int16_t> i16Prop(path, "I16", iface);
    EXPECT_TRUE(i16Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(i16Prop.await_resume(), static_cast<int16_t>(-17));

    conn.nextPropertyValue = static_cast<uint16_t>(23);
    pldm::utils::coGetDbusProperty<uint16_t> u16Prop(path, "U16", iface);
    EXPECT_TRUE(u16Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(u16Prop.await_resume(), static_cast<uint16_t>(23));

    conn.nextPropertyValue = static_cast<int32_t>(-101);
    pldm::utils::coGetDbusProperty<int32_t> i32Prop(path, "I32", iface);
    EXPECT_TRUE(i32Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(i32Prop.await_resume(), static_cast<int32_t>(-101));

    conn.nextPropertyValue = static_cast<uint32_t>(101);
    pldm::utils::coGetDbusProperty<uint32_t> u32Prop(path, "U32", iface);
    EXPECT_TRUE(u32Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(u32Prop.await_resume(), static_cast<uint32_t>(101));

    conn.nextPropertyValue = static_cast<int64_t>(-1001);
    pldm::utils::coGetDbusProperty<int64_t> i64Prop(path, "I64", iface);
    EXPECT_TRUE(i64Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(i64Prop.await_resume(), static_cast<int64_t>(-1001));

    conn.nextPropertyValue = 12.5;
    pldm::utils::coGetDbusProperty<double> doubleProp(path, "Double", iface);
    EXPECT_TRUE(doubleProp.await_suspend(std::noop_coroutine()));
    EXPECT_DOUBLE_EQ(doubleProp.await_resume(), 12.5);
}

TEST_F(DBusAsyncUtilsTest, coGetDbusPropertyReturnsScalarDefaultsOnError)
{
    constexpr auto path = "/xyz/openbmc_project/test/object";
    constexpr auto iface = "xyz.openbmc_project.Test.Interface";
    auto& conn = connection();
    conn.nextPropertyError = boost::system::errc::make_error_code(
        boost::system::errc::permission_denied);

    pldm::utils::coGetDbusProperty<uint8_t> u8Prop(path, "U8", iface);
    EXPECT_TRUE(u8Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(u8Prop.await_resume(), static_cast<uint8_t>(0));

    pldm::utils::coGetDbusProperty<int16_t> i16Prop(path, "I16", iface);
    EXPECT_TRUE(i16Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(i16Prop.await_resume(), static_cast<int16_t>(0));

    pldm::utils::coGetDbusProperty<uint16_t> u16Prop(path, "U16", iface);
    EXPECT_TRUE(u16Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(u16Prop.await_resume(), static_cast<uint16_t>(0));

    pldm::utils::coGetDbusProperty<int32_t> i32Prop(path, "I32", iface);
    EXPECT_TRUE(i32Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(i32Prop.await_resume(), static_cast<int32_t>(0));

    pldm::utils::coGetDbusProperty<uint32_t> u32Prop(path, "U32", iface);
    EXPECT_TRUE(u32Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(u32Prop.await_resume(), static_cast<uint32_t>(0));

    pldm::utils::coGetDbusProperty<int64_t> i64Prop(path, "I64", iface);
    EXPECT_TRUE(i64Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(i64Prop.await_resume(), static_cast<int64_t>(0));

    pldm::utils::coGetDbusProperty<double> doubleProp(path, "Double", iface);
    EXPECT_TRUE(doubleProp.await_suspend(std::noop_coroutine()));
    EXPECT_DOUBLE_EQ(doubleProp.await_resume(), 0.0);
}

TEST_F(DBusAsyncUtilsTest,
       coGetDbusPropertyReturnsDefaultOnRemainingScalarMismatches)
{
    // Exercises the variant-mismatch -> default-value path for additional
    // numeric source types (different from the std::string("wrong") cases
    // above). Confirms the catch-and-default behavior is uniform across
    // any source variant that doesn't match the requested type.
    constexpr auto path = "/xyz/openbmc_project/test/object";
    constexpr auto iface = "xyz.openbmc_project.Test.Interface";
    auto& conn = connection();

    conn.nextPropertyValue = std::string("wrong-u8");
    pldm::utils::coGetDbusProperty<uint8_t> u8Prop(path, "U8", iface);
    EXPECT_TRUE(u8Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(u8Prop.await_resume(), static_cast<uint8_t>(0));

    conn.nextPropertyValue = true;
    pldm::utils::coGetDbusProperty<int16_t> i16Prop(path, "I16", iface);
    EXPECT_TRUE(i16Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(i16Prop.await_resume(), static_cast<int16_t>(0));

    conn.nextPropertyValue = static_cast<int64_t>(77);
    pldm::utils::coGetDbusProperty<uint16_t> u16Prop(path, "U16", iface);
    EXPECT_TRUE(u16Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(u16Prop.await_resume(), static_cast<uint16_t>(0));

    conn.nextPropertyValue = static_cast<uint32_t>(88);
    pldm::utils::coGetDbusProperty<int32_t> i32Prop(path, "I32", iface);
    EXPECT_TRUE(i32Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(i32Prop.await_resume(), static_cast<int32_t>(0));

    conn.nextPropertyValue = static_cast<int32_t>(-99);
    pldm::utils::coGetDbusProperty<uint32_t> u32Prop(path, "U32", iface);
    EXPECT_TRUE(u32Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(u32Prop.await_resume(), static_cast<uint32_t>(0));

    conn.nextPropertyValue = static_cast<uint16_t>(123);
    pldm::utils::coGetDbusProperty<int64_t> i64Prop(path, "I64", iface);
    EXPECT_TRUE(i64Prop.await_suspend(std::noop_coroutine()));
    EXPECT_EQ(i64Prop.await_resume(), static_cast<int64_t>(0));

    conn.nextPropertyValue = static_cast<uint64_t>(456);
    pldm::utils::coGetDbusProperty<double> doubleProp(path, "Double", iface);
    EXPECT_TRUE(doubleProp.await_suspend(std::noop_coroutine()));
    EXPECT_DOUBLE_EQ(doubleProp.await_resume(), 0.0);
}

TEST_F(DBusAsyncUtilsTest,
       coGetDbusPropertyDefaultServiceCtorCoversRemainingScalarTypes)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(20, 's');
    const std::string iface =
        "xyz.openbmc_project.Test." + std::string(18, 'T');

    pldm::utils::coGetDbusProperty<std::string> nameProp(path, "Name", iface);
    EXPECT_EQ(nameProp.service, pldm::utils::entityManagerService);
    EXPECT_EQ(nameProp.objectPath, path);
    EXPECT_EQ(nameProp.interface, iface);
    EXPECT_EQ(nameProp.property, "Name");
    EXPECT_TRUE(nameProp.await_resume().empty());

    pldm::utils::coGetDbusProperty<bool> boolProp(path, "Present", iface);
    EXPECT_EQ(boolProp.service, pldm::utils::entityManagerService);
    EXPECT_FALSE(boolProp.await_resume());

    pldm::utils::coGetDbusProperty<uint8_t> u8Prop(path, "U8", iface);
    EXPECT_EQ(u8Prop.service, pldm::utils::entityManagerService);
    EXPECT_EQ(u8Prop.await_resume(), static_cast<uint8_t>(0));

    pldm::utils::coGetDbusProperty<int16_t> i16Prop(path, "I16", iface);
    EXPECT_EQ(i16Prop.service, pldm::utils::entityManagerService);
    EXPECT_EQ(i16Prop.await_resume(), static_cast<int16_t>(0));

    pldm::utils::coGetDbusProperty<uint16_t> u16Prop(path, "U16", iface);
    EXPECT_EQ(u16Prop.service, pldm::utils::entityManagerService);
    EXPECT_EQ(u16Prop.await_resume(), static_cast<uint16_t>(0));

    pldm::utils::coGetDbusProperty<int32_t> i32Prop(path, "I32", iface);
    EXPECT_EQ(i32Prop.service, pldm::utils::entityManagerService);
    EXPECT_EQ(i32Prop.await_resume(), static_cast<int32_t>(0));

    pldm::utils::coGetDbusProperty<uint32_t> u32Prop(path, "U32", iface);
    EXPECT_EQ(u32Prop.service, pldm::utils::entityManagerService);
    EXPECT_EQ(u32Prop.await_resume(), static_cast<uint32_t>(0));

    pldm::utils::coGetDbusProperty<int64_t> i64Prop(path, "I64", iface);
    EXPECT_EQ(i64Prop.service, pldm::utils::entityManagerService);
    EXPECT_EQ(i64Prop.await_resume(), static_cast<int64_t>(0));

    pldm::utils::coGetDbusProperty<uint64_t> u64Prop(path, "U64", iface);
    EXPECT_EQ(u64Prop.service, pldm::utils::entityManagerService);
    EXPECT_EQ(u64Prop.await_resume(), static_cast<uint64_t>(0));

    pldm::utils::coGetDbusProperty<double> doubleProp(path, "Double", iface);
    EXPECT_EQ(doubleProp.service, pldm::utils::entityManagerService);
    EXPECT_DOUBLE_EQ(doubleProp.await_resume(), 0.0);
}

TEST_F(
    DBusAsyncUtilsTest,
    coGetDbusPropertyDefaultServiceCtorExhaustiveBadAllocCoverageForRemainingTypes)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(128, 'r');
    const std::string property = "Remaining" + std::string(112, 'P');
    const std::string iface =
        "xyz.openbmc_project.Test." + std::string(108, 'I');

    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<bool> prop(path, property + "Bool",
                                                  iface);
        (void)prop;
    }));
    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<uint8_t> prop(path, property + "U8",
                                                     iface);
        (void)prop;
    }));
    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<int16_t> prop(path, property + "I16",
                                                     iface);
        (void)prop;
    }));
    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<uint16_t> prop(path, property + "U16",
                                                      iface);
        (void)prop;
    }));
    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<int32_t> prop(path, property + "I32",
                                                     iface);
        (void)prop;
    }));
    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<uint32_t> prop(path, property + "U32",
                                                      iface);
        (void)prop;
    }));
    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<int64_t> prop(path, property + "I64",
                                                     iface);
        (void)prop;
    }));
    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<uint64_t> prop(path, property + "U64",
                                                      iface);
        (void)prop;
    }));
    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<double> prop(path, property + "Double",
                                                    iface);
        (void)prop;
    }));
    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<std::vector<uint8_t>> prop(
            path, property + "RawData", iface);
        (void)prop;
    }));
    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc([&] {
        pldm::utils::coGetDbusProperty<std::vector<uint64_t>> prop(
            path, property + "Counters", iface);
        (void)prop;
    }));
}

TEST_F(DBusAsyncUtilsTest, coGetDbusPropertyLifecycleDeepBadAllocCoverage)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(176, 'l');
    const std::string property = "Lifecycle" + std::string(148, 'P');
    const std::string iface =
        "xyz.openbmc_project.Test." + std::string(144, 'I');
    const std::string service =
        "xyz.openbmc_project.Test." + std::string(152, 'S');

    expectPropertyLifecycleExhaustiveBadAlloc(
        path, property + "Name", iface, service, std::string(192, 'v'), 2048);
    expectPropertyLifecycleExhaustiveBadAlloc(
        path, property + "Associations", iface, service,
        Associations{
            {"forward-" + std::string(40, 'f'),
             "reverse-" + std::string(44, 'r'),
             "/xyz/openbmc_project/inventory/" + std::string(120, 'p')}},
        2048);
    expectPropertyLifecycleExhaustiveBadAlloc(
        path, property + "ObjectPaths", iface, service,
        ObjectPaths{sdbusplus::object_path(
                        "/xyz/openbmc_project/object/" + std::string(116, 'x')),
                    sdbusplus::object_path("/xyz/openbmc_project/object/" +
                                           std::string(124, 'y'))},
        2048);
}

TEST_F(DBusAsyncUtilsTest,
       coGetDbusPropertyDefaultServiceCtorDeepBadAllocCoverage)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(184, 'd');
    const std::string property = "DefaultDeep" + std::string(156, 'Q');
    const std::string iface =
        "xyz.openbmc_project.Test." + std::string(152, 'J');

    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::coGetDbusProperty<std::string> prop(
                path, property + "Name", iface);
            (void)prop;
        },
        2048));
    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::coGetDbusProperty<Associations> prop(
                path, property + "Associations", iface);
            (void)prop;
        },
        2048));
    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::coGetDbusProperty<ObjectPaths> prop(
                path, property + "Paths", iface);
            (void)prop;
        },
        2048));
}

TEST_F(DBusAsyncUtilsTest, coGetDbusPropertyRemainingCtorDeepBadAllocCoverage)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(192, 'p');
    const std::string property = "CtorCoverage" + std::string(164, 'R');
    const std::string iface =
        "xyz.openbmc_project.Test." + std::string(148, 'K');
    const std::string service =
        "xyz.openbmc_project.Test.Service." + std::string(144, 'S');

    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::coGetDbusProperty<bool> prop(path, property + "Bool",
                                                      iface, service);
            (void)prop;
        },
        2048));
    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::coGetDbusProperty<uint64_t> prop(
                path, property + "Uint64", iface, service);
            (void)prop;
        },
        2048));
    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::coGetDbusProperty<double> prop(
                path, property + "Double", iface);
            (void)prop;
        },
        2048));
    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::coGetDbusProperty<std::vector<uint8_t>> prop(
                path, property + "Bytes", iface, service);
            (void)prop;
        },
        2048));
    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::coGetDbusProperty<std::vector<uint64_t>> prop(
                path, property + "Counters", iface);
            (void)prop;
        },
        2048));
    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::coGetDbusProperty<std::vector<std::string>> prop(
                path, property + "Names", iface, service);
            (void)prop;
        },
        2048));
}

TEST_F(DBusAsyncUtilsTest, coGetServiceMapDeepLifecycleBadAllocCoverage)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(152, 'm');
    const pldm::dbus::Interfaces ifaces{
        "xyz.openbmc_project.Test.Interface." + std::string(88, 'A'),
        "xyz.openbmc_project.Test.Interface." + std::string(92, 'B'),
        "xyz.openbmc_project.Test.Interface." + std::string(96, 'C')};
    const pldm::utils::MapperServiceMap value{
        {"xyz.openbmc_project.Test.Service." + std::string(88, 'S'), ifaces},
        {"xyz.openbmc_project.Test.Service." + std::string(92, 'T'),
         {"xyz.openbmc_project.Test.Interface." + std::string(84, 'U'),
          "xyz.openbmc_project.Test.Interface." + std::string(86, 'V')}}};

    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::coGetServiceMap op(path, ifaces);
            op.ret = value;
        },
        2048));
}

TEST_F(DBusAsyncUtilsTest, coGetSubTreeDeepLifecycleBadAllocCoverage)
{
    const std::string path =
        "/xyz/openbmc_project/test/" + std::string(156, 't');
    const pldm::dbus::Interfaces ifaces{
        "xyz.openbmc_project.Test.Interface." + std::string(82, 'L'),
        "xyz.openbmc_project.Test.Interface." + std::string(86, 'M')};
    const pldm::utils::GetSubTreeResponse value{
        {"/xyz/openbmc_project/test/object/" + std::string(94, 'a'),
         {{"xyz.openbmc_project.Test.Service." + std::string(80, 'A'),
           ifaces}}},
        {"/xyz/openbmc_project/test/object/" + std::string(98, 'b'),
         {{"xyz.openbmc_project.Test.Service." + std::string(84, 'B'),
           {"xyz.openbmc_project.Test.Interface." + std::string(78, 'N'),
            "xyz.openbmc_project.Test.Interface." + std::string(82, 'O')}}}}};

    EXPECT_TRUE(async_utils_test::exerciseAllBadAlloc(
        [&] {
            pldm::utils::coGetSubTree op(path, 5, ifaces);
            op.ret = value;
        },
        2048));
}

} // namespace
