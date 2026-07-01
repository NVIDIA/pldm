#include "libpldm/oem/nvidia/state_set_oem_nvidia.h"
#include "libpldm/platform.h"

#include "../../test/test_valgrind_utils.hpp"
#include "common/types.hpp"
#include "common/utils.hpp"
#include "platform-mc/state_sensor.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace state_set_nvidia_alloc
{

thread_local bool enabled = false;
thread_local std::size_t failAt = 0;
thread_local std::size_t allocationCount = 0;

bool shouldFail()
{
    return enabled && failAt != 0 && ++allocationCount == failAt;
}

void* allocate(std::size_t size,
               std::size_t alignment = alignof(std::max_align_t))
{
    if (shouldFail())
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

struct ScopedFailure
{
    explicit ScopedFailure(std::size_t failAllocation) :
        previousEnabled(enabled), previousFailAt(failAt),
        previousAllocationCount(allocationCount)
    {
        enabled = true;
        failAt = failAllocation;
        allocationCount = 0;
    }

    ~ScopedFailure()
    {
        enabled = previousEnabled;
        failAt = previousFailAt;
        allocationCount = previousAllocationCount;
    }

  private:
    bool previousEnabled;
    std::size_t previousFailAt;
    std::size_t previousAllocationCount;
};

} // namespace state_set_nvidia_alloc

void* operator new(std::size_t size)
{
    return state_set_nvidia_alloc::allocate(size);
}

void* operator new[](std::size_t size)
{
    return state_set_nvidia_alloc::allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    return state_set_nvidia_alloc::allocate(
        size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return state_set_nvidia_alloc::allocate(
        size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return state_set_nvidia_alloc::allocate(size);
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
        return state_set_nvidia_alloc::allocate(size);
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

using RealDBusHandler = DBusHandler;

struct NvlinkDbusMockState
{
    bool throwOnGetSubtree = false;
    bool throwOnGetDbusProperty = false;
    uint64_t instanceNumber = 0;
    GetSubTreeResponse subtreeResponse{};
};

class NvlinkTestDBusHandler
{
  public:
    static auto& getBus()
    {
        return RealDBusHandler::getBus();
    }

    static NvlinkDbusMockState& state()
    {
        static NvlinkDbusMockState mockState{};
        return mockState;
    }

    static void reset()
    {
        state() = {};
    }

    GetSubTreeResponse getSubtree(const std::string&, int,
                                  const std::vector<std::string>&) const
    {
        if (state().throwOnGetSubtree)
        {
            throw sdbusplus::exception::SdBusError(EIO, "mock getSubtree");
        }
        return state().subtreeResponse;
    }

    template <typename Property>
    Property getDbusProperty(const char*, const char*, const char*) const
    {
        if (state().throwOnGetDbusProperty)
        {
            throw std::runtime_error("mock getDbusProperty");
        }
        return static_cast<Property>(state().instanceNumber);
    }
};

} // namespace pldm::utils

#define DBusHandler NvlinkTestDBusHandler
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#define protected public
#include "oem/nvidia/platform-mc/state_set/memoryPerformance.hpp"
#include "oem/nvidia/platform-mc/state_set/memorySpareChannel.hpp"
#include "oem/nvidia/platform-mc/state_set/nvlink.hpp"
#include "oem/nvidia/platform-mc/state_set/processorPowerBreak.hpp"
#undef protected
#undef private
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#undef DBusHandler

using namespace pldm::platform_mc;

namespace
{

pldm::dbus::PathAssociation makeAssociation(const std::string& forward,
                                            const std::string& reverse,
                                            const std::string& path)
{
    return pldm::dbus::PathAssociation{forward, reverse, path};
}

StateSetInfo makeStateSetInfo(uint16_t entityType, uint16_t entityInstance,
                              const std::vector<StateSetData>& sets)
{
    return StateSetInfo{EntityInfo{1, entityType, entityInstance}, sets};
}

std::shared_ptr<StateSensor> makeStateSensor(
    uint8_t tid, uint16_t sensorId, uint16_t entityType,
    uint16_t entityInstance, uint16_t stateSetId,
    const std::string& associationPath)
{
    PossibleStates possibleStates{
        PLDM_STATESET_LINK_STATE_DISCONNECTED,
        PLDM_STATESET_LINK_STATE_CONNECTED,
        PLDM_STATESET_PRESENCE_PRESENT,
        PLDM_STATESET_PRESENCE_NOT_PRESENT,
        PLDM_STATESET_PERFORMANCE_NORMAL,
        PLDM_STATESET_PERFORMANCE_THROTTLED};
    StateSetInfo info = makeStateSetInfo(
        entityType, entityInstance,
        std::vector<StateSetData>{{stateSetId, possibleStates}});
    std::string assocPath = associationPath;
    return std::make_shared<StateSensor>(tid, false, sensorId, std::move(info),
                                         nullptr, assocPath, nullptr);
}

class StateSetNvlinkCoverage : public oem_nvidia::StateSetNvlink
{
  public:
    using oem_nvidia::StateSetNvlink::StateSetNvlink;

    Associations getAssociations() const
    {
        return associationDefinitionsIntf->associations();
    }

    auto& getAssociationDefinitionsIntf()
    {
        return associationDefinitionsIntf;
    }

    void setAssociations(const Associations& assocs)
    {
        associationDefinitionsIntf->associations(assocs);
    }
};

class StateSetMemoryPerformanceCoverage : public StateSetMemoryPerformance
{
  public:
    using StateSetMemoryPerformance::StateSetMemoryPerformance;

    void setAssociations(const Associations& assocs)
    {
        associationDefinitionsIntf->associations(assocs);
    }
};

class StateSetMemorySpareChannelCoverage : public StateSetMemorySpareChannel
{
  public:
    using StateSetMemorySpareChannel::StateSetMemorySpareChannel;

    void setAssociations(const Associations& assocs)
    {
        associationDefinitionsIntf->associations(assocs);
    }
};

class StateSetProcessorPowerBreakCoverage : public StateSetProcessorPowerBreak
{
  public:
    using StateSetProcessorPowerBreak::StateSetProcessorPowerBreak;

    void setAssociations(const Associations& assocs)
    {
        associationDefinitionsIntf->associations(assocs);
    }
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
            state_set_nvidia_alloc::ScopedFailure failure(failIndex);
            operation();
        }
        catch (const std::bad_alloc&)
        {
            return true;
        }
        catch (const std::exception&)
        {}
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
            state_set_nvidia_alloc::ScopedFailure failure(failIndex);
            operation();
        }
        catch (const std::bad_alloc&)
        {
            sawBadAlloc = true;
        }
        catch (const std::exception&)
        {}
    }

    return sawBadAlloc;
}

template <typename Operation>
bool exerciseSwallowedBadAlloc(Operation&& operation,
                               std::size_t maxFailAt = 128)
{
    if (pldm::test::runningOnValgrind())
    {
        return true;
    }

    for (std::size_t failIndex = 1; failIndex <= maxFailAt; ++failIndex)
    {
        try
        {
            state_set_nvidia_alloc::ScopedFailure failure(failIndex);
            operation();
            if (state_set_nvidia_alloc::allocationCount >= failIndex)
            {
                return true;
            }
        }
        catch (const std::bad_alloc&)
        {}
        catch (const std::exception&)
        {}
    }

    return false;
}

class StateSetNvlinkInternalTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        pldm::utils::NvlinkTestDBusHandler::reset();
    }
};

TEST_F(StateSetNvlinkInternalTest,
       setAssociationReturnsWhenAssociationDefinitionsAreMissing)
{
    auto sensor = makeStateSensor(
        8, 0x1508, PLDM_ENTITY_SYS_BUS, 8, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric8");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric8"}, false);

    auto initialAssociation = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis8/CPU_0");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_8";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    initialAssociation, *sensor);
    stateSet.getAssociationDefinitionsIntf().reset();

    std::vector<pldm::dbus::PathAssociation> associations{initialAssociation};
    stateSet.setAssociation(associations);

    EXPECT_EQ(nullptr, stateSet.endpointIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointInstanceIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointAssociationDefinitionsIntf.get());
}

TEST_F(StateSetNvlinkInternalTest,
       setAssociationReturnsWhenAssociationsAreEmpty)
{
    auto sensor = makeStateSensor(
        9, 0x1509, PLDM_ENTITY_SYS_BUS, 9, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric9");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric9"}, false);

    auto initialAssociation = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis9/CPU_0");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_9";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    initialAssociation, *sensor);

    std::vector<pldm::dbus::PathAssociation> associations;
    stateSet.setAssociation(associations);

    EXPECT_EQ(nullptr, stateSet.endpointIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointInstanceIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointAssociationDefinitionsIntf.get());
}

TEST_F(StateSetNvlinkInternalTest,
       setAssociationFiltersOutChassisAssociationAndCreatesEndpoints)
{
    auto sensor = makeStateSensor(
        1, 0x1501, PLDM_ENTITY_SYS_BUS, 1, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric1");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric1"}, false);

    auto initialAssociation = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis1/CPU_0");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_1";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    initialAssociation, *sensor);

    auto& mockState = pldm::utils::NvlinkTestDBusHandler::state();
    mockState.instanceNumber = 9;
    mockState.subtreeResponse = {
        {"/xyz/openbmc_project/inventory/system/chassis/chassis1",
         {{"xyz.openbmc_project.ObjectMapper",
           {"xyz.openbmc_project.Inventory.Item.Chassis"}}}}};

    std::vector<pldm::dbus::PathAssociation> associations{
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis1"),
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis1/CPU_0")};

    stateSet.setAssociation(associations);

    ASSERT_EQ(1u,
              stateSet.getAssociationDefinitionsIntf()->associations().size());
    EXPECT_EQ(
        "/xyz/openbmc_project/inventory/system/chassis/chassis1/CPU_0",
        std::get<2>(
            stateSet.getAssociationDefinitionsIntf()->associations().front()));
    EXPECT_NE(nullptr, stateSet.endpointIntf.get());
    EXPECT_NE(nullptr, stateSet.endpointInstanceIntf.get());
    EXPECT_NE(nullptr, stateSet.endpointAssociationDefinitionsIntf.get());
}

TEST_F(StateSetNvlinkInternalTest,
       setAssociationHandlesEmptySubtreeResponseWithoutFiltering)
{
    auto sensor = makeStateSensor(
        2, 0x1502, PLDM_ENTITY_SYS_BUS, 2, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric2");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric2"}, false);

    auto initialAssociation = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis2/CPU_0");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_2";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    initialAssociation, *sensor);

    auto& mockState = pldm::utils::NvlinkTestDBusHandler::state();
    mockState.instanceNumber = 3;

    std::vector<pldm::dbus::PathAssociation> associations{
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis2/CPU_0"),
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis2/CPU_1")};

    stateSet.setAssociation(associations);

    ASSERT_EQ(1u,
              stateSet.getAssociationDefinitionsIntf()->associations().size());
    EXPECT_EQ(
        "/xyz/openbmc_project/inventory/system/chassis/chassis2/CPU_0",
        std::get<2>(
            stateSet.getAssociationDefinitionsIntf()->associations().front()));
}

TEST_F(StateSetNvlinkInternalTest,
       setAssociationStopsAfterGetDbusPropertyFailure)
{
    auto sensor = makeStateSensor(
        3, 0x1503, PLDM_ENTITY_SYS_BUS, 3, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric3");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric3"}, false);

    auto initialAssociation = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis3/CPU_0");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_3";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    initialAssociation, *sensor);

    auto& mockState = pldm::utils::NvlinkTestDBusHandler::state();
    mockState.throwOnGetDbusProperty = true;
    mockState.subtreeResponse = {
        {"/xyz/openbmc_project/inventory/system/chassis/chassis3",
         {{"xyz.openbmc_project.ObjectMapper",
           {"xyz.openbmc_project.Inventory.Item.Chassis"}}}}};

    std::vector<pldm::dbus::PathAssociation> associations{
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis3"),
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis3/CPU_0")};

    stateSet.setAssociation(associations);

    ASSERT_EQ(1u,
              stateSet.getAssociationDefinitionsIntf()->associations().size());
    EXPECT_EQ(
        "/xyz/openbmc_project/inventory/system/chassis/chassis3/CPU_0",
        std::get<2>(
            stateSet.getAssociationDefinitionsIntf()->associations().front()));
    EXPECT_EQ(nullptr, stateSet.endpointIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointInstanceIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointAssociationDefinitionsIntf.get());
}

TEST_F(StateSetNvlinkInternalTest,
       setAssociationUsesOriginalAssociationAfterGetSubtreeFailure)
{
    auto sensor = makeStateSensor(
        4, 0x1504, PLDM_ENTITY_SYS_BUS, 4, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric4");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric4"}, false);

    auto initialAssociation = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis4/CPU_0");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_4";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    initialAssociation, *sensor);

    auto& mockState = pldm::utils::NvlinkTestDBusHandler::state();
    mockState.throwOnGetSubtree = true;
    mockState.throwOnGetDbusProperty = true;

    std::vector<pldm::dbus::PathAssociation> associations{makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis4/CPU_0")};

    stateSet.setAssociation(associations);

    ASSERT_EQ(1u,
              stateSet.getAssociationDefinitionsIntf()->associations().size());
    EXPECT_EQ(
        "/xyz/openbmc_project/inventory/system/chassis/chassis4/CPU_0",
        std::get<2>(
            stateSet.getAssociationDefinitionsIntf()->associations().front()));
    EXPECT_EQ(nullptr, stateSet.endpointIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointInstanceIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointAssociationDefinitionsIntf.get());
}

TEST_F(StateSetNvlinkInternalTest,
       setAssociationKeepsOriginalWhenOnlyFilteredPathExists)
{
    auto sensor = makeStateSensor(
        28, 0x1528, PLDM_ENTITY_SYS_BUS, 28, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric28");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric28"}, false);

    auto initialAssociation = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis28/CPU_0");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_28";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    initialAssociation, *sensor);

    auto& mockState = pldm::utils::NvlinkTestDBusHandler::state();
    mockState.instanceNumber = 28;
    mockState.subtreeResponse = {
        {"/xyz/openbmc_project/inventory/system/chassis/chassis28/CPU_0",
         {{"xyz.openbmc_project.ObjectMapper",
           {"xyz.openbmc_project.Inventory.Item.Chassis"}}}}};

    std::vector<pldm::dbus::PathAssociation> associations{initialAssociation};
    EXPECT_NO_THROW(stateSet.setAssociation(associations));

    ASSERT_EQ(1u, stateSet.getAssociations().size());
    EXPECT_EQ(initialAssociation.path,
              std::get<2>(stateSet.getAssociations().front()));
}

TEST_F(StateSetNvlinkInternalTest,
       updateShmemReadingSkipsTelemetryForDefaultInventoryAssociation)
{
    auto sensor = makeStateSensor(
        11, 0x1511, PLDM_ENTITY_SYS_BUS, 11, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric11");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric11"}, true);

    auto initialAssociation = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis11/CPU_0");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_11";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    initialAssociation, *sensor);

    Associations associations{
        {"chassis", "all_states",
         "/xyz/openbmc_project/inventory/system/chassis/chassis11/CPU_0"}};
    stateSet.setAssociations(associations);

    EXPECT_NO_THROW(stateSet.updateShmemReading("LinkStatus"));
    EXPECT_NO_THROW(stateSet.updateShmemReading("LinkState"));
}

TEST_F(StateSetNvlinkInternalTest,
       updateShmemReadingSkipsTelemetryWhenEndpointAssociationIsMissing)
{
    auto sensor = makeStateSensor(
        30, 0x1530, PLDM_ENTITY_SYS_BUS, 30, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric30");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric30"}, false);

    auto initialAssociation = makeAssociation(
        "inventory", "contains",
        "/xyz/openbmc_project/inventory/system/chassis/chassis30/CPU_0");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_30";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    initialAssociation, *sensor);

    stateSet.setAssociations(
        {{"inventory", "contains", initialAssociation.path}});

    EXPECT_NO_THROW(stateSet.updateShmemReading("LinkStatus"));
    EXPECT_NO_THROW(stateSet.updateShmemReading("LinkState"));
}

TEST_F(StateSetNvlinkInternalTest, setAssociationReusesExistingEndpointPdIs)
{
    auto sensor = makeStateSensor(
        5, 0x1505, PLDM_ENTITY_SYS_BUS, 5, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric5");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric5"}, false);

    auto initialAssociation = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis5/CPU_0");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_5";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    initialAssociation, *sensor);

    auto& mockState = pldm::utils::NvlinkTestDBusHandler::state();
    mockState.instanceNumber = 11;
    mockState.subtreeResponse = {
        {"/xyz/openbmc_project/inventory/system/chassis/chassis5",
         {{"xyz.openbmc_project.ObjectMapper",
           {"xyz.openbmc_project.Inventory.Item.Chassis"}}}}};

    std::vector<pldm::dbus::PathAssociation> associations{
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis5"),
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis5/CPU_0")};

    stateSet.setAssociation(associations);
    auto* endpointIntf = stateSet.endpointIntf.get();
    auto* endpointInstanceIntf = stateSet.endpointInstanceIntf.get();
    auto* endpointAssociationDefinitionsIntf =
        stateSet.endpointAssociationDefinitionsIntf.get();

    ASSERT_NE(nullptr, endpointIntf);
    ASSERT_NE(nullptr, endpointInstanceIntf);
    ASSERT_NE(nullptr, endpointAssociationDefinitionsIntf);

    stateSet.setAssociation(associations);

    EXPECT_EQ(endpointIntf, stateSet.endpointIntf.get());
    EXPECT_EQ(endpointInstanceIntf, stateSet.endpointInstanceIntf.get());
    EXPECT_EQ(endpointAssociationDefinitionsIntf,
              stateSet.endpointAssociationDefinitionsIntf.get());
}

TEST_F(StateSetNvlinkInternalTest,
       setAssociationRecreatesOnlyMissingEndpointPdIs)
{
    auto sensor = makeStateSensor(
        6, 0x1506, PLDM_ENTITY_SYS_BUS, 6, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric6");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric6"}, false);

    auto initialAssociation = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis6/CPU_0");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_6";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    initialAssociation, *sensor);

    auto& mockState = pldm::utils::NvlinkTestDBusHandler::state();
    mockState.instanceNumber = 12;
    mockState.subtreeResponse = {
        {"/xyz/openbmc_project/inventory/system/chassis/chassis6",
         {{"xyz.openbmc_project.ObjectMapper",
           {"xyz.openbmc_project.Inventory.Item.Chassis"}}}}};

    std::vector<pldm::dbus::PathAssociation> associations{
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis6"),
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis6/CPU_0")};

    stateSet.setAssociation(associations);

    auto* initialEndpointIntf = stateSet.endpointIntf.get();
    auto* initialEndpointInstanceIntf = stateSet.endpointInstanceIntf.get();
    auto* initialEndpointAssociationDefinitionsIntf =
        stateSet.endpointAssociationDefinitionsIntf.get();

    ASSERT_NE(nullptr, initialEndpointIntf);
    ASSERT_NE(nullptr, initialEndpointInstanceIntf);
    ASSERT_NE(nullptr, initialEndpointAssociationDefinitionsIntf);

    stateSet.endpointAssociationDefinitionsIntf.reset();
    stateSet.setAssociation(associations);
    EXPECT_EQ(initialEndpointIntf, stateSet.endpointIntf.get());
    EXPECT_EQ(initialEndpointInstanceIntf, stateSet.endpointInstanceIntf.get());
    EXPECT_NE(nullptr, stateSet.endpointAssociationDefinitionsIntf.get());

    auto* recreatedAssociationDefinitionsIntf =
        stateSet.endpointAssociationDefinitionsIntf.get();
    stateSet.endpointInstanceIntf.reset();
    stateSet.setAssociation(associations);
    EXPECT_EQ(initialEndpointIntf, stateSet.endpointIntf.get());
    EXPECT_NE(nullptr, stateSet.endpointInstanceIntf.get());
    EXPECT_EQ(recreatedAssociationDefinitionsIntf,
              stateSet.endpointAssociationDefinitionsIntf.get());

    auto* recreatedEndpointInstanceIntf = stateSet.endpointInstanceIntf.get();
    stateSet.endpointIntf.reset();
    stateSet.setAssociation(associations);
    EXPECT_NE(nullptr, stateSet.endpointIntf.get());
    EXPECT_EQ(recreatedEndpointInstanceIntf,
              stateSet.endpointInstanceIntf.get());
    EXPECT_EQ(recreatedAssociationDefinitionsIntf,
              stateSet.endpointAssociationDefinitionsIntf.get());
}

TEST_F(StateSetNvlinkInternalTest,
       setAssociationSwallowsEndpointCreationFailure)
{
    auto sensor = makeStateSensor(
        7, 0x1507, PLDM_ENTITY_SYS_BUS, 7, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric7");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric7"}, false);

    auto initialAssociation = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis7/CPU_0");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_7";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    initialAssociation, *sensor);

    auto& mockState = pldm::utils::NvlinkTestDBusHandler::state();
    mockState.instanceNumber = 13;
    mockState.subtreeResponse = {
        {"/xyz/openbmc_project/inventory/system/chassis/chassis7",
         {{"xyz.openbmc_project.ObjectMapper",
           {"xyz.openbmc_project.Inventory.Item.Chassis"}}}}};

    const auto invalidEndpointAssociation = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis7/CPU 0");
    std::vector<pldm::dbus::PathAssociation> associations{
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis7"),
        invalidEndpointAssociation};

    EXPECT_NO_THROW(stateSet.setAssociation(associations));

    ASSERT_EQ(1u,
              stateSet.getAssociationDefinitionsIntf()->associations().size());
    EXPECT_EQ(
        invalidEndpointAssociation.path,
        std::get<2>(
            stateSet.getAssociationDefinitionsIntf()->associations().front()));
    EXPECT_EQ(nullptr, stateSet.endpointIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointInstanceIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointAssociationDefinitionsIntf.get());
}

TEST_F(StateSetNvlinkInternalTest,
       setAssociationSwallowsEndpointInstanceCreationFailure)
{
    auto sensor = makeStateSensor(
        14, 0x1514, PLDM_ENTITY_SYS_BUS, 14, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric14");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric14"}, false);

    auto initialAssociation = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis14/CPU_0");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_14";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    initialAssociation, *sensor);

    auto& mockState = pldm::utils::NvlinkTestDBusHandler::state();
    mockState.instanceNumber = 18;
    mockState.subtreeResponse = {
        {"/xyz/openbmc_project/inventory/system/chassis/chassis14",
         {{"xyz.openbmc_project.ObjectMapper",
           {"xyz.openbmc_project.Inventory.Item.Chassis"}}}}};

    std::vector<pldm::dbus::PathAssociation> associations{
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis14"),
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis14/CPU_0")};

    const auto endpointPath =
        stateSet.fabricsObjectPath + stateSet.c2clinkFabricPrefix +
        std::to_string(mockState.instanceNumber) + "/Endpoints/CPU_0";
    oem_nvidia::InstanceIntf conflictingInstance(
        pldm::utils::NvlinkTestDBusHandler::getBus(), endpointPath.c_str());
    while (pldm::utils::NvlinkTestDBusHandler::getBus().process_discard() > 0)
    {}

    EXPECT_NO_THROW(stateSet.setAssociation(associations));

    ASSERT_EQ(1u, stateSet.getAssociations().size());
    EXPECT_NE(nullptr, stateSet.endpointIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointInstanceIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointAssociationDefinitionsIntf.get());
}

TEST_F(StateSetNvlinkInternalTest,
       setAssociationSwallowsEndpointAssociationCreationFailure)
{
    auto sensor = makeStateSensor(
        15, 0x1515, PLDM_ENTITY_SYS_BUS, 15, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric15");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric15"}, false);

    auto initialAssociation = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis15/CPU_0");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_15";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    initialAssociation, *sensor);

    auto& mockState = pldm::utils::NvlinkTestDBusHandler::state();
    mockState.instanceNumber = 19;
    mockState.subtreeResponse = {
        {"/xyz/openbmc_project/inventory/system/chassis/chassis15",
         {{"xyz.openbmc_project.ObjectMapper",
           {"xyz.openbmc_project.Inventory.Item.Chassis"}}}}};

    std::vector<pldm::dbus::PathAssociation> associations{
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis15"),
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis15/CPU_0")};

    const auto endpointPath =
        stateSet.fabricsObjectPath + stateSet.c2clinkFabricPrefix +
        std::to_string(mockState.instanceNumber) + "/Endpoints/CPU_0";
    AssociationDefinitionsInft conflictingAssociations(
        pldm::utils::NvlinkTestDBusHandler::getBus(), endpointPath.c_str());
    conflictingAssociations.associations(
        {{"entity_link", "",
          "/xyz/openbmc_project/inventory/system/chassis/chassis15/CPU_0"}});
    while (pldm::utils::NvlinkTestDBusHandler::getBus().process_discard() > 0)
    {}

    EXPECT_NO_THROW(stateSet.setAssociation(associations));

    ASSERT_EQ(1u, stateSet.getAssociations().size());
    EXPECT_NE(nullptr, stateSet.endpointIntf.get());
    EXPECT_NE(nullptr, stateSet.endpointInstanceIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointAssociationDefinitionsIntf.get());
}

TEST_F(StateSetNvlinkInternalTest,
       setAssociationSwallowsEndpointPortCreationFailure)
{
    auto sensor = makeStateSensor(
        19, 0x1519, PLDM_ENTITY_SYS_BUS, 19, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric19");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric19"}, false);

    auto initialAssociation = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis19/CPU_0");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_19";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    initialAssociation, *sensor);

    auto& mockState = pldm::utils::NvlinkTestDBusHandler::state();
    mockState.instanceNumber = 22;
    mockState.subtreeResponse = {
        {"/xyz/openbmc_project/inventory/system/chassis/chassis19",
         {{"xyz.openbmc_project.ObjectMapper",
           {"xyz.openbmc_project.Inventory.Item.Chassis"}}}}};

    std::vector<pldm::dbus::PathAssociation> associations{
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis19"),
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis19/CPU_0")};

    const auto endpointPath =
        stateSet.fabricsObjectPath + stateSet.c2clinkFabricPrefix +
        std::to_string(mockState.instanceNumber) + "/Endpoints/CPU_0";
    oem_nvidia::EndpointIntf conflictingEndpoint(
        pldm::utils::NvlinkTestDBusHandler::getBus(), endpointPath.c_str());
    while (pldm::utils::NvlinkTestDBusHandler::getBus().process_discard() > 0)
    {}

    EXPECT_NO_THROW(stateSet.setAssociation(associations));

    ASSERT_EQ(1u, stateSet.getAssociations().size());
    EXPECT_EQ(nullptr, stateSet.endpointIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointInstanceIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointAssociationDefinitionsIntf.get());
}

TEST_F(StateSetNvlinkInternalTest,
       getEventDataHandlesMissingLinkDownEventIdWithSensorInfo)
{
    auto sensor = makeStateSensor(
        10, 0x1510, PLDM_ENTITY_SYS_BUS, 10, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric10");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_10";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis10/CPU_0");
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *sensor);

    stateSet.setValue(PLDM_STATE_SET_NVLINK_INACTIVE);

    pldm::utils::SensorEventInfo sensorEventInfo;
    sensorEventInfo.impactedComponent = "NvlinkEndpoint10";
    auto [message, arg, level, eventId,
          impacted] = stateSet.getEventData(&sensorEventInfo);

    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedOK", message);
    EXPECT_EQ("LinkDown", arg);
    EXPECT_EQ(Level::Informational, level);
    EXPECT_TRUE(eventId.empty());
    EXPECT_EQ("NvlinkEndpoint10", impacted);
}

TEST_F(StateSetNvlinkInternalTest,
       getEventDataCoversLinkUpErrorAndUnknownStates)
{
    auto sensor = makeStateSensor(
        14, 0x1514, PLDM_ENTITY_SYS_BUS, 14, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric14");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric14"}, false);

    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_14";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis14/CPU_0");
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *sensor);

    EXPECT_NO_THROW(stateSet.setValue(PLDM_STATE_SET_NVLINK_ACTIVE));
    auto [upMessage, upArg, upLevel, upEventId,
          upImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedOK", upMessage);
    EXPECT_EQ("LinkUp", upArg);
    EXPECT_EQ(Level::Informational, upLevel);
    EXPECT_TRUE(upEventId.empty());
    EXPECT_TRUE(upImpacted.empty());

    EXPECT_NO_THROW(stateSet.setValue(PLDM_STATE_SET_NVLINK_INVALID_FREQ));
    auto [errorMessage, errorArg, errorLevel, errorEventId,
          errorImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedOK", errorMessage);
    EXPECT_EQ("LinkDown", errorArg);
    EXPECT_EQ(Level::Informational, errorLevel);
    EXPECT_TRUE(errorEventId.empty());
    EXPECT_TRUE(errorImpacted.empty());

    EXPECT_NO_THROW(stateSet.setValue(0xFF));
    auto [unknownMessage, unknownArg, unknownLevel, unknownEventId,
          unknownImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedWarning", unknownMessage);
    EXPECT_EQ("Unknown", unknownArg);
    EXPECT_EQ(Level::Warning, unknownLevel);
    EXPECT_TRUE(unknownEventId.empty());
    EXPECT_TRUE(unknownImpacted.empty());
}

TEST_F(StateSetNvlinkInternalTest,
       getEventDataUsesConfiguredLinkDownEventIdAndUpdatesShmemCoverage)
{
    auto sensor = makeStateSensor(
        15, 0x1515, PLDM_ENTITY_SYS_BUS, 15, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric15");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric15"}, false);

    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_15";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis15/CPU_0");
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *sensor);

    pldm::utils::SensorEventInfo sensorEventInfo;
    sensorEventInfo.impactedComponent = "NvlinkEndpoint15";
    sensorEventInfo.eventIdsMap.emplace("LinkDown",
                                        "ResourceEvent.1.0.LinkDown");

    EXPECT_NO_THROW(stateSet.setValue(PLDM_STATE_SET_NVLINK_ACTIVE));
    EXPECT_NO_THROW(stateSet.setValue(PLDM_STATE_SET_NVLINK_INACTIVE));
    auto [message, arg, level, eventId,
          impacted] = stateSet.getEventData(&sensorEventInfo);

    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedOK", message);
    EXPECT_EQ("LinkDown", arg);
    EXPECT_EQ(Level::Informational, level);
    EXPECT_EQ("ResourceEvent.1.0.LinkDown", eventId);
    EXPECT_EQ("NvlinkEndpoint15", impacted);
}

TEST_F(StateSetNvlinkInternalTest,
       setValueSkipsTelemetryWhenUsingDefaultInventoryAssociationCoverage)
{
    auto sensor = makeStateSensor(
        16, 0x1516, PLDM_ENTITY_SYS_BUS, 16, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric16");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric16"}, true);

    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_16";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis16/CPU_0");
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *sensor);

    EXPECT_NO_THROW(stateSet.setValue(PLDM_STATE_SET_NVLINK_ACTIVE));
    auto [message, arg, level, eventId,
          impacted] = stateSet.getEventData(nullptr);

    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedOK", message);
    EXPECT_EQ("LinkUp", arg);
    EXPECT_EQ(Level::Informational, level);
    EXPECT_TRUE(eventId.empty());
    EXPECT_TRUE(impacted.empty());
}

TEST_F(StateSetNvlinkInternalTest,
       updateShmemReadingIgnoresUnknownPropertyAndNonMatchingAssociations)
{
    auto sensor = makeStateSensor(
        11, 0x1511, PLDM_ENTITY_SYS_BUS, 11, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric11");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric11"}, false);

    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_11";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis11/CPU_0");
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *sensor);

    stateSet.setAssociations(
        {{"chassis", "reverse_only",
          "/xyz/openbmc_project/inventory/system/chassis/chassis11/CPU_0"},
         {"processor", "all_states",
          "/xyz/openbmc_project/inventory/system/chassis/chassis11/CPU_1"}});

    EXPECT_NO_THROW(stateSet.updateShmemReading("UnsupportedProperty"));
}

TEST_F(StateSetNvlinkInternalTest,
       setAssociationReturnsWhenSelectedAssociationPathIsEmpty)
{
    auto sensor = makeStateSensor(
        12, 0x1512, PLDM_ENTITY_SYS_BUS, 12, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric12");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_12";
    auto association = makeAssociation("chassis", "all_states", "");
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *sensor);

    std::vector<pldm::dbus::PathAssociation> associations{association};
    stateSet.setAssociation(associations);

    ASSERT_EQ(1u, stateSet.getAssociations().size());
    EXPECT_TRUE(std::get<2>(stateSet.getAssociations().front()).empty());
    EXPECT_EQ(nullptr, stateSet.endpointIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointInstanceIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointAssociationDefinitionsIntf.get());
}

TEST_F(StateSetNvlinkInternalTest,
       setAssociationKeepsOnlyChassisPathWhenNoProcessorAssociationExists)
{
    auto sensor = makeStateSensor(
        13, 0x1513, PLDM_ENTITY_SYS_BUS, 13, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric13");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric13"}, false);

    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_13";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis13");
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *sensor);

    auto& mockState = pldm::utils::NvlinkTestDBusHandler::state();
    mockState.instanceNumber = 17;
    mockState.subtreeResponse = {
        {"/xyz/openbmc_project/inventory/system/chassis/chassis13",
         {{"xyz.openbmc_project.ObjectMapper",
           {"xyz.openbmc_project.Inventory.Item.Chassis"}}}}};

    std::vector<pldm::dbus::PathAssociation> associations{makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis13")};

    stateSet.setAssociation(associations);

    ASSERT_EQ(1u, stateSet.getAssociations().size());
    EXPECT_EQ("/xyz/openbmc_project/inventory/system/chassis/chassis13",
              std::get<2>(stateSet.getAssociations().front()));
}

TEST_F(StateSetNvlinkInternalTest,
       setAssociationKeepsInitialAssociationWhenSubtreeIsEmpty)
{
    auto sensor = makeStateSensor(
        20, 0x1520, PLDM_ENTITY_SYS_BUS, 20, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric20");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric20"}, false);

    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis20");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_20";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *sensor);

    pldm::utils::NvlinkTestDBusHandler::state().subtreeResponse = {};
    std::vector<pldm::dbus::PathAssociation> associations{
        association,
        makeAssociation(
            "processor", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis20/CPU_0")};

    stateSet.setAssociation(associations);

    ASSERT_EQ(1u, stateSet.getAssociations().size());
    EXPECT_EQ("/xyz/openbmc_project/inventory/system/chassis/chassis20",
              std::get<2>(stateSet.getAssociations().front()));
}

TEST_F(StateSetNvlinkInternalTest,
       getEventDataReturnsOkWhenLinkDownHasNoSensorInfo)
{
    auto sensor = makeStateSensor(
        17, 0x1517, PLDM_ENTITY_SYS_BUS, 17, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric17");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_17";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis17/CPU_0");
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *sensor);

    stateSet.setValue(PLDM_STATE_SET_NVLINK_INACTIVE);
    auto [message, arg, level, eventId,
          impacted] = stateSet.getEventData(nullptr);

    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedOK", message);
    EXPECT_EQ("LinkDown", arg);
    EXPECT_EQ(Level::Informational, level);
    EXPECT_TRUE(eventId.empty());
    EXPECT_TRUE(impacted.empty());
}

TEST_F(StateSetNvlinkInternalTest,
       setAssociationReusesExistingEndpointInterfaces)
{
    auto sensor = makeStateSensor(
        18, 0x1518, PLDM_ENTITY_SYS_BUS, 18, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric18");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric18"}, false);

    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis18/CPU_0");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_18";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *sensor);

    auto& mockState = pldm::utils::NvlinkTestDBusHandler::state();
    mockState.instanceNumber = 21;
    mockState.subtreeResponse = {
        {"/xyz/openbmc_project/inventory/system/chassis/chassis18",
         {{"xyz.openbmc_project.ObjectMapper",
           {"xyz.openbmc_project.Inventory.Item.Chassis"}}}}};

    std::vector<pldm::dbus::PathAssociation> firstAssociations{
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis18"),
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis18/CPU_0")};

    stateSet.setAssociation(firstAssociations);
    auto* endpointIntf = stateSet.endpointIntf.get();
    auto* endpointInstanceIntf = stateSet.endpointInstanceIntf.get();
    auto* endpointAssociationIntf =
        stateSet.endpointAssociationDefinitionsIntf.get();
    ASSERT_NE(nullptr, endpointIntf);
    ASSERT_NE(nullptr, endpointInstanceIntf);
    ASSERT_NE(nullptr, endpointAssociationIntf);

    std::vector<pldm::dbus::PathAssociation> secondAssociations{
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis18"),
        makeAssociation(
            "processor", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis18/CPU_1")};

    stateSet.setAssociation(secondAssociations);

    EXPECT_EQ(endpointIntf, stateSet.endpointIntf.get());
    EXPECT_EQ(endpointInstanceIntf, stateSet.endpointInstanceIntf.get());
    EXPECT_EQ(endpointAssociationIntf,
              stateSet.endpointAssociationDefinitionsIntf.get());
    ASSERT_EQ(1u, stateSet.getAssociations().size());
    EXPECT_EQ("/xyz/openbmc_project/inventory/system/chassis/chassis18/CPU_1",
              std::get<2>(stateSet.getAssociations().front()));
}

TEST_F(StateSetNvlinkInternalTest,
       memoryPerformanceCoversDefaultNormalAndThrottledValues)
{
    [[maybe_unused]] auto sensor = makeStateSensor(
        21, 0x1521, PLDM_ENTITY_MEMORY_CONTROLLER, 21,
        PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/chassis/chassis21");
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_performance_21";
    auto association = makeAssociation("memory", "all_states", "");

    StateSetMemoryPerformance stateSet(PLDM_STATESET_ID_PERFORMANCE, 0, path,
                                       association);

    EXPECT_EQ("Performance", stateSet.getStringStateType());

    auto [defaultMessage, defaultArg, defaultLevel, defaultEventId,
          defaultImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedWarning", defaultMessage);
    EXPECT_EQ("Throttled (performance degraded)", defaultArg);
    EXPECT_EQ(Level::Warning, defaultLevel);
    EXPECT_TRUE(defaultEventId.empty());
    EXPECT_TRUE(defaultImpacted.empty());

    EXPECT_NO_THROW(stateSet.setValue(PLDM_STATESET_PERFORMANCE_NORMAL));
    auto [normalMessage, normalArg, normalLevel, normalEventId,
          normalImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedOK", normalMessage);
    EXPECT_EQ("Normal", normalArg);
    EXPECT_EQ(Level::Informational, normalLevel);
    EXPECT_TRUE(normalEventId.empty());
    EXPECT_TRUE(normalImpacted.empty());

    EXPECT_NO_THROW(stateSet.setValue(PLDM_STATESET_PERFORMANCE_THROTTLED));
    auto [throttledMessage, throttledArg, throttledLevel, throttledEventId,
          throttledImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedWarning",
              throttledMessage);
    EXPECT_EQ("Throttled (performance degraded)", throttledArg);
    EXPECT_EQ(Level::Warning, throttledLevel);
    EXPECT_TRUE(throttledEventId.empty());
    EXPECT_TRUE(throttledImpacted.empty());

    EXPECT_NO_THROW(stateSet.setValue(0xFF));
    auto [unknownMessage, unknownArg, unknownLevel, unknownEventId,
          unknownImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedWarning", unknownMessage);
    EXPECT_EQ("Throttled (performance degraded)", unknownArg);
    EXPECT_EQ(Level::Warning, unknownLevel);
    EXPECT_TRUE(unknownEventId.empty());
    EXPECT_TRUE(unknownImpacted.empty());
}

TEST_F(StateSetNvlinkInternalTest,
       memorySpareChannelCoversAllPresenceValuesAndDefaultAssociation)
{
    auto sensor = makeStateSensor(
        22, 0x1522, PLDM_ENTITY_MEMORY_CONTROLLER, 22,
        PLDM_STATESET_ID_PRESENCE,
        "/xyz/openbmc_project/inventory/system/chassis/chassis22");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis22"}, true);

    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_spare_channel_22";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis22");
    StateSetMemorySpareChannel stateSet(PLDM_STATESET_ID_PRESENCE, 0, path,
                                        association, *sensor);

    EXPECT_EQ("MemorySpareChannelPresence", stateSet.getStringStateType());

    auto [defaultMessage, defaultArg, defaultLevel, defaultEventId,
          defaultImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedOK", defaultMessage);
    EXPECT_EQ("Unknown", defaultArg);
    EXPECT_EQ(Level::Informational, defaultLevel);
    EXPECT_TRUE(defaultEventId.empty());
    EXPECT_TRUE(defaultImpacted.empty());

    EXPECT_NO_THROW(stateSet.setValue(PLDM_STATESET_PRESENCE_PRESENT));
    auto [presentMessage, presentArg, presentLevel, presentEventId,
          presentImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedOK", presentMessage);
    EXPECT_EQ("True", presentArg);
    EXPECT_EQ(Level::Informational, presentLevel);
    EXPECT_TRUE(presentEventId.empty());
    EXPECT_TRUE(presentImpacted.empty());

    EXPECT_NO_THROW(stateSet.setValue(PLDM_STATESET_PRESENCE_NOT_PRESENT));
    auto [missingMessage, missingArg, missingLevel, missingEventId,
          missingImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedOK", missingMessage);
    EXPECT_EQ("False", missingArg);
    EXPECT_EQ(Level::Informational, missingLevel);
    EXPECT_TRUE(missingEventId.empty());
    EXPECT_TRUE(missingImpacted.empty());

    EXPECT_NO_THROW(stateSet.setValue(0xFF));
    auto [unknownMessage, unknownArg, unknownLevel, unknownEventId,
          unknownImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedOK", unknownMessage);
    EXPECT_EQ("Unknown", unknownArg);
    EXPECT_EQ(Level::Informational, unknownLevel);
    EXPECT_TRUE(unknownEventId.empty());
    EXPECT_TRUE(unknownImpacted.empty());
}

TEST_F(StateSetNvlinkInternalTest,
       processorPowerBreakCoversAllValuesAndDefaultAssociation)
{
    auto sensor = makeStateSensor(
        23, STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC, 23,
        PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/chassis/chassis23");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis23"}, true);

    std::string path =
        "/xyz/openbmc_project/state/coverage/processor_power_break_23";
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis23");
    StateSetProcessorPowerBreak stateSet(PLDM_STATESET_ID_PERFORMANCE, 0, path,
                                         association, *sensor);

    EXPECT_EQ("PowerBreak", stateSet.getStringStateType());

    auto [defaultMessage, defaultArg, defaultLevel, defaultEventId,
          defaultImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedWarning", defaultMessage);
    EXPECT_EQ("Throttled", defaultArg);
    EXPECT_EQ(Level::Warning, defaultLevel);
    EXPECT_TRUE(defaultEventId.empty());
    EXPECT_TRUE(defaultImpacted.empty());

    EXPECT_NO_THROW(stateSet.setValue(PLDM_STATESET_PERFORMANCE_NORMAL));
    auto [normalMessage, normalArg, normalLevel, normalEventId,
          normalImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedOK", normalMessage);
    EXPECT_EQ("Normal", normalArg);
    EXPECT_EQ(Level::Informational, normalLevel);
    EXPECT_TRUE(normalEventId.empty());
    EXPECT_TRUE(normalImpacted.empty());

    EXPECT_NO_THROW(stateSet.setValue(PLDM_STATESET_PERFORMANCE_THROTTLED));
    auto [throttledMessage, throttledArg, throttledLevel, throttledEventId,
          throttledImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedWarning",
              throttledMessage);
    EXPECT_EQ("Throttled", throttledArg);
    EXPECT_EQ(Level::Warning, throttledLevel);
    EXPECT_TRUE(throttledEventId.empty());
    EXPECT_TRUE(throttledImpacted.empty());

    EXPECT_NO_THROW(stateSet.setValue(0xFF));
    auto [unknownMessage, unknownArg, unknownLevel, unknownEventId,
          unknownImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedWarning", unknownMessage);
    EXPECT_EQ("Throttled", unknownArg);
    EXPECT_EQ(Level::Warning, unknownLevel);
    EXPECT_TRUE(unknownEventId.empty());
    EXPECT_TRUE(unknownImpacted.empty());
}

TEST_F(StateSetNvlinkInternalTest, nvlinkDefaultValueAndStateTypeCoverage)
{
    auto sensor = makeStateSensor(
        24, 0x1524, PLDM_ENTITY_SYS_BUS, 24, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric24");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_24";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis24/CPU_0");
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *sensor);

    EXPECT_EQ("NVLink", stateSet.getStringStateType());

    auto [defaultMessage, defaultArg, defaultLevel, defaultEventId,
          defaultImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedWarning", defaultMessage);
    EXPECT_EQ("Unknown", defaultArg);
    EXPECT_EQ(Level::Warning, defaultLevel);
    EXPECT_TRUE(defaultEventId.empty());
    EXPECT_TRUE(defaultImpacted.empty());

    EXPECT_NO_THROW(stateSet.setValue(0xFE));
    auto [unknownMessage, unknownArg, unknownLevel, unknownEventId,
          unknownImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedWarning", unknownMessage);
    EXPECT_EQ("Unknown", unknownArg);
    EXPECT_EQ(Level::Warning, unknownLevel);
    EXPECT_TRUE(unknownEventId.empty());
    EXPECT_TRUE(unknownImpacted.empty());
}

TEST_F(StateSetNvlinkInternalTest,
       memoryPerformanceUpdateShmemReadingSkipCoverage)
{
    [[maybe_unused]] auto sensor = makeStateSensor(
        25, 0x1525, PLDM_ENTITY_MEMORY_CONTROLLER, 25,
        PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/chassis/chassis25");
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_performance_25";
    auto association = makeAssociation("memory", "all_states", "");
    StateSetMemoryPerformance stateSet(PLDM_STATESET_ID_PERFORMANCE, 0, path,
                                       association);

    std::vector<pldm::dbus::PathAssociation> associations{
        makeAssociation("memory", "all_states", ""),
        makeAssociation(
            "processor", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis25/CPU_0")};
    stateSet.setAssociation(associations);
    EXPECT_NO_THROW(stateSet.updateShmemReading("Value"));
}

TEST_F(StateSetNvlinkInternalTest,
       memorySpareChannelUpdateShmemReadingSkipCoverage)
{
    auto sensor = makeStateSensor(
        26, 0x1526, PLDM_ENTITY_MEMORY_CONTROLLER, 26,
        PLDM_STATESET_ID_PRESENCE,
        "/xyz/openbmc_project/inventory/system/chassis/chassis26");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis26"}, true);

    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_spare_channel_26";
    auto association = makeAssociation("chassis", "all_states", "");
    StateSetMemorySpareChannel stateSet(PLDM_STATESET_ID_PRESENCE, 0, path,
                                        association, *sensor);

    std::vector<pldm::dbus::PathAssociation> associations{
        makeAssociation("chassis", "all_states", ""),
        makeAssociation(
            "memory", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis26")};
    stateSet.setAssociation(associations);
    EXPECT_NO_THROW(stateSet.updateShmemReading("MemorySpareChannelPresence"));
}

TEST_F(StateSetNvlinkInternalTest,
       processorPowerBreakUpdateShmemReadingSkipCoverage)
{
    auto sensor = makeStateSensor(
        27, STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC, 27,
        PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/chassis/chassis27");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis27"}, true);

    std::string path =
        "/xyz/openbmc_project/state/coverage/processor_power_break_27";
    auto association = makeAssociation("memory", "all_states", "");
    StateSetProcessorPowerBreak stateSet(PLDM_STATESET_ID_PERFORMANCE, 0, path,
                                         association, *sensor);

    std::vector<pldm::dbus::PathAssociation> associations{
        makeAssociation("memory", "all_states", ""),
        makeAssociation(
            "processor", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis27/CPU_0")};
    stateSet.setAssociation(associations);
    EXPECT_NO_THROW(stateSet.updateShmemReading("Value"));
}

TEST_F(StateSetNvlinkInternalTest, nvlinkCtorBadAllocCoverageInternal)
{
    auto sensor = makeStateSensor(
        31, 0x1531, PLDM_ENTITY_SYS_BUS, 31, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric31/" +
            std::string(96, 'f'));
    std::string path =
        "/xyz/openbmc_project/state/coverage/nvlink_ctor_alloc_internal_" +
        std::string(96, 'n');
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis31/" +
            std::string(96, 'c'));

    EXPECT_TRUE(exerciseBadAlloc([&] {
        StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                        association, *sensor);
    }));
}

TEST_F(StateSetNvlinkInternalTest,
       nvlinkSetAssociationSwallowsBadAllocCoverageInternal)
{
    auto sensor = makeStateSensor(
        32, 0x1532, PLDM_ENTITY_SYS_BUS, 32, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric32");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric32"}, false);

    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis32/CPU_0");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_internal_32";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *sensor);

    auto& mockState = pldm::utils::NvlinkTestDBusHandler::state();
    mockState.instanceNumber = 32;
    mockState.subtreeResponse = {
        {"/xyz/openbmc_project/inventory/system/chassis/chassis32",
         {{"xyz.openbmc_project.ObjectMapper",
           {"xyz.openbmc_project.Inventory.Item.Chassis"}}}}};

    std::vector<pldm::dbus::PathAssociation> associations{
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis32"),
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis32/" +
                std::string(96, 'e'))};

    EXPECT_TRUE(exerciseSwallowedBadAlloc([&] {
        stateSet.setAssociation(associations);
    }));
}

TEST_F(StateSetNvlinkInternalTest, nvlinkCtorDeepBadAllocCoverageInternal)
{
    auto sensor = makeStateSensor(
        48, 0x1548, PLDM_ENTITY_SYS_BUS, 48, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric48/" +
            std::string(144, 'f'));
    std::string path =
        "/xyz/openbmc_project/state/coverage/nvlink_ctor_alloc_deep_" +
        std::string(144, 'n');
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis48/" +
            std::string(144, 'c'));

    EXPECT_TRUE(exerciseBadAlloc(
        [&] {
            StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
                                            path, association, *sensor);
        },
        512));
}

TEST_F(StateSetNvlinkInternalTest, nvlinkCtorExhaustiveBadAllocCoverageInternal)
{
    auto sensor = makeStateSensor(
        58, 0x1558, PLDM_ENTITY_SYS_BUS, 58, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric58/" +
            std::string(160, 'f'));
    std::string path =
        "/xyz/openbmc_project/state/coverage/nvlink_ctor_alloc_sweep_" +
        std::string(160, 'n');
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis58/" +
            std::string(160, 'c'));

    EXPECT_TRUE(exerciseAllBadAlloc(
        [&] {
            StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
                                            path, association, *sensor);
        },
        512));
}

TEST_F(StateSetNvlinkInternalTest,
       nvlinkSetAssociationSwallowsDeepBadAllocCoverageInternal)
{
    auto sensor = makeStateSensor(
        49, 0x1549, PLDM_ENTITY_SYS_BUS, 49, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric49");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric49"}, false);

    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis49/CPU_0");
    std::string path =
        "/xyz/openbmc_project/state/coverage/nvlink_internal_deep_49";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *sensor);

    auto& mockState = pldm::utils::NvlinkTestDBusHandler::state();
    mockState.instanceNumber = 49;
    mockState.subtreeResponse = {
        {"/xyz/openbmc_project/inventory/system/chassis/chassis49",
         {{"xyz.openbmc_project.ObjectMapper",
           {"xyz.openbmc_project.Inventory.Item.Chassis"}}}}};

    std::vector<pldm::dbus::PathAssociation> associations{
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis49"),
        makeAssociation(
            "chassis", "all_states",
            "/xyz/openbmc_project/inventory/system/chassis/chassis49/" +
                std::string(144, 'e'))};

    EXPECT_TRUE(exerciseSwallowedBadAlloc(
        [&] { stateSet.setAssociation(associations); }, 512));
}

TEST_F(StateSetNvlinkInternalTest,
       memoryPerformanceCtorBadAllocCoverageInternal)
{
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_performance_alloc_" +
        std::string(96, 'm');
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory31/" +
            std::string(96, 'a'));

    EXPECT_TRUE(exerciseBadAlloc([&] {
        StateSetMemoryPerformanceCoverage stateSet(PLDM_STATESET_ID_PERFORMANCE,
                                                   0, path, association);
    }));
}

TEST_F(StateSetNvlinkInternalTest,
       memoryPerformanceCtorDeepBadAllocCoverageInternal)
{
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_performance_alloc_deep_" +
        std::string(144, 'm');
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory48/" +
            std::string(144, 'a'));

    EXPECT_TRUE(exerciseBadAlloc(
        [&] {
            StateSetMemoryPerformanceCoverage stateSet(
                PLDM_STATESET_ID_PERFORMANCE, 0, path, association);
        },
        512));
}

TEST_F(StateSetNvlinkInternalTest,
       memoryPerformanceCtorExhaustiveBadAllocCoverageInternal)
{
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_performance_alloc_sweep_" +
        std::string(160, 'm');
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory58/" +
            std::string(160, 'a'));

    EXPECT_TRUE(exerciseAllBadAlloc(
        [&] {
            StateSetMemoryPerformanceCoverage stateSet(
                PLDM_STATESET_ID_PERFORMANCE, 0, path, association);
        },
        512));
}

TEST_F(StateSetNvlinkInternalTest,
       memoryPerformanceUpdateShmemReadingBadAllocCoverageInternal)
{
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_performance_update_" +
        std::string(96, 'u');
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory32");
    StateSetMemoryPerformanceCoverage stateSet(PLDM_STATESET_ID_PERFORMANCE, 0,
                                               path, association);

    Associations associations{
        {"memory", "all_states",
         "/xyz/openbmc_project/inventory/system/memory/memory32/" +
             std::string(96, 'e')}};
    stateSet.setAssociations(associations);

    EXPECT_TRUE(
        exerciseBadAlloc([&] { stateSet.updateShmemReading("Value"); }));
}

TEST_F(StateSetNvlinkInternalTest,
       memoryPerformanceUpdateShmemReadingDeepBadAllocCoverageInternal)
{
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_performance_update_deep_" +
        std::string(144, 'u');
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory52");
    StateSetMemoryPerformanceCoverage stateSet(PLDM_STATESET_ID_PERFORMANCE, 0,
                                               path, association);

    Associations associations{
        {"memory", "all_states",
         "/xyz/openbmc_project/inventory/system/memory/memory52/" +
             std::string(144, 'e')}};
    stateSet.setAssociations(associations);

    EXPECT_TRUE(exerciseBadAlloc(
        [&] { stateSet.updateShmemReading("Value" + std::string(96, 'p')); },
        512));
}

TEST_F(StateSetNvlinkInternalTest,
       memoryPerformanceUpdateShmemReadingExhaustiveBadAllocCoverageInternal)
{
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_performance_update_sweep_" +
        std::string(160, 'u');
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory53");
    StateSetMemoryPerformanceCoverage stateSet(PLDM_STATESET_ID_PERFORMANCE, 0,
                                               path, association);

    Associations associations{
        {"memory", "all_states",
         "/xyz/openbmc_project/inventory/system/memory/memory53/" +
             std::string(160, 'e')}};
    stateSet.setAssociations(associations);

    EXPECT_TRUE(exerciseAllBadAlloc(
        [&] { stateSet.updateShmemReading("Value" + std::string(112, 'p')); },
        512));
}

TEST_F(StateSetNvlinkInternalTest,
       memoryPerformanceUpdateShmemReadingLongAssociationCoverageInternal)
{
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_performance_long_assoc_" +
        std::string(64, 'm');
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory_long_assoc");
    StateSetMemoryPerformanceCoverage stateSet(PLDM_STATESET_ID_PERFORMANCE, 0,
                                               path, association);

    Associations associations{
        {"memory_" + std::string(64, 'x'), "all_states",
         "/xyz/openbmc_project/inventory/system/ignored/" +
             std::string(64, 'a')},
        {"memory", "all_states",
         "/xyz/openbmc_project/inventory/system/memory/memory_long_assoc/" +
             std::string(96, 'b')}};
    stateSet.setAssociations(associations);

    EXPECT_NO_THROW(
        stateSet.updateShmemReading("Value" + std::string(96, 'p')));
}

TEST_F(StateSetNvlinkInternalTest,
       memorySpareChannelCtorBadAllocCoverageInternal)
{
    auto sensor = makeStateSensor(
        33, 0x1533, PLDM_ENTITY_MEMORY_CONTROLLER, 33,
        PLDM_STATESET_ID_PRESENCE,
        "/xyz/openbmc_project/inventory/system/chassis/chassis33/" +
            std::string(96, 's'));
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_spare_alloc_" +
        std::string(96, 'm');
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis33/" +
            std::string(96, 'a'));

    EXPECT_TRUE(exerciseBadAlloc([&] {
        StateSetMemorySpareChannelCoverage stateSet(
            PLDM_STATESET_ID_PRESENCE, 0, path, association, *sensor);
    }));
}

TEST_F(StateSetNvlinkInternalTest,
       memorySpareChannelCtorDeepBadAllocCoverageInternal)
{
    auto sensor = makeStateSensor(
        50, 0x1550, PLDM_ENTITY_MEMORY_CONTROLLER, 50,
        PLDM_STATESET_ID_PRESENCE,
        "/xyz/openbmc_project/inventory/system/chassis/chassis50/" +
            std::string(144, 's'));
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_spare_alloc_deep_" +
        std::string(144, 'm');
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis50/" +
            std::string(144, 'a'));

    EXPECT_TRUE(exerciseBadAlloc(
        [&] {
            StateSetMemorySpareChannelCoverage stateSet(
                PLDM_STATESET_ID_PRESENCE, 0, path, association, *sensor);
        },
        512));
}

TEST_F(StateSetNvlinkInternalTest,
       memorySpareChannelCtorExhaustiveBadAllocCoverageInternal)
{
    auto sensor = makeStateSensor(
        59, 0x1559, PLDM_ENTITY_MEMORY_CONTROLLER, 59,
        PLDM_STATESET_ID_PRESENCE,
        "/xyz/openbmc_project/inventory/system/chassis/chassis59/" +
            std::string(160, 's'));
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_spare_alloc_sweep_" +
        std::string(160, 'm');
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis59/" +
            std::string(160, 'a'));

    EXPECT_TRUE(exerciseAllBadAlloc(
        [&] {
            StateSetMemorySpareChannelCoverage stateSet(
                PLDM_STATESET_ID_PRESENCE, 0, path, association, *sensor);
        },
        512));
}

TEST_F(StateSetNvlinkInternalTest,
       memorySpareChannelUpdateShmemReadingBadAllocCoverageInternal)
{
    auto sensor = makeStateSensor(
        34, 0x1534, PLDM_ENTITY_MEMORY_CONTROLLER, 34,
        PLDM_STATESET_ID_PRESENCE,
        "/xyz/openbmc_project/inventory/system/chassis/chassis34");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis34"}, false);

    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_spare_update_" +
        std::string(96, 'u');
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis34");
    StateSetMemorySpareChannelCoverage stateSet(PLDM_STATESET_ID_PRESENCE, 0,
                                                path, association, *sensor);

    Associations associations{
        {"chassis", "all_states",
         "/xyz/openbmc_project/inventory/system/chassis/chassis34/" +
             std::string(96, 'e')}};
    stateSet.setAssociations(associations);

    EXPECT_TRUE(exerciseBadAlloc([&] {
        stateSet.updateShmemReading("MemorySpareChannelPresence");
    }));
}

TEST_F(StateSetNvlinkInternalTest,
       memorySpareChannelUpdateShmemReadingDeepBadAllocCoverageInternal)
{
    auto sensor = makeStateSensor(
        53, 0x1553, PLDM_ENTITY_MEMORY_CONTROLLER, 53,
        PLDM_STATESET_ID_PRESENCE,
        "/xyz/openbmc_project/inventory/system/chassis/chassis53");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis53"}, false);

    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_spare_update_deep_" +
        std::string(144, 'u');
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis53");
    StateSetMemorySpareChannelCoverage stateSet(PLDM_STATESET_ID_PRESENCE, 0,
                                                path, association, *sensor);

    Associations associations{
        {"chassis", "all_states",
         "/xyz/openbmc_project/inventory/system/chassis/chassis53/" +
             std::string(144, 'e')}};
    stateSet.setAssociations(associations);

    EXPECT_TRUE(exerciseBadAlloc(
        [&] {
            stateSet.updateShmemReading(
                "MemorySpareChannelPresence" + std::string(96, 'q'));
        },
        512));
}

TEST_F(StateSetNvlinkInternalTest,
       memorySpareChannelUpdateShmemReadingExhaustiveBadAllocCoverageInternal)
{
    auto sensor = makeStateSensor(
        63, 0x1563, PLDM_ENTITY_MEMORY_CONTROLLER, 63,
        PLDM_STATESET_ID_PRESENCE,
        "/xyz/openbmc_project/inventory/system/chassis/chassis63");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis63"}, false);

    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_spare_update_sweep_" +
        std::string(160, 'u');
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis63");
    StateSetMemorySpareChannelCoverage stateSet(PLDM_STATESET_ID_PRESENCE, 0,
                                                path, association, *sensor);

    Associations associations{
        {"chassis", "all_states",
         "/xyz/openbmc_project/inventory/system/chassis/chassis63/" +
             std::string(160, 'e')}};
    stateSet.setAssociations(associations);

    EXPECT_TRUE(exerciseAllBadAlloc(
        [&] {
            stateSet.updateShmemReading(
                "MemorySpareChannelPresence" + std::string(112, 'q'));
        },
        512));
}

TEST_F(StateSetNvlinkInternalTest,
       memorySpareChannelUpdateShmemReadingLongAssociationCoverageInternal)
{
    auto sensor = makeStateSensor(
        37, 0x1537, PLDM_ENTITY_MEMORY_CONTROLLER, 37,
        PLDM_STATESET_ID_PRESENCE,
        "/xyz/openbmc_project/inventory/system/chassis/chassis37");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis37"}, false);

    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_spare_long_assoc_" +
        std::string(64, 's');
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis37");
    StateSetMemorySpareChannelCoverage stateSet(PLDM_STATESET_ID_PRESENCE, 0,
                                                path, association, *sensor);

    Associations associations{
        {"chassis_" + std::string(48, 'x'), "all_states",
         "/xyz/openbmc_project/inventory/system/ignored/" +
             std::string(48, 'a')},
        {"chassis", "all_states",
         "/xyz/openbmc_project/inventory/system/chassis/chassis37/" +
             std::string(96, 'b')}};
    stateSet.setAssociations(associations);

    EXPECT_NO_THROW(stateSet.updateShmemReading(
        "MemorySpareChannelPresence" + std::string(64, 'p')));
}

TEST_F(StateSetNvlinkInternalTest,
       processorPowerBreakCtorBadAllocCoverageInternal)
{
    auto sensor = makeStateSensor(
        35, STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC, 35,
        PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu35/" +
            std::string(96, 'p'));
    std::string path =
        "/xyz/openbmc_project/state/coverage/processor_power_break_alloc_" +
        std::string(96, 'p');
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory35/" +
            std::string(96, 'a'));

    EXPECT_TRUE(exerciseBadAlloc([&] {
        StateSetProcessorPowerBreakCoverage stateSet(
            PLDM_STATESET_ID_PERFORMANCE, 0, path, association, *sensor);
    }));
}

TEST_F(StateSetNvlinkInternalTest,
       processorPowerBreakCtorDeepBadAllocCoverageInternal)
{
    auto sensor = makeStateSensor(
        51, STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC, 51,
        PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu51/" +
            std::string(144, 'p'));
    std::string path =
        "/xyz/openbmc_project/state/coverage/processor_power_break_alloc_deep_" +
        std::string(144, 'p');
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory51/" +
            std::string(144, 'a'));

    EXPECT_TRUE(exerciseBadAlloc(
        [&] {
            StateSetProcessorPowerBreakCoverage stateSet(
                PLDM_STATESET_ID_PERFORMANCE, 0, path, association, *sensor);
        },
        512));
}

TEST_F(StateSetNvlinkInternalTest,
       processorPowerBreakCtorExhaustiveBadAllocCoverageInternal)
{
    auto sensor = makeStateSensor(
        60, STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC, 60,
        PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu60/" +
            std::string(160, 'p'));
    std::string path =
        "/xyz/openbmc_project/state/coverage/processor_power_break_alloc_sweep_" +
        std::string(160, 'p');
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory60/" +
            std::string(160, 'a'));

    EXPECT_TRUE(exerciseAllBadAlloc(
        [&] {
            StateSetProcessorPowerBreakCoverage stateSet(
                PLDM_STATESET_ID_PERFORMANCE, 0, path, association, *sensor);
        },
        512));
}

TEST_F(StateSetNvlinkInternalTest,
       processorPowerBreakUpdateShmemReadingBadAllocCoverageInternal)
{
    auto sensor = makeStateSensor(
        36, STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC, 36,
        PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu36");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu36"}, false);

    std::string path =
        "/xyz/openbmc_project/state/coverage/processor_power_break_update_" +
        std::string(96, 'u');
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory36");
    StateSetProcessorPowerBreakCoverage stateSet(PLDM_STATESET_ID_PERFORMANCE,
                                                 0, path, association, *sensor);

    Associations associations{
        {"memory", "all_states",
         "/xyz/openbmc_project/inventory/system/memory/memory36/" +
             std::string(96, 'e')}};
    stateSet.setAssociations(associations);

    EXPECT_TRUE(
        exerciseBadAlloc([&] { stateSet.updateShmemReading("Value"); }));
}

TEST_F(StateSetNvlinkInternalTest,
       processorPowerBreakUpdateShmemReadingDeepBadAllocCoverageInternal)
{
    auto sensor = makeStateSensor(
        54, STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC, 54,
        PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu54");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu54"}, false);

    std::string path =
        "/xyz/openbmc_project/state/coverage/processor_power_break_update_deep_" +
        std::string(144, 'u');
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory54");
    StateSetProcessorPowerBreakCoverage stateSet(PLDM_STATESET_ID_PERFORMANCE,
                                                 0, path, association, *sensor);

    Associations associations{
        {"memory", "all_states",
         "/xyz/openbmc_project/inventory/system/memory/memory54/" +
             std::string(144, 'e')}};
    stateSet.setAssociations(associations);

    EXPECT_TRUE(exerciseBadAlloc(
        [&] { stateSet.updateShmemReading("Value" + std::string(96, 'r')); },
        512));
}

TEST_F(StateSetNvlinkInternalTest,
       processorPowerBreakUpdateShmemReadingExhaustiveBadAllocCoverageInternal)
{
    auto sensor = makeStateSensor(
        64, STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC, 64,
        PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu64");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu64"}, false);

    std::string path =
        "/xyz/openbmc_project/state/coverage/processor_power_break_update_sweep_" +
        std::string(160, 'u');
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory64");
    StateSetProcessorPowerBreakCoverage stateSet(PLDM_STATESET_ID_PERFORMANCE,
                                                 0, path, association, *sensor);

    Associations associations{
        {"memory", "all_states",
         "/xyz/openbmc_project/inventory/system/memory/memory64/" +
             std::string(160, 'e')}};
    stateSet.setAssociations(associations);

    EXPECT_TRUE(exerciseAllBadAlloc(
        [&] { stateSet.updateShmemReading("Value" + std::string(112, 'r')); },
        512));
}

TEST_F(StateSetNvlinkInternalTest,
       processorPowerBreakUpdateShmemReadingLongAssociationCoverageInternal)
{
    auto sensor = makeStateSensor(
        38, STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC, 38,
        PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu38");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu38"}, false);

    std::string path =
        "/xyz/openbmc_project/state/coverage/processor_power_break_long_assoc_" +
        std::string(64, 'p');
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory38");
    StateSetProcessorPowerBreakCoverage stateSet(PLDM_STATESET_ID_PERFORMANCE,
                                                 0, path, association, *sensor);

    Associations associations{
        {"memory_" + std::string(48, 'x'), "all_states",
         "/xyz/openbmc_project/inventory/system/ignored/" +
             std::string(48, 'a')},
        {"memory", "all_states",
         "/xyz/openbmc_project/inventory/system/memory/memory38/" +
             std::string(96, 'b')}};
    stateSet.setAssociations(associations);

    EXPECT_NO_THROW(
        stateSet.updateShmemReading("Value" + std::string(96, 'p')));
}

TEST_F(StateSetNvlinkInternalTest,
       nvlinkUpdateShmemReadingLongAssociationCoverageInternal)
{
    auto sensor = makeStateSensor(
        39, 0x1539, PLDM_ENTITY_SYS_BUS, 39, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric39");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric39"}, false);

    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis39/CPU_0");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_long_assoc";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *sensor);

    stateSet.setAssociations(
        {{"chassis_" + std::string(48, 'x'), "all_states",
          "/xyz/openbmc_project/inventory/system/ignored/" +
              std::string(48, 'a')},
         {"chassis", "all_states",
          "/xyz/openbmc_project/inventory/system/chassis/chassis39/CPU_0/" +
              std::string(96, 'b')}});

    EXPECT_NO_THROW(stateSet.updateShmemReading(std::string(96, 'L')));
    EXPECT_NO_THROW(stateSet.updateShmemReading("LinkStatus"));
    EXPECT_NO_THROW(stateSet.updateShmemReading("LinkState"));
}

TEST_F(StateSetNvlinkInternalTest,
       nvlinkUpdateShmemReadingDeepBadAllocCoverageInternal)
{
    auto sensor = makeStateSensor(
        55, 0x1555, PLDM_ENTITY_SYS_BUS, 55, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric55");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric55"}, false);

    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis55/CPU_0");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_update_deep";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *sensor);

    stateSet.setAssociations(
        {{"chassis", "all_states",
          "/xyz/openbmc_project/inventory/system/chassis/chassis55/CPU_0/" +
              std::string(144, 'b')}});

    EXPECT_TRUE(exerciseBadAlloc(
        [&] {
            stateSet.updateShmemReading("LinkState" + std::string(96, 'L'));
        },
        512));
}

TEST_F(StateSetNvlinkInternalTest,
       nvlinkUpdateShmemReadingExhaustiveBadAllocCoverageInternal)
{
    auto sensor = makeStateSensor(
        65, 0x1565, PLDM_ENTITY_SYS_BUS, 65, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric65");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric65"}, false);

    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis65/CPU_0");
    std::string path =
        "/xyz/openbmc_project/state/coverage/nvlink_update_sweep";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *sensor);

    stateSet.setAssociations(
        {{"chassis", "all_states",
          "/xyz/openbmc_project/inventory/system/chassis/chassis65/CPU_0/" +
              std::string(160, 'b')}});

    EXPECT_TRUE(exerciseAllBadAlloc(
        [&] {
            stateSet.updateShmemReading("LinkState" + std::string(112, 'L'));
        },
        512));
}

TEST_F(StateSetNvlinkInternalTest,
       memoryPerformanceUpdateShmemReadingEmptyEndpointCoverageInternal)
{
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_performance_empty_endpoint";
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory_empty_endpoint");
    StateSetMemoryPerformanceCoverage stateSet(PLDM_STATESET_ID_PERFORMANCE, 0,
                                               path, association);

    stateSet.setAssociations({{"memory", "all_states", ""}});
    EXPECT_NO_THROW(
        stateSet.updateShmemReading("Value" + std::string(80, 'e')));
}

TEST_F(StateSetNvlinkInternalTest,
       memorySpareChannelUpdateShmemReadingDefaultInventoryCoverageInternal)
{
    auto sensor = makeStateSensor(
        41, 0x1541, PLDM_ENTITY_MEMORY_CONTROLLER, 41,
        PLDM_STATESET_ID_PRESENCE,
        "/xyz/openbmc_project/inventory/system/chassis/chassis41");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis41"}, true);

    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_spare_default_inventory";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis41");
    StateSetMemorySpareChannelCoverage stateSet(PLDM_STATESET_ID_PRESENCE, 0,
                                                path, association, *sensor);

    stateSet.setAssociations(
        {{"chassis", "all_states",
          "/xyz/openbmc_project/inventory/system/chassis/chassis41/" +
              std::string(84, 'd')}});
    EXPECT_NO_THROW(stateSet.updateShmemReading(
        "MemorySpareChannelPresence" + std::string(72, 'D')));
}

TEST_F(StateSetNvlinkInternalTest,
       memorySpareChannelUpdateShmemReadingEmptyEndpointCoverageInternal)
{
    auto sensor = makeStateSensor(
        42, 0x1542, PLDM_ENTITY_MEMORY_CONTROLLER, 42,
        PLDM_STATESET_ID_PRESENCE,
        "/xyz/openbmc_project/inventory/system/chassis/chassis42");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis42"}, false);

    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_spare_empty_endpoint";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis42");
    StateSetMemorySpareChannelCoverage stateSet(PLDM_STATESET_ID_PRESENCE, 0,
                                                path, association, *sensor);

    stateSet.setAssociations({{"chassis", "all_states", ""}});
    EXPECT_NO_THROW(stateSet.updateShmemReading(
        "MemorySpareChannelPresence" + std::string(72, 'E')));
}

TEST_F(StateSetNvlinkInternalTest,
       processorPowerBreakUpdateShmemReadingDefaultInventoryCoverageInternal)
{
    auto sensor = makeStateSensor(
        43, STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC, 43,
        PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu43");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu43"}, true);

    std::string path =
        "/xyz/openbmc_project/state/coverage/processor_power_break_default_inventory";
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory43");
    StateSetProcessorPowerBreakCoverage stateSet(PLDM_STATESET_ID_PERFORMANCE,
                                                 0, path, association, *sensor);

    stateSet.setAssociations(
        {{"memory", "all_states",
          "/xyz/openbmc_project/inventory/system/memory/memory43/" +
              std::string(88, 'd')}});
    EXPECT_NO_THROW(
        stateSet.updateShmemReading("Value" + std::string(76, 'P')));
}

TEST_F(StateSetNvlinkInternalTest,
       processorPowerBreakUpdateShmemReadingEmptyEndpointCoverageInternal)
{
    auto sensor = makeStateSensor(
        44, STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC, 44,
        PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu44");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu44"}, false);

    std::string path =
        "/xyz/openbmc_project/state/coverage/processor_power_break_empty_endpoint";
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory44");
    StateSetProcessorPowerBreakCoverage stateSet(PLDM_STATESET_ID_PERFORMANCE,
                                                 0, path, association, *sensor);

    stateSet.setAssociations({{"memory", "all_states", ""}});
    EXPECT_NO_THROW(
        stateSet.updateShmemReading("Value" + std::string(76, 'E')));
}

TEST_F(StateSetNvlinkInternalTest,
       nvlinkUpdateShmemReadingDefaultInventoryCoverageInternal)
{
    auto sensor = makeStateSensor(
        45, 0x1545, PLDM_ENTITY_SYS_BUS, 45, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric45");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric45"}, true);

    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis45/CPU_0");
    std::string path =
        "/xyz/openbmc_project/state/coverage/nvlink_default_inventory";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *sensor);

    stateSet.setAssociations(
        {{"chassis", "all_states",
          "/xyz/openbmc_project/inventory/system/chassis/chassis45/CPU_0/" +
              std::string(88, 'd')}});
    EXPECT_NO_THROW(
        stateSet.updateShmemReading("LinkState" + std::string(64, 'D')));
    EXPECT_NO_THROW(
        stateSet.updateShmemReading("LinkStatus" + std::string(64, 'S')));
}

TEST_F(StateSetNvlinkInternalTest,
       nvlinkUpdateShmemReadingEmptyEndpointCoverageInternal)
{
    auto sensor = makeStateSensor(
        46, 0x1546, PLDM_ENTITY_SYS_BUS, 46, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric46");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric46"}, false);

    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis46/CPU_0");
    std::string path =
        "/xyz/openbmc_project/state/coverage/nvlink_empty_endpoint";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *sensor);

    stateSet.setAssociations({{"chassis", "all_states", ""}});
    EXPECT_NO_THROW(stateSet.updateShmemReading("LinkState"));
    EXPECT_NO_THROW(stateSet.updateShmemReading("LinkStatus"));
}

TEST_F(StateSetNvlinkInternalTest,
       nvlinkSetAssociationPrefersNonChassisCpuAssociationCoverageInternal)
{
    auto sensor = makeStateSensor(
        47, 0x1547, PLDM_ENTITY_SYS_BUS, 47, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric47");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric47"}, false);

    pldm::utils::NvlinkTestDBusHandler::state().subtreeResponse = {
        {"/xyz/openbmc_project/inventory/system/chassis/chassis47/CPU_0",
         {{"xyz.openbmc_project.Inventory.Manager",
           {"xyz.openbmc_project.Inventory.Item.Chassis"}}}}};

    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis47/CPU_0");
    std::string path =
        "/xyz/openbmc_project/state/coverage/nvlink_assoc_filter";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *sensor);

    std::vector<pldm::dbus::PathAssociation> associations{
        {"chassis", "all_states",
         "/xyz/openbmc_project/inventory/system/chassis/chassis47/CPU_0"},
        {"chassis", "all_states",
         "/xyz/openbmc_project/inventory/system/chassis/chassis47/CPU_1"}};
    stateSet.setAssociation(associations);

    auto updatedAssociations = stateSet.getAssociations();
    ASSERT_EQ(updatedAssociations.size(), 1u);
    EXPECT_EQ("/xyz/openbmc_project/inventory/system/chassis/chassis47/CPU_1",
              std::get<2>(updatedAssociations.front()));
}

} // namespace
