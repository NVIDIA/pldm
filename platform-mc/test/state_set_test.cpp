/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 */

#include "libpldm/entity.h"
#include "libpldm/oem/nvidia/state_set_oem_nvidia.h"
#include "libpldm/platform.h"

#include "../../test/test_valgrind_utils.hpp"
#include "common/utils.hpp"
#include "oem/nvidia/platform-mc/derived_sensor/switchBandwidthSensor.hpp"
#include "oem/nvidia/platform-mc/remoteDebug.hpp"
#include "oem/nvidia/platform-mc/state_set/memoryPerformance.hpp"
#include "oem/nvidia/platform-mc/state_set/memorySpareChannel.hpp"
#include "oem/nvidia/platform-mc/state_set/processorPowerBreak.hpp"
#include "platform-mc/numeric_sensor.hpp"
#include "platform-mc/state_sensor.hpp"
#include "platform-mc/state_set.hpp"
#include "platform-mc/state_set/clearNonVolatileVariables.hpp"
#include "platform-mc/state_set/healthState.hpp"
#include "platform-mc/state_set/performance.hpp"
#include "platform-mc/state_set/powerSupplyInput.hpp"
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#define protected public
#include "oem/nvidia/platform-mc/state_set/nvlink.hpp"
#include "platform-mc/state_set/ethIBPortLinkState.hpp"
#include "platform-mc/state_set/pciePortLinkState.hpp"
#undef protected
#undef private
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <xyz/openbmc_project/Inventory/Item/Chassis/server.hpp>
#include <xyz/openbmc_project/ObjectMapper/server.hpp>

#include <array>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace pldm::platform_mc;

namespace stateSetTestAlloc
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

} // namespace stateSetTestAlloc

void* operator new(std::size_t size)
{
    return stateSetTestAlloc::allocate(size);
}

void* operator new[](std::size_t size)
{
    return stateSetTestAlloc::allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    return stateSetTestAlloc::allocate(size,
                                       static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return stateSetTestAlloc::allocate(size,
                                       static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return stateSetTestAlloc::allocate(size);
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
        return stateSetTestAlloc::allocate(size);
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
            stateSetTestAlloc::ScopedFailure failure(failIndex);
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

class NvlinkObjectMapper :
    public sdbusplus::server::object_t<
        sdbusplus::xyz::openbmc_project::server::ObjectMapper>
{
  public:
    NvlinkObjectMapper(sdbusplus::bus::bus& bus, const char* path,
                       std::string serviceName, std::string chassisPath) :
        sdbusplus::server::object_t<
            sdbusplus::xyz::openbmc_project::server::ObjectMapper>(bus, path),
        serviceName(std::move(serviceName)), chassisPath(std::move(chassisPath))
    {}

    std::map<std::string, std::vector<std::string>> getObject(
        std::string path, std::vector<std::string> interfaces) override
    {
        if (path != chassisPath)
        {
            return {};
        }

        if (interfaces.empty())
        {
            return {{serviceName, {instanceInterface, chassisInterface}}};
        }

        for (const auto& interface : interfaces)
        {
            if (interface == instanceInterface || interface == chassisInterface)
            {
                return {{serviceName, {interface}}};
            }
        }

        return {};
    }

    std::map<std::string, std::map<std::string, std::vector<std::string>>>
        getAncestors(std::string, std::vector<std::string>) override
    {
        return {};
    }

    std::map<std::string, std::map<std::string, std::vector<std::string>>>
        getSubTree(std::string subtree, int32_t,
                   std::vector<std::string> interfaces) override
    {
        if (subtree != chassisPath)
        {
            return {};
        }

        if (!interfaces.empty())
        {
            bool hasChassis = false;
            for (const auto& interface : interfaces)
            {
                if (interface == chassisInterface)
                {
                    hasChassis = true;
                    break;
                }
            }
            if (!hasChassis)
            {
                return {};
            }
        }

        return {{chassisPath, {{serviceName, {chassisInterface}}}}};
    }

    std::vector<std::string> getSubTreePaths(std::string subtree, int32_t,
                                             std::vector<std::string>) override
    {
        if (subtree == chassisPath)
        {
            return {chassisPath};
        }
        return {};
    }

    std::map<std::string, std::map<std::string, std::vector<std::string>>>
        getAssociatedSubTree(sdbusplus::message::object_path,
                             sdbusplus::message::object_path, int32_t,
                             std::vector<std::string>) override
    {
        return {};
    }

    std::vector<std::string> getAssociatedSubTreePaths(
        std::string, std::string, int32_t, std::vector<std::string>) override
    {
        return {};
    }

    std::map<std::string, std::map<std::string, std::vector<std::string>>>
        getAssociatedSubTreeById(std::string, std::string,
                                 std::vector<std::string>, std::string,
                                 std::vector<std::string>) override
    {
        return {};
    }

    std::vector<std::string> getAssociatedSubTreePathsById(
        std::string, std::string, std::vector<std::string>, std::string,
        std::vector<std::string>) override
    {
        return {};
    }

  private:
    static constexpr auto instanceInterface =
        "xyz.openbmc_project.Inventory.Decorator.Instance";
    static constexpr auto chassisInterface =
        "xyz.openbmc_project.Inventory.Item.Chassis";

    std::string serviceName;
    std::string chassisPath;
};

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

std::shared_ptr<NumericSensor> makeNumericSensor(
    uint8_t tid, uint16_t sensorId, uint16_t entityType,
    uint16_t entityInstance, uint8_t baseUnit, const std::string& sensorName)
{
    auto pdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    pdr->sensor_id = sensorId;
    pdr->entity_type = entityType;
    pdr->entity_instance = entityInstance;
    pdr->container_id = 1;
    pdr->base_unit = baseUnit;
    pdr->unit_modifier = 0;
    pdr->aux_unit = PLDM_SENSOR_UNIT_NONE;
    pdr->sensor_data_size = PLDM_SENSOR_DATA_SIZE_UINT32;
    pdr->resolution = 1.0f;
    pdr->offset = 0.0f;
    pdr->is_linear = true;
    pdr->range_field_format = PLDM_RANGE_FIELD_FORMAT_UINT32;
    pdr->range_field_support.byte = 0;
    pdr->max_readable.value_u32 = 100000;
    pdr->min_readable.value_u32 = 0;
    pdr->nominal_value.value_u32 = 0;
    pdr->normal_max.value_u32 = 0;
    pdr->normal_min.value_u32 = 0;

    std::string mutableName = sensorName;
    std::string associationPath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis0";
    return std::make_shared<NumericSensor>(tid, false, pdr, mutableName,
                                           associationPath, nullptr);
}

class StateSetNvlinkCoverage : public oem_nvidia::StateSetNvlink
{
  public:
    using oem_nvidia::StateSetNvlink::StateSetNvlink;

    Associations getAssociations() const
    {
        return associationDefinitionsIntf->associations();
    }

    void resetAssociationDefinitionsIntf()
    {
        associationDefinitionsIntf.reset();
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

class StateSetPerformanceCoverage : public StateSetPerformance
{
  public:
    using StateSetPerformance::StateSetPerformance;

    void setAssociations(const Associations& assocs)
    {
        associationDefinitionsIntf->associations(assocs);
    }
};

class StateSetPowerSupplyInputCoverage : public StateSetPowerSupplyInput
{
  public:
    using StateSetPowerSupplyInput::StateSetPowerSupplyInput;

    void setAssociations(const Associations& assocs)
    {
        associationDefinitionsIntf->associations(assocs);
    }
};

class StateSetPciePortLinkStateCoverage : public StateSetPciePortLinkState
{
  public:
    using StateSetPciePortLinkState::StateSetPciePortLinkState;

    void setAssociations(const Associations& assocs)
    {
        associationDefinitionsIntf->associations(assocs);
    }
};

class StateSetHealthStateCoverage : public StateSetHealthState
{
  public:
    using StateSetHealthState::StateSetHealthState;

    void resetAssociationDefinitionsIntf()
    {
        associationDefinitionsIntf.reset();
    }
};

class StateSetEthIBPortLinkStateRenameCoverage :
    public StateSetEthIBPortLinkState
{
  public:
    using StateSetEthIBPortLinkState::StateSetEthIBPortLinkState;

    void resetAssociationDefinitionsIntf()
    {
        associationDefinitionsIntf.reset();
    }

    void resetPortIntf()
    {
        ValuePortIntf.reset();
    }

    void resetPortInfoIntf()
    {
        ValuePortInfoIntf.reset();
    }

    void resetPortStateIntf()
    {
        ValuePortStateIntf.reset();
    }

    void setValue(uint8_t value) override
    {
        presentState = value;
        if (ValuePortStateIntf && ValuePortInfoIntf)
        {
            StateSetEthIBPortLinkState::setValue(value);
        }
    }

    void setDefaultValue() override
    {
        if (ValuePortStateIntf && ValuePortInfoIntf)
        {
            StateSetEthIBPortLinkState::setDefaultValue();
        }
    }
};

class BaseStateSetCoverage : public StateSet
{
  public:
    using StateSet::StateSet;

    void initAssociationDefinitions(const std::string& path)
    {
        associationDefinitionsIntf =
            std::make_unique<AssociationDefinitionsInft>(
                pldm::utils::DBusHandler::getBus(), path.c_str());
    }

    void resetAssociationDefinitions()
    {
        associationDefinitionsIntf.reset();
    }

    Associations getAssociations() const
    {
        if (!associationDefinitionsIntf)
        {
            return {};
        }
        return associationDefinitionsIntf->associations();
    }

    void setValue(uint8_t) override {}

    void setDefaultValue() override {}

    std::string getStringStateType() const override
    {
        return "Base";
    }

    std::tuple<std::string, std::string, Level, std::string, std::string>
        getEventData(pldm::utils::SensorEventInfo*) const override
    {
        return {"", "", Level::Informational, "", ""};
    }
};

class StateSetCoverageTest : public testing::Test
{
  protected:
    sdbusplus::bus::bus& bus = pldm::utils::DBusHandler::getBus();
};

void ensureMapperServiceRequested(sdbusplus::bus::bus& bus)
{
    try
    {
        bus.request_name(pldm::utils::mapperService);
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        if (std::string(e.what()).find("EALREADY") == std::string::npos)
        {
            throw;
        }
    }
}

TEST_F(StateSetCoverageTest, stateSetCreatorSensorMatrixCoverage)
{
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis0");
    auto nullPath =
        std::string("/xyz/openbmc_project/state/coverage/creator/null");
    auto presencePath =
        std::string("/xyz/openbmc_project/state/coverage/creator/presence");
    auto nvlinkPath =
        std::string("/xyz/openbmc_project/state/coverage/creator/nvlink");
    auto debugPath =
        std::string("/xyz/openbmc_project/state/coverage/creator/debug");
    auto memoryPerformancePath = std::string(
        "/xyz/openbmc_project/state/coverage/creator/memory_performance");
    auto powerBreakPath =
        std::string("/xyz/openbmc_project/state/coverage/creator/power_break");
    auto performancePath =
        std::string("/xyz/openbmc_project/state/coverage/creator/performance");
    auto powerSupplyPath =
        std::string("/xyz/openbmc_project/state/coverage/creator/powersupply");
    auto pciePath =
        std::string("/xyz/openbmc_project/state/coverage/creator/pcie");
    auto bootPath =
        std::string("/xyz/openbmc_project/state/coverage/creator/boot");
    auto ethPath =
        std::string("/xyz/openbmc_project/state/coverage/creator/eth");
    auto healthPath =
        std::string("/xyz/openbmc_project/state/coverage/creator/health");
    auto unknownPath =
        std::string("/xyz/openbmc_project/state/coverage/creator/unknown");

    EXPECT_EQ(nullptr,
              StateSetCreator::createSensor(PLDM_STATESET_ID_HEALTHSTATE, 0,
                                            nullPath, association, nullptr));

    auto presenceSensor = makeStateSensor(
        1, 0x1000, PLDM_ENTITY_PROC, 1, PLDM_STATESET_ID_PRESENCE,
        "/xyz/openbmc_project/inventory/system/board/proc0");
    auto presenceState = StateSetCreator::createSensor(
        PLDM_STATESET_ID_PRESENCE, 0, presencePath, association,
        presenceSensor.get());
    EXPECT_NE(nullptr,
              dynamic_cast<StateSetMemorySpareChannel*>(presenceState.get()));

    auto nvlinkSensor = makeStateSensor(
        1, 0x1001, PLDM_ENTITY_SYS_BUS, 2, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric0");
    auto nvlinkState = StateSetCreator::createSensor(
        PLDM_NVIDIA_OEM_STATE_SET_NVLINK, 1, nvlinkPath, association,
        nvlinkSensor.get());
    EXPECT_NE(nullptr,
              dynamic_cast<oem_nvidia::StateSetNvlink*>(nvlinkState.get()));

    auto debugSensor =
        makeStateSensor(1, 0x1002, PLDM_ENTITY_SYS_BOARD, 1,
                        PLDM_NVIDIA_OEM_STATE_SET_DEBUG_STATE,
                        "/xyz/openbmc_project/inventory/system/board/board0");
    auto debugState = StateSetCreator::createSensor(
        PLDM_NVIDIA_OEM_STATE_SET_DEBUG_STATE, 2, debugPath, association,
        debugSensor.get());
    EXPECT_NE(nullptr,
              dynamic_cast<oem_nvidia::StateSetDebugState*>(debugState.get()));

    auto memoryPerfSensor =
        makeStateSensor(1, 0x1003, PLDM_ENTITY_MEMORY_CONTROLLER, 3,
                        PLDM_STATESET_ID_PERFORMANCE,
                        "/xyz/openbmc_project/inventory/system/memory/memory0");
    auto memoryPerfState = StateSetCreator::createSensor(
        PLDM_STATESET_ID_PERFORMANCE, 3, memoryPerformancePath, association,
        memoryPerfSensor.get());
    EXPECT_NE(nullptr,
              dynamic_cast<StateSetMemoryPerformance*>(memoryPerfState.get()));

    auto powerBreakSensor =
        makeStateSensor(1, STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC, 4,
                        PLDM_STATESET_ID_PERFORMANCE,
                        "/xyz/openbmc_project/inventory/system/processor/cpu0");
    auto powerBreakState = StateSetCreator::createSensor(
        PLDM_STATESET_ID_PERFORMANCE, 4, powerBreakPath, association,
        powerBreakSensor.get());
    EXPECT_NE(nullptr, dynamic_cast<StateSetProcessorPowerBreak*>(
                           powerBreakState.get()));

    auto performanceSensor = makeStateSensor(
        1, 0x1004, PLDM_ENTITY_PROC, 5, PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu1");
    auto performanceState = StateSetCreator::createSensor(
        PLDM_STATESET_ID_PERFORMANCE, 5, performancePath, association,
        performanceSensor.get());
    EXPECT_NE(nullptr,
              dynamic_cast<StateSetPerformance*>(performanceState.get()));

    auto powerSupplySensor = makeStateSensor(
        1, 0x1005, PLDM_ENTITY_POWER_SUPPLY, 6, PLDM_STATESET_ID_POWERSUPPLY,
        "/xyz/openbmc_project/inventory/system/powersupply/psu0");
    auto powerSupplyState = StateSetCreator::createSensor(
        PLDM_STATESET_ID_POWERSUPPLY, 6, powerSupplyPath, association,
        powerSupplySensor.get());
    EXPECT_NE(nullptr,
              dynamic_cast<StateSetPowerSupplyInput*>(powerSupplyState.get()));

    auto pcieSensor = makeStateSensor(
        1, 0x1006, PLDM_ENTITY_PCI_EXPRESS_BUS, 7, PLDM_STATESET_ID_LINKSTATE,
        "/xyz/openbmc_project/inventory/system/pcie/slot0");
    auto pcieState = StateSetCreator::createSensor(
        PLDM_STATESET_ID_LINKSTATE, 7, pciePath, association, pcieSensor.get());
    EXPECT_NE(nullptr,
              dynamic_cast<StateSetPciePortLinkState*>(pcieState.get()));

    auto bootSensor = makeStateSensor(
        1, 0x1007, PLDM_ENTITY_SYS_BOARD, 8, PLDM_STATESET_ID_BOOT_REQUEST,
        "/xyz/openbmc_project/inventory/system/board/board1");
    auto bootState =
        StateSetCreator::createSensor(PLDM_STATESET_ID_BOOT_REQUEST, 8,
                                      bootPath, association, bootSensor.get());
    EXPECT_NE(nullptr,
              dynamic_cast<StateSetClearNonvolatileVariable*>(bootState.get()));

    auto ethSensor = makeStateSensor(
        1, 0x1008, PLDM_ENTITY_ETHERNET, 9, PLDM_STATESET_ID_LINKSTATE,
        "/xyz/openbmc_project/inventory/system/network/eth0");
    auto ethState = StateSetCreator::createSensor(
        PLDM_STATESET_ID_LINKSTATE, 9, ethPath, association, ethSensor.get());
    EXPECT_NE(nullptr,
              dynamic_cast<StateSetEthIBPortLinkState*>(ethState.get()));

    auto healthSensor = makeStateSensor(
        1, 0x1009, PLDM_ENTITY_SYS_BOARD, 10, PLDM_STATESET_ID_HEALTHSTATE,
        "/xyz/openbmc_project/inventory/system/board/board2");
    auto healthState = StateSetCreator::createSensor(
        PLDM_STATESET_ID_HEALTHSTATE, 10, healthPath, association,
        healthSensor.get());
    EXPECT_NE(nullptr, dynamic_cast<StateSetHealthState*>(healthState.get()));

    EXPECT_EQ(nullptr,
              StateSetCreator::createSensor(0xFFFE, 0, unknownPath, association,
                                            healthSensor.get()));
}

TEST_F(StateSetCoverageTest, stateSetCreatorMemoryControllerPresenceCoverage)
{
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis11");
    auto path = std::string(
        "/xyz/openbmc_project/state/coverage/creator/presence_memory");
    auto sensor = makeStateSensor(
        1, 0x1011, PLDM_ENTITY_MEMORY_CONTROLLER, 11, PLDM_STATESET_ID_PRESENCE,
        "/xyz/openbmc_project/inventory/system/memory/memory11");

    auto state = StateSetCreator::createSensor(PLDM_STATESET_ID_PRESENCE, 0,
                                               path, association, sensor.get());
    EXPECT_NE(nullptr, dynamic_cast<StateSetMemorySpareChannel*>(state.get()));
}

TEST_F(StateSetCoverageTest, stateSetCreatorInfiniBandCoverage)
{
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis12");
    auto path =
        std::string("/xyz/openbmc_project/state/coverage/creator/infiniband");
    auto sensor = makeStateSensor(
        1, 0x1012, PLDM_ENTITY_INFINIBAND, 12, PLDM_STATESET_ID_LINKSTATE,
        "/xyz/openbmc_project/inventory/system/network/ib12");

    auto state = StateSetCreator::createSensor(PLDM_STATESET_ID_LINKSTATE, 0,
                                               path, association, sensor.get());
    EXPECT_NE(nullptr, dynamic_cast<StateSetEthIBPortLinkState*>(state.get()));
}

TEST_F(StateSetCoverageTest, stateSetCreatorNvlinkWrongEntityCoverage)
{
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis13");
    auto path = std::string(
        "/xyz/openbmc_project/state/coverage/creator/nvlink_wrong_entity");
    auto sensor = makeStateSensor(
        1, 0x1013, PLDM_ENTITY_SYS_BOARD, 13, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/board/board13");

    auto state = StateSetCreator::createSensor(
        PLDM_NVIDIA_OEM_STATE_SET_NVLINK, 0, path, association, sensor.get());
    EXPECT_EQ(nullptr, state);
}

TEST_F(StateSetCoverageTest, baseStateSetAssociationCoverage)
{
    BaseStateSetCoverage stateSet(0x1555);
    std::string path = "/xyz/openbmc_project/state/coverage/base_state_set";
    stateSet.initAssociationDefinitions(path);

    std::vector<pldm::dbus::PathAssociation> associations{
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/chassis/"
                        "chassis0"),
        makeAssociation("sensors", "all_sensors",
                        "/xyz/openbmc_project/inventory/system/board/board0")};
    stateSet.setAssociation(associations);

    auto stored = stateSet.getAssociations();
    ASSERT_EQ(2u, stored.size());
    EXPECT_EQ("/xyz/openbmc_project/inventory/system/chassis/chassis0",
              std::get<2>(stored.front()));
    EXPECT_EQ("/xyz/openbmc_project/inventory/system/board/board0",
              std::get<2>(stored.back()));

    stateSet.resetAssociationDefinitions();
    stateSet.setAssociation(associations);
    EXPECT_TRUE(stateSet.getAssociations().empty());
}

TEST_F(StateSetCoverageTest, healthStateRenameGuardCoverage)
{
    std::string path = "/xyz/openbmc_project/state/coverage/health/Id_0";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/board/board9");
    StateSetHealthState healthState(PLDM_STATESET_ID_HEALTHSTATE, 0, path,
                                    association);

    healthState.updateSensorName("Id_0");
    healthState.setValue(PLDM_STATESET_HEALTH_STATE_CRITICAL);
    auto [criticalMessage, criticalArg, criticalLevel, criticalEventId,
          criticalImpacted] = healthState.getEventData(nullptr);
    EXPECT_EQ("Critical", criticalArg);
    EXPECT_TRUE(criticalEventId.empty());
    EXPECT_TRUE(criticalImpacted.empty());

    healthState.updateSensorName("Health_0");
    healthState.setDefaultValue();
    auto [okMessage, okArg, okLevel, okEventId,
          okImpacted] = healthState.getEventData(nullptr);
    EXPECT_EQ("OK", okArg);
    EXPECT_TRUE(okEventId.empty());
    EXPECT_TRUE(okImpacted.empty());

    BaseStateSetCoverage baseStateSet(0x1666);
    std::vector<pldm::dbus::PathAssociation> associations{
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/board/board9")};
    baseStateSet.setAssociation(associations);
    EXPECT_TRUE(baseStateSet.getAssociations().empty());
}

TEST_F(StateSetCoverageTest, healthStateWarningCoverage)
{
    std::string path = "/xyz/openbmc_project/state/coverage/health/Health_2";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/board/board11");
    StateSetHealthState healthState(PLDM_STATESET_ID_HEALTHSTATE, 0, path,
                                    association);

    healthState.setValue(PLDM_STATESET_HEALTH_STATE_NON_CRITICAL);
    auto [warningMessage, warningArg, warningLevel, warningEventId,
          warningImpacted] = healthState.getEventData(nullptr);
    EXPECT_EQ("Warning", warningArg);
    EXPECT_TRUE(warningEventId.empty());
    EXPECT_TRUE(warningImpacted.empty());
}

TEST_F(StateSetCoverageTest,
       healthStateRenameWithoutAssociationDefinitionsCoverage)
{
    std::string path = "/xyz/openbmc_project/state/coverage/health/Health_3";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/board/board12");
    StateSetHealthStateCoverage healthState(PLDM_STATESET_ID_HEALTHSTATE, 0,
                                            path, association);

    healthState.setValue(PLDM_STATESET_HEALTH_STATE_FATAL);
    healthState.resetAssociationDefinitionsIntf();
    EXPECT_NO_THROW(healthState.updateSensorName("Health+Board#12"));

    auto [criticalMessage, criticalArg, criticalLevel, criticalEventId,
          criticalImpacted] = healthState.getEventData(nullptr);
    EXPECT_EQ("Critical", criticalArg);
    EXPECT_TRUE(criticalEventId.empty());
    EXPECT_TRUE(criticalImpacted.empty());
}

TEST_F(StateSetCoverageTest, clearNonVolatileVariableStateCoverage)
{
    std::string path =
        "/xyz/openbmc_project/state/coverage/clear_nonvolatile_variable";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/board/board14");
    StateSetClearNonvolatileVariable stateSet(PLDM_STATESET_ID_BOOT_REQUEST, 0,
                                              path, association, nullptr);

    EXPECT_EQ(PLDM_STATESET_BOOT_REQUEST_NORMAL, stateSet.getValue());
    auto [normalMessage, normalArg, normalLevel, normalEventId,
          normalImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("False", normalArg);
    EXPECT_TRUE(normalEventId.empty());
    EXPECT_TRUE(normalImpacted.empty());

    stateSet.setValue(PLDM_STATESET_BOOT_REQUEST_REQUESTED);
    EXPECT_EQ(PLDM_STATESET_BOOT_REQUEST_REQUESTED, stateSet.getValue());
    auto [requestedMessage, requestedArg, requestedLevel, requestedEventId,
          requestedImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("True", requestedArg);
    EXPECT_TRUE(requestedEventId.empty());
    EXPECT_TRUE(requestedImpacted.empty());

    stateSet.setValue(PLDM_STATESET_BOOT_REQUEST_NORMAL);
    EXPECT_EQ(PLDM_STATESET_BOOT_REQUEST_NORMAL, stateSet.getValue());
    EXPECT_EQ("ClearNonvolatileVariable", stateSet.getStringStateType());
}

TEST_F(StateSetCoverageTest, ethIbPortLinkStateCoverage)
{
    auto stateSensor = makeStateSensor(
        2, 0x1100, PLDM_ENTITY_ETHERNET, 3, PLDM_STATESET_ID_LINKSTATE,
        "/xyz/openbmc_project/inventory/system/network/eth3");
    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/network/eth3"}, false);

    auto numericSensor =
        makeNumericSensor(2, 0x2200, PLDM_ENTITY_ETHERNET, 3,
                          PLDM_SENSOR_UNIT_BITS, "link_speed_bits");
    numericSensor->updateReading(true, true, 40000000000.0);

    std::string path = "/xyz/openbmc_project/state/coverage/eth_port_3";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/network/eth3");
    StateSetEthIBPortLinkState stateSet(PLDM_STATESET_ID_LINKSTATE, 0, path,
                                        association, 3);

    std::vector<std::shared_ptr<NumericSensor>> wrongSensors{
        makeNumericSensor(2, 0x2201, PLDM_ENTITY_SYS_BOARD, 3,
                          PLDM_SENSOR_UNIT_WATTS, "wrong_sensor")};
    stateSet.associateNumericSensor(stateSensor->getEntityInfo(), wrongSensors);
    EXPECT_EQ(nullptr, stateSet.linkSpeedSensor);

    std::vector<std::shared_ptr<NumericSensor>> sensors{numericSensor};
    stateSet.associateNumericSensor(stateSensor->getEntityInfo(), sensors);
    ASSERT_NE(nullptr, stateSet.linkSpeedSensor);

    std::string switchType{
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet"};
    std::vector<std::string> switchProtocols{
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet",
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.PCIe"};
    std::vector<pldm::dbus::PathAssociation> switchAssociations{association};
    auto switchBandwidthSensor =
        std::make_shared<oem_nvidia::SwitchBandwidthSensor>(
            2, "switch_bw_3", switchType, switchProtocols, switchAssociations);
    stateSet.associateDerivedSensor(switchBandwidthSensor);
    EXPECT_TRUE(stateSet.isDerivedSensorAssociated());

    stateSet.setPortProtocolValue(PortProtocol::InfiniBand);
    stateSet.setMaxSpeedValue(200.0);
    stateSet.addSharedMemObjectPath(
        "/xyz/openbmc_project/inventory/system/network/eth3");

    stateSet.setValue(PLDM_STATESET_LINK_STATE_CONNECTED);
    EXPECT_EQ(PortLinkStates::Enabled,
              stateSet.ValuePortStateIntf->linkState());
    EXPECT_EQ(PortLinkStatus::LinkUp,
              stateSet.ValuePortStateIntf->linkStatus());
    EXPECT_GT(stateSet.ValuePortInfoIntf->currentSpeed(), 0.0);
    auto [upMessage, upArg, upLevel, upEventId,
          upImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("LinkUp", upArg);
    EXPECT_TRUE(upEventId.empty());
    EXPECT_TRUE(upImpacted.empty());

    stateSet.setValue(PLDM_STATESET_LINK_STATE_DISCONNECTED);
    pldm::utils::SensorEventInfo eventInfo{
        "Ethernet_3", {{"LinkDown", "OpenBMC.0.1.LinkDown"}}};
    auto [downMessage, downArg, downLevel, downEventId,
          downImpacted] = stateSet.getEventData(&eventInfo);
    EXPECT_EQ("LinkDown", downArg);
    EXPECT_EQ("OpenBMC.0.1.LinkDown", downEventId);
    EXPECT_EQ("Ethernet_3", downImpacted);

    stateSet.ValuePortStateIntf->linkState(PortLinkStates::Error);
    stateSet.ValuePortStateIntf->linkStatus(PortLinkStatus::NoLink);
    auto [errorMessage, errorArg, errorLevel, errorEventId,
          errorImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("Error", errorArg);
    EXPECT_TRUE(errorEventId.empty());
    EXPECT_TRUE(errorImpacted.empty());

    stateSet.setValue(0xFF);
    auto [unknownMessage, unknownArg, unknownLevel, unknownEventId,
          unknownImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("Unknown", unknownArg);
    EXPECT_TRUE(unknownEventId.empty());
    EXPECT_TRUE(unknownImpacted.empty());

    stateSet.addAssociation(std::vector<pldm::dbus::PathAssociation>{
        association,
        makeAssociation("ports", "associated_port",
                        "/xyz/openbmc_project/inventory/system/network/"
                        "eth3")});
    EXPECT_EQ(2u, stateSet.associationDefinitionsIntf->associations().size());

    stateSet.updateSensorName("Ethernet Port 3");
    EXPECT_EQ("Ethernet Port 3", stateSet.objectName);
    auto originalPath = stateSet.objectPath;
    stateSet.updateSensorName("Ethernet Port 3");
    EXPECT_EQ(originalPath, stateSet.objectPath);
}

TEST_F(StateSetCoverageTest, ethIbPortLinkStateEventFallbackCoverage)
{
    std::string path = "/xyz/openbmc_project/state/coverage/eth_port_4";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/network/eth4");
    StateSetEthIBPortLinkState stateSet(PLDM_STATESET_ID_LINKSTATE, 0, path,
                                        association, 4);

    stateSet.setValue(PLDM_STATESET_LINK_STATE_DISCONNECTED);
    pldm::utils::SensorEventInfo eventInfo{"Ethernet_4", {}};
    auto [message, arg, level, eventId,
          impacted] = stateSet.getEventData(&eventInfo);
    EXPECT_EQ("LinkDown", arg);
    EXPECT_TRUE(eventId.empty());
    EXPECT_EQ("Ethernet_4", impacted);
}

TEST_F(StateSetCoverageTest, ethIbPortLinkStateMatchingWrongBaseUnitCoverage)
{
    auto stateSensor = makeStateSensor(
        2, 0x1101, PLDM_ENTITY_ETHERNET, 5, PLDM_STATESET_ID_LINKSTATE,
        "/xyz/openbmc_project/inventory/system/network/eth5");
    std::string path = "/xyz/openbmc_project/state/coverage/eth_port_5";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/network/eth5");
    StateSetEthIBPortLinkState stateSet(PLDM_STATESET_ID_LINKSTATE, 0, path,
                                        association, 5);

    std::vector<std::shared_ptr<NumericSensor>> wrongUnitSensors{
        makeNumericSensor(2, 0x2205, PLDM_ENTITY_ETHERNET, 5,
                          PLDM_SENSOR_UNIT_WATTS, "eth_speed_watts")};
    stateSet.associateNumericSensor(stateSensor->getEntityInfo(),
                                    wrongUnitSensors);
    EXPECT_EQ(nullptr, stateSet.linkSpeedSensor);
}

TEST_F(StateSetCoverageTest, ethIbPortLinkStateNoBandwidthDeltaCoverage)
{
    auto stateSensor = makeStateSensor(
        2, 0x1102, PLDM_ENTITY_ETHERNET, 6, PLDM_STATESET_ID_LINKSTATE,
        "/xyz/openbmc_project/inventory/system/network/eth6");
    auto numericSensor =
        makeNumericSensor(2, 0x2206, PLDM_ENTITY_ETHERNET, 6,
                          PLDM_SENSOR_UNIT_BITS, "link_speed_zero_bits");
    numericSensor->updateReading(true, true, 0.0);

    std::string path = "/xyz/openbmc_project/state/coverage/eth_port_6";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/network/eth6");
    StateSetEthIBPortLinkState stateSet(PLDM_STATESET_ID_LINKSTATE, 0, path,
                                        association, 6);
    std::vector<std::shared_ptr<NumericSensor>> sensors{numericSensor};
    stateSet.associateNumericSensor(stateSensor->getEntityInfo(), sensors);
    ASSERT_NE(nullptr, stateSet.linkSpeedSensor);

    std::string switchType{
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet"};
    std::vector<std::string> switchProtocols{switchType};
    std::vector<pldm::dbus::PathAssociation> switchAssociations{association};
    auto switchBandwidthSensor =
        std::make_shared<oem_nvidia::SwitchBandwidthSensor>(
            2, "switch_bw_6", switchType, switchProtocols, switchAssociations);
    stateSet.associateDerivedSensor(switchBandwidthSensor);

    stateSet.setValue(PLDM_STATESET_LINK_STATE_CONNECTED);
    EXPECT_EQ(0.0, stateSet.ValuePortInfoIntf->currentSpeed());
}

TEST_F(StateSetCoverageTest, ethIbPortLinkStateDerivedSensorFalseCoverage)
{
    std::string path = "/xyz/openbmc_project/state/coverage/eth_port_7";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/network/eth7");
    StateSetEthIBPortLinkState stateSet(PLDM_STATESET_ID_LINKSTATE, 0, path,
                                        association, 7);

    EXPECT_FALSE(stateSet.isDerivedSensorAssociated());
}

TEST_F(StateSetCoverageTest, ethIbPortLinkStateLinkDownNullEventCoverage)
{
    std::string path = "/xyz/openbmc_project/state/coverage/eth_port_8";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/network/eth8");
    StateSetEthIBPortLinkState stateSet(PLDM_STATESET_ID_LINKSTATE, 0, path,
                                        association, 8);

    stateSet.setValue(PLDM_STATESET_LINK_STATE_DISCONNECTED);
    auto [message, arg, level, eventId,
          impacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("LinkDown", arg);
    EXPECT_TRUE(eventId.empty());
    EXPECT_TRUE(impacted.empty());
}

TEST_F(StateSetCoverageTest,
       ethIbPortLinkStateUpdateSensorNameNullIntfsCoverage)
{
    std::string path = "/xyz/openbmc_project/state/coverage/eth_port_9";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/network/eth9");
    StateSetEthIBPortLinkStateRenameCoverage stateSet(
        PLDM_STATESET_ID_LINKSTATE, 0, path, association, 9);

    stateSet.resetAssociationDefinitionsIntf();
    stateSet.resetPortIntf();
    stateSet.resetPortInfoIntf();
    stateSet.resetPortStateIntf();
    stateSet.updateSensorName("Ethernet Port 9");

    EXPECT_EQ("Ethernet Port 9", stateSet.objectName);
}

TEST_F(StateSetCoverageTest, pciePortLinkStateCoverage)
{
    auto stateSensor = makeStateSensor(
        3, 0x1200, PLDM_ENTITY_PCI_EXPRESS_BUS, 1, PLDM_STATESET_ID_LINKSTATE,
        "/xyz/openbmc_project/inventory/system/pcie/slot1");
    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/pcie/slot1"}, false);

    std::string path = "/xyz/openbmc_project/state/coverage/pcie_slot_1";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/pcie/slot1");
    StateSetPciePortLinkState stateSet(PLDM_STATESET_ID_LINKSTATE, 0, path,
                                       association, *stateSensor);

    stateSet.setValue(PLDM_STATESET_LINK_STATE_CONNECTED);
    auto [activeMessage, activeArg, activeLevel, activeEventId,
          activeImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("Active", activeArg);
    EXPECT_TRUE(activeEventId.empty());
    EXPECT_TRUE(activeImpacted.empty());

    stateSet.setValue(PLDM_STATESET_LINK_STATE_DISCONNECTED);
    auto [inactiveMessage, inactiveArg, inactiveLevel, inactiveEventId,
          inactiveImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("Inactive", inactiveArg);
    EXPECT_TRUE(inactiveEventId.empty());
    EXPECT_TRUE(inactiveImpacted.empty());

    stateSet.ValuePortStateIntf->linkState(PortLinkStates::Error);
    stateSet.ValuePortStateIntf->linkStatus(PortLinkStatus::NoLink);
    auto [errorMessage, errorArg, errorLevel, errorEventId,
          errorImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("Error", errorArg);
    EXPECT_TRUE(errorEventId.empty());
    EXPECT_TRUE(errorImpacted.empty());

    stateSet.setValue(0xFF);
    auto [unknownMessage, unknownArg, unknownLevel, unknownEventId,
          unknownImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("Unknown", unknownArg);
    EXPECT_TRUE(unknownEventId.empty());
    EXPECT_TRUE(unknownImpacted.empty());
    EXPECT_EQ("PCIe", stateSet.getStringStateType());
}

TEST_F(StateSetCoverageTest, performanceAndPowerSupplyCoverage)
{
    std::string perfPath = "/xyz/openbmc_project/state/coverage/performance_0";
    auto perfAssoc =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/processor/cpu0");
    auto perfSensor = makeStateSensor(
        4, 0x1300, PLDM_ENTITY_PROC, 1, PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu0");
    perfSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu0"}, false);
    StateSetPerformance performance(PLDM_STATESET_ID_PERFORMANCE, 0, perfPath,
                                    perfAssoc, *perfSensor);
    performance.setValue(PLDM_STATESET_PERFORMANCE_NORMAL);
    auto [perfNormalMessage, perfNormalArg, perfNormalLevel, perfNormalEventId,
          perfNormalImpacted] = performance.getEventData(nullptr);
    EXPECT_EQ("Normal", perfNormalArg);
    EXPECT_TRUE(perfNormalEventId.empty());
    EXPECT_TRUE(perfNormalImpacted.empty());
    performance.setValue(PLDM_STATESET_PERFORMANCE_THROTTLED);
    auto [perfWarnMessage, perfWarnArg, perfWarnLevel, perfWarnEventId,
          perfWarnImpacted] = performance.getEventData(nullptr);
    EXPECT_EQ("Throttled", perfWarnArg);
    EXPECT_TRUE(perfWarnEventId.empty());
    EXPECT_TRUE(perfWarnImpacted.empty());
    performance.setValue(0xFF);
    EXPECT_EQ("Performance", performance.getStringStateType());

    std::string psuPath =
        "/xyz/openbmc_project/state/coverage/power_supply_input_0";
    auto psuAssoc = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/powersupply/psu0");
    auto psuSensor = makeStateSensor(
        4, 0x1301, PLDM_ENTITY_POWER_SUPPLY, 1, PLDM_STATESET_ID_POWERSUPPLY,
        "/xyz/openbmc_project/inventory/system/powersupply/psu0");
    psuSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/powersupply/psu0"}, false);
    StateSetPowerSupplyInput powerSupply(PLDM_STATESET_ID_POWERSUPPLY, 1,
                                         psuPath, psuAssoc, *psuSensor);
    powerSupply.setValue(PLDM_STATESET_POWERSUPPLY_NORMAL);
    auto [psuNormalMessage, psuNormalArg, psuNormalLevel, psuNormalEventId,
          psuNormalImpacted] = powerSupply.getEventData(nullptr);
    EXPECT_EQ("Normal", psuNormalArg);
    EXPECT_TRUE(psuNormalEventId.empty());
    EXPECT_TRUE(psuNormalImpacted.empty());
    powerSupply.setValue(PLDM_STATESET_POWERSUPPLY_OUTOFRANGE);
    auto [psuWarnMessage, psuWarnArg, psuWarnLevel, psuWarnEventId,
          psuWarnImpacted] = powerSupply.getEventData(nullptr);
    EXPECT_EQ("Current Input out of Range", psuWarnArg);
    EXPECT_TRUE(psuWarnEventId.empty());
    EXPECT_TRUE(psuWarnImpacted.empty());
    powerSupply.setValue(0xFF);
    EXPECT_EQ("EDP Violation State", powerSupply.getStringStateType());
}

TEST_F(StateSetCoverageTest,
       performanceTelemetryPositiveAndDefaultValueCoverage)
{
    auto perfSensor = makeStateSensor(
        7, 0x7000, PLDM_ENTITY_SYS_BOARD, 1, PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/chassis/chassis70/cpu0");
    perfSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis70/cpu0"},
        false);

    std::string perfPath = "/xyz/openbmc_project/state/coverage/performance_7";
    auto perfAssoc = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis70/cpu0");
    StateSetPerformanceCoverage performance(PLDM_STATESET_ID_PERFORMANCE, 0,
                                            perfPath, perfAssoc, *perfSensor);

    performance.setValue(PLDM_STATESET_PERFORMANCE_THROTTLED);
    EXPECT_NO_THROW(performance.updateShmemReading("Value"));

    performance.setDefaultValue();

    auto [messageId, state, level, eventId,
          impacted] = performance.getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedWarning", messageId);
    EXPECT_EQ("Throttled", state);
    EXPECT_EQ(Level::Informational, level);
    EXPECT_TRUE(eventId.empty());
    EXPECT_TRUE(impacted.empty());
}

TEST_F(StateSetCoverageTest,
       powerSupplyTelemetryPositiveAndDefaultValueCoverage)
{
    auto psuSensor = makeStateSensor(
        8, 0x8000, PLDM_ENTITY_SYS_BOARD, 1, PLDM_STATESET_ID_POWERSUPPLY,
        "/xyz/openbmc_project/inventory/system/chassis/chassis80/psu0");
    psuSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/chassis/chassis80/psu0"},
        false);

    std::string psuPath = "/xyz/openbmc_project/state/coverage/power_supply_8";
    auto psuAssoc = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis80/psu0");
    StateSetPowerSupplyInputCoverage powerSupply(
        PLDM_STATESET_ID_POWERSUPPLY, 0, psuPath, psuAssoc, *psuSensor);

    powerSupply.setValue(PLDM_STATESET_POWERSUPPLY_OUTOFRANGE);
    EXPECT_NO_THROW(powerSupply.updateShmemReading("Status"));

    powerSupply.setDefaultValue();

    auto [messageId, state, level, eventId,
          impacted] = powerSupply.getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedWarning", messageId);
    EXPECT_EQ("Current Input out of Range", state);
    EXPECT_EQ(Level::Informational, level);
    EXPECT_TRUE(eventId.empty());
    EXPECT_TRUE(impacted.empty());
}

TEST_F(StateSetCoverageTest, pcieTelemetryErrorAndDefaultCoverage)
{
    auto pcieSensor = makeStateSensor(
        9, 0x9000, PLDM_ENTITY_PCI_EXPRESS_BUS, 1, PLDM_STATESET_ID_LINKSTATE,
        "/xyz/openbmc_project/inventory/system/pcie/slot13");
    pcieSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/pcie/slot13"}, false);

    std::string pciePath = "/xyz/openbmc_project/state/coverage/pcie_9";
    auto pcieAssoc =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/pcie/slot13");
    StateSetPciePortLinkStateCoverage pcieState(
        PLDM_STATESET_ID_LINKSTATE, 0, pciePath, pcieAssoc, *pcieSensor);

    pcieState.setValue(PLDM_STATESET_LINK_STATE_CONNECTED);
    EXPECT_NO_THROW(pcieState.updateShmemReading("LinkState"));

    pcieState.setDefaultValue();
    EXPECT_EQ(PortProtocol::PCIe, pcieState.ValuePortInfoIntf->protocol());
    EXPECT_EQ(PortLinkStates::Unknown,
              pcieState.ValuePortStateIntf->linkState());
    EXPECT_EQ(PortLinkStatus::NoLink,
              pcieState.ValuePortStateIntf->linkStatus());

    pcieState.ValuePortStateIntf->linkState(PortLinkStates::Error);
    pcieState.ValuePortStateIntf->linkStatus(PortLinkStatus::NoLink);
    auto [messageId, state, level, eventId,
          impacted] = pcieState.getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedCritical", messageId);
    EXPECT_EQ("Error", state);
    EXPECT_EQ(Level::Informational, level);
    EXPECT_TRUE(eventId.empty());
    EXPECT_TRUE(impacted.empty());
}

TEST_F(StateSetCoverageTest, telemetryGuardBranchCoverage)
{
    auto perfDefaultSensor = makeStateSensor(
        8, 0x1700, PLDM_ENTITY_PROC, 1, PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu8");
    std::string perfNonChassisPath =
        "/xyz/openbmc_project/state/coverage/performance_non_chassis";
    auto perfNonChassisAssoc =
        makeAssociation("memory", "all_states",
                        "/xyz/openbmc_project/inventory/system/processor/cpu8");
    StateSetPerformance perfNonChassis(PLDM_STATESET_ID_PERFORMANCE, 0,
                                       perfNonChassisPath, perfNonChassisAssoc,
                                       *perfDefaultSensor);
    perfNonChassis.setValue(PLDM_STATESET_PERFORMANCE_NORMAL);

    std::string perfDefaultPath =
        "/xyz/openbmc_project/state/coverage/performance_default_inventory";
    auto perfDefaultAssoc =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/processor/cpu8");
    StateSetPerformance perfDefault(PLDM_STATESET_ID_PERFORMANCE, 1,
                                    perfDefaultPath, perfDefaultAssoc,
                                    *perfDefaultSensor);
    perfDefault.setValue(PLDM_STATESET_PERFORMANCE_THROTTLED);

    auto psuDefaultSensor = makeStateSensor(
        8, 0x1701, PLDM_ENTITY_POWER_SUPPLY, 1, PLDM_STATESET_ID_POWERSUPPLY,
        "/xyz/openbmc_project/inventory/system/powersupply/psu8");
    std::string psuNonChassisPath =
        "/xyz/openbmc_project/state/coverage/power_supply_non_chassis";
    auto psuNonChassisAssoc = makeAssociation(
        "power", "all_states",
        "/xyz/openbmc_project/inventory/system/powersupply/psu8");
    StateSetPowerSupplyInput psuNonChassis(
        PLDM_STATESET_ID_POWERSUPPLY, 0, psuNonChassisPath, psuNonChassisAssoc,
        *psuDefaultSensor);
    psuNonChassis.setValue(PLDM_STATESET_POWERSUPPLY_NORMAL);

    std::string psuDefaultPath =
        "/xyz/openbmc_project/state/coverage/power_supply_default_inventory";
    auto psuDefaultAssoc = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/powersupply/psu8");
    StateSetPowerSupplyInput psuDefault(PLDM_STATESET_ID_POWERSUPPLY, 1,
                                        psuDefaultPath, psuDefaultAssoc,
                                        *psuDefaultSensor);
    psuDefault.setValue(PLDM_STATESET_POWERSUPPLY_OUTOFRANGE);

    auto pcieDefaultSensor = makeStateSensor(
        8, 0x1702, PLDM_ENTITY_PCI_EXPRESS_BUS, 1, PLDM_STATESET_ID_LINKSTATE,
        "/xyz/openbmc_project/inventory/system/pcie/slot8");
    std::string pcieNonChassisPath =
        "/xyz/openbmc_project/state/coverage/pcie_non_chassis";
    auto pcieNonChassisAssoc =
        makeAssociation("ports", "associated_port",
                        "/xyz/openbmc_project/inventory/system/pcie/slot8");
    StateSetPciePortLinkState pcieNonChassis(
        PLDM_STATESET_ID_LINKSTATE, 0, pcieNonChassisPath, pcieNonChassisAssoc,
        *pcieDefaultSensor);
    pcieNonChassis.setValue(PLDM_STATESET_LINK_STATE_CONNECTED);

    std::string pcieDefaultPath =
        "/xyz/openbmc_project/state/coverage/pcie_default_inventory";
    auto pcieDefaultAssoc =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/pcie/slot8");
    StateSetPciePortLinkState pcieDefault(
        PLDM_STATESET_ID_LINKSTATE, 1, pcieDefaultPath, pcieDefaultAssoc,
        *pcieDefaultSensor);
    pcieDefault.setValue(PLDM_STATESET_LINK_STATE_DISCONNECTED);
}

TEST_F(StateSetCoverageTest, performanceTelemetryWrongReverseCoverage)
{
    auto perfSensor = makeStateSensor(
        9, 0x1800, PLDM_ENTITY_PROC, 1, PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu9");
    perfSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu9"}, false);

    std::string perfPath =
        "/xyz/openbmc_project/state/coverage/performance_wrong_reverse";
    auto perfAssoc =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/processor/cpu9");
    StateSetPerformanceCoverage performance(PLDM_STATESET_ID_PERFORMANCE, 0,
                                            perfPath, perfAssoc, *perfSensor);
    performance.setAssociations({{"chassis", "not_all_states",
                                  "/xyz/openbmc_project/inventory/system/"
                                  "processor/cpu9"}});
    performance.updateShmemReading("Value");
    EXPECT_EQ("Performance", performance.getStringStateType());
}

TEST_F(StateSetCoverageTest, performanceTelemetryEmptyEndpointCoverage)
{
    auto perfSensor = makeStateSensor(
        9, 0x1801, PLDM_ENTITY_PROC, 2, PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu10");
    perfSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu10"}, false);

    std::string perfPath =
        "/xyz/openbmc_project/state/coverage/performance_empty_endpoint";
    auto perfAssoc = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/processor/cpu10");
    StateSetPerformanceCoverage performance(PLDM_STATESET_ID_PERFORMANCE, 0,
                                            perfPath, perfAssoc, *perfSensor);
    performance.setAssociations({{"chassis", "all_states", ""}});
    performance.updateShmemReading("Value");
    EXPECT_EQ("Performance", performance.getStringStateType());
}

TEST_F(StateSetCoverageTest, powerSupplyTelemetryWrongReverseCoverage)
{
    auto psuSensor = makeStateSensor(
        9, 0x1802, PLDM_ENTITY_POWER_SUPPLY, 1, PLDM_STATESET_ID_POWERSUPPLY,
        "/xyz/openbmc_project/inventory/system/powersupply/psu9");
    psuSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/powersupply/psu9"}, false);

    std::string psuPath =
        "/xyz/openbmc_project/state/coverage/powersupply_wrong_reverse";
    auto psuAssoc =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/powersupply/"
                        "psu9");
    StateSetPowerSupplyInputCoverage powerSupply(
        PLDM_STATESET_ID_POWERSUPPLY, 0, psuPath, psuAssoc, *psuSensor);
    powerSupply.setAssociations({{"chassis", "not_all_states",
                                  "/xyz/openbmc_project/inventory/system/"
                                  "powersupply/psu9"}});
    powerSupply.updateShmemReading("Status");
    EXPECT_EQ("EDP Violation State", powerSupply.getStringStateType());
}

TEST_F(StateSetCoverageTest, powerSupplyTelemetryEmptyEndpointCoverage)
{
    auto psuSensor = makeStateSensor(
        9, 0x1803, PLDM_ENTITY_POWER_SUPPLY, 2, PLDM_STATESET_ID_POWERSUPPLY,
        "/xyz/openbmc_project/inventory/system/powersupply/psu10");
    psuSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/powersupply/psu10"}, false);

    std::string psuPath =
        "/xyz/openbmc_project/state/coverage/powersupply_empty_endpoint";
    auto psuAssoc =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/powersupply/"
                        "psu10");
    StateSetPowerSupplyInputCoverage powerSupply(
        PLDM_STATESET_ID_POWERSUPPLY, 0, psuPath, psuAssoc, *psuSensor);
    powerSupply.setAssociations({{"chassis", "all_states", ""}});
    powerSupply.updateShmemReading("Status");
    EXPECT_EQ("EDP Violation State", powerSupply.getStringStateType());
}

TEST_F(StateSetCoverageTest, pcieTelemetryWrongReverseCoverage)
{
    auto pcieSensor = makeStateSensor(
        9, 0x1804, PLDM_ENTITY_PCI_EXPRESS_BUS, 1, PLDM_STATESET_ID_LINKSTATE,
        "/xyz/openbmc_project/inventory/system/pcie/slot9");
    pcieSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/pcie/slot9"}, false);

    std::string pciePath =
        "/xyz/openbmc_project/state/coverage/pcie_wrong_reverse";
    auto pcieAssoc =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/pcie/slot9");
    StateSetPciePortLinkStateCoverage pcieState(
        PLDM_STATESET_ID_LINKSTATE, 0, pciePath, pcieAssoc, *pcieSensor);
    pcieState.setAssociations({{"chassis", "not_all_states",
                                "/xyz/openbmc_project/inventory/system/"
                                "pcie/slot9"}});
    pcieState.updateShmemReading("LinkState");
    EXPECT_EQ("PCIe", pcieState.getStringStateType());
}

TEST_F(StateSetCoverageTest, pcieTelemetryEmptyEndpointCoverage)
{
    auto pcieSensor = makeStateSensor(
        9, 0x1805, PLDM_ENTITY_PCI_EXPRESS_BUS, 2, PLDM_STATESET_ID_LINKSTATE,
        "/xyz/openbmc_project/inventory/system/pcie/slot10");
    pcieSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/pcie/slot10"}, false);

    std::string pciePath =
        "/xyz/openbmc_project/state/coverage/pcie_empty_endpoint";
    auto pcieAssoc =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/pcie/slot10");
    StateSetPciePortLinkStateCoverage pcieState(
        PLDM_STATESET_ID_LINKSTATE, 0, pciePath, pcieAssoc, *pcieSensor);
    pcieState.setAssociations({{"chassis", "all_states", ""}});
    pcieState.updateShmemReading("LinkState");
    EXPECT_EQ("PCIe", pcieState.getStringStateType());
}

TEST_F(StateSetCoverageTest, performanceTelemetryWrongForwardCoverage)
{
    auto perfSensor = makeStateSensor(
        10, 0x1806, PLDM_ENTITY_PROC, 1, PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu10");
    perfSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu10"}, false);

    std::string perfPath =
        "/xyz/openbmc_project/state/coverage/performance_wrong_forward";
    auto perfAssoc = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/processor/cpu10");
    StateSetPerformanceCoverage performance(PLDM_STATESET_ID_PERFORMANCE, 0,
                                            perfPath, perfAssoc, *perfSensor);
    performance.updateShmemReading("Value");
    EXPECT_EQ("Performance", performance.getStringStateType());
}

TEST_F(StateSetCoverageTest, powerSupplyTelemetryWrongForwardCoverage)
{
    auto psuSensor = makeStateSensor(
        10, 0x1807, PLDM_ENTITY_POWER_SUPPLY, 1, PLDM_STATESET_ID_POWERSUPPLY,
        "/xyz/openbmc_project/inventory/system/powersupply/psu10");
    psuSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/powersupply/psu10"}, false);

    std::string psuPath =
        "/xyz/openbmc_project/state/coverage/power_supply_wrong_forward";
    auto psuAssoc = makeAssociation(
        "power", "all_states",
        "/xyz/openbmc_project/inventory/system/powersupply/psu10");
    StateSetPowerSupplyInputCoverage powerSupply(
        PLDM_STATESET_ID_POWERSUPPLY, 0, psuPath, psuAssoc, *psuSensor);
    powerSupply.updateShmemReading("Status");
    EXPECT_EQ("EDP Violation State", powerSupply.getStringStateType());
}

TEST_F(StateSetCoverageTest, pcieTelemetryWrongForwardCoverage)
{
    auto pcieSensor = makeStateSensor(
        10, 0x1808, PLDM_ENTITY_PCI_EXPRESS_BUS, 1, PLDM_STATESET_ID_LINKSTATE,
        "/xyz/openbmc_project/inventory/system/pcie/slot11");
    pcieSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/pcie/slot11"}, false);

    std::string pciePath =
        "/xyz/openbmc_project/state/coverage/pcie_wrong_forward";
    auto pcieAssoc =
        makeAssociation("ports", "all_states",
                        "/xyz/openbmc_project/inventory/system/pcie/slot11");
    StateSetPciePortLinkStateCoverage pcieState(
        PLDM_STATESET_ID_LINKSTATE, 0, pciePath, pcieAssoc, *pcieSensor);
    pcieState.updateShmemReading("LinkState");
    EXPECT_EQ("PCIe", pcieState.getStringStateType());
}

TEST_F(StateSetCoverageTest,
       performanceTelemetryDefaultInventoryEndpointCoverage)
{
    auto perfDefaultSensor = makeStateSensor(
        10, 0x1809, PLDM_ENTITY_PROC, 2, PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu11");

    std::string perfPath =
        "/xyz/openbmc_project/state/coverage/performance_default_endpoint";
    auto perfAssoc = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/processor/cpu11");
    StateSetPerformanceCoverage performance(
        PLDM_STATESET_ID_PERFORMANCE, 0, perfPath, perfAssoc,
        *perfDefaultSensor);
    performance.updateShmemReading("Value");
    EXPECT_EQ("Performance", performance.getStringStateType());
}

TEST_F(StateSetCoverageTest,
       powerSupplyTelemetryDefaultInventoryEndpointCoverage)
{
    auto psuDefaultSensor = makeStateSensor(
        10, 0x180A, PLDM_ENTITY_POWER_SUPPLY, 2, PLDM_STATESET_ID_POWERSUPPLY,
        "/xyz/openbmc_project/inventory/system/powersupply/psu11");

    std::string psuPath =
        "/xyz/openbmc_project/state/coverage/power_supply_default_endpoint";
    auto psuAssoc = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/powersupply/psu11");
    StateSetPowerSupplyInputCoverage powerSupply(
        PLDM_STATESET_ID_POWERSUPPLY, 0, psuPath, psuAssoc, *psuDefaultSensor);
    powerSupply.updateShmemReading("Status");
    EXPECT_EQ("EDP Violation State", powerSupply.getStringStateType());
}

TEST_F(StateSetCoverageTest, pcieTelemetryDefaultInventoryEndpointCoverage)
{
    auto pcieDefaultSensor = makeStateSensor(
        10, 0x180B, PLDM_ENTITY_PCI_EXPRESS_BUS, 2, PLDM_STATESET_ID_LINKSTATE,
        "/xyz/openbmc_project/inventory/system/pcie/slot12");

    std::string pciePath =
        "/xyz/openbmc_project/state/coverage/pcie_default_endpoint";
    auto pcieAssoc =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/pcie/slot12");
    StateSetPciePortLinkStateCoverage pcieState(
        PLDM_STATESET_ID_LINKSTATE, 0, pciePath, pcieAssoc, *pcieDefaultSensor);
    pcieState.updateShmemReading("LinkState");
    EXPECT_EQ("PCIe", pcieState.getStringStateType());
}

TEST_F(StateSetCoverageTest, healthStateRenameSanitizeCoverage)
{
    std::string path = "/xyz/openbmc_project/state/coverage/health/Health_1";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/board/board10");
    StateSetHealthState healthState(PLDM_STATESET_ID_HEALTHSTATE, 0, path,
                                    association);

    healthState.setValue(PLDM_STATESET_HEALTH_STATE_NORMAL);
    healthState.updateSensorName("Health+Critical/Board#10");
    auto [message, arg, level, eventId,
          impacted] = healthState.getEventData(nullptr);
    EXPECT_EQ("OK", arg);
    EXPECT_TRUE(eventId.empty());
    EXPECT_TRUE(impacted.empty());
}

TEST_F(StateSetCoverageTest, oemStateSetCoverage)
{
    auto memorySensor = makeStateSensor(
        5, 0x1400, PLDM_ENTITY_MEMORY_CONTROLLER, 1, PLDM_STATESET_ID_PRESENCE,
        "/xyz/openbmc_project/inventory/system/memory/memory1");
    memorySensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/memory/memory1"}, false);
    std::string memoryPath =
        "/xyz/openbmc_project/state/coverage/memory_spare_channel_1";
    auto memoryAssoc =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/memory/memory1");
    StateSetMemorySpareChannel memorySpare(
        PLDM_STATESET_ID_PRESENCE, 0, memoryPath, memoryAssoc, *memorySensor);
    memorySpare.setValue(PLDM_STATESET_PRESENCE_PRESENT);
    auto [presentMessage, presentArg, presentLevel, presentEventId,
          presentImpacted] = memorySpare.getEventData(nullptr);
    EXPECT_EQ("True", presentArg);
    EXPECT_TRUE(presentEventId.empty());
    EXPECT_TRUE(presentImpacted.empty());
    memorySpare.setValue(PLDM_STATESET_PRESENCE_NOT_PRESENT);
    auto [absentMessage, absentArg, absentLevel, absentEventId,
          absentImpacted] = memorySpare.getEventData(nullptr);
    EXPECT_EQ("False", absentArg);
    EXPECT_TRUE(absentEventId.empty());
    EXPECT_TRUE(absentImpacted.empty());
    memorySpare.setValue(0xFF);
    auto [unknownMessage, unknownArg, unknownLevel, unknownEventId,
          unknownImpacted] = memorySpare.getEventData(nullptr);
    EXPECT_EQ("Unknown", unknownArg);
    EXPECT_TRUE(unknownEventId.empty());
    EXPECT_TRUE(unknownImpacted.empty());

    auto powerBreakSensor =
        makeStateSensor(5, STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC, 2,
                        PLDM_STATESET_ID_PERFORMANCE,
                        "/xyz/openbmc_project/inventory/system/processor/cpu2");
    powerBreakSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu2"}, false);
    std::string powerBreakPath =
        "/xyz/openbmc_project/state/coverage/power_break_2";
    auto powerBreakAssoc =
        makeAssociation("memory", "all_states",
                        "/xyz/openbmc_project/inventory/system/processor/cpu2");
    StateSetProcessorPowerBreak powerBreak(
        PLDM_STATESET_ID_PERFORMANCE, 0, powerBreakPath, powerBreakAssoc,
        *powerBreakSensor);
    powerBreak.setValue(PLDM_STATESET_PERFORMANCE_NORMAL);
    auto [powerBreakOkMessage, powerBreakOkArg, powerBreakOkLevel,
          powerBreakOkEventId,
          powerBreakOkImpacted] = powerBreak.getEventData(nullptr);
    EXPECT_EQ("Normal", powerBreakOkArg);
    EXPECT_TRUE(powerBreakOkEventId.empty());
    EXPECT_TRUE(powerBreakOkImpacted.empty());
    powerBreak.setValue(PLDM_STATESET_PERFORMANCE_THROTTLED);
    auto [powerBreakWarnMessage, powerBreakWarnArg, powerBreakWarnLevel,
          powerBreakWarnEventId,
          powerBreakWarnImpacted] = powerBreak.getEventData(nullptr);
    EXPECT_EQ("Throttled", powerBreakWarnArg);
    EXPECT_TRUE(powerBreakWarnEventId.empty());
    EXPECT_TRUE(powerBreakWarnImpacted.empty());
    powerBreak.setValue(0xFF);

    std::string memoryPerfPath =
        "/xyz/openbmc_project/state/coverage/memory_performance_2";
    auto memoryPerfAssoc =
        makeAssociation("memory", "all_states",
                        "/xyz/openbmc_project/inventory/system/memory/memory2");
    StateSetMemoryPerformance memoryPerformance(
        PLDM_STATESET_ID_PERFORMANCE, 0, memoryPerfPath, memoryPerfAssoc);
    memoryPerformance.setValue(PLDM_STATESET_PERFORMANCE_NORMAL);
    auto [memoryPerfOkMessage, memoryPerfOkArg, memoryPerfOkLevel,
          memoryPerfOkEventId,
          memoryPerfOkImpacted] = memoryPerformance.getEventData(nullptr);
    EXPECT_EQ("Normal", memoryPerfOkArg);
    EXPECT_TRUE(memoryPerfOkEventId.empty());
    EXPECT_TRUE(memoryPerfOkImpacted.empty());
    memoryPerformance.setValue(PLDM_STATESET_PERFORMANCE_THROTTLED);
    auto [memoryPerfWarnMessage, memoryPerfWarnArg, memoryPerfWarnLevel,
          memoryPerfWarnEventId,
          memoryPerfWarnImpacted] = memoryPerformance.getEventData(nullptr);
    EXPECT_EQ("PerformanceDegraded due to high temperature", memoryPerfWarnArg);
    EXPECT_TRUE(memoryPerfWarnEventId.empty());
    EXPECT_TRUE(memoryPerfWarnImpacted.empty());
    memoryPerformance.setValue(0xFF);
}

TEST_F(StateSetCoverageTest, nvlinkCoverage)
{
    auto stateSensor = makeStateSensor(
        6, 0x1500, PLDM_ENTITY_SYS_BUS, 5, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric5");
    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric5"}, false);

    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_5";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/processor/cpu5");
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *stateSensor);

    stateSet.setValue(PLDM_STATE_SET_NVLINK_ACTIVE);
    auto [activeMessage, activeArg, activeLevel, activeEventId,
          activeImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("LinkUp", activeArg);
    EXPECT_TRUE(activeEventId.empty());
    EXPECT_TRUE(activeImpacted.empty());

    stateSet.setValue(PLDM_STATE_SET_NVLINK_INACTIVE);
    pldm::utils::SensorEventInfo eventInfo{
        "NVLink_5", {{"LinkDown", "OpenBMC.0.1.NvlinkDown"}}};
    auto [inactiveMessage, inactiveArg, inactiveLevel, inactiveEventId,
          inactiveImpacted] = stateSet.getEventData(&eventInfo);
    EXPECT_EQ("LinkDown", inactiveArg);
    EXPECT_EQ("OpenBMC.0.1.NvlinkDown", inactiveEventId);
    EXPECT_EQ("NVLink_5", inactiveImpacted);

    stateSet.setValue(PLDM_STATE_SET_NVLINK_ERROR);
    auto [errorMessage, errorArg, errorLevel, errorEventId,
          errorImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("Error", errorArg);
    EXPECT_TRUE(errorEventId.empty());
    EXPECT_TRUE(errorImpacted.empty());

    stateSet.setValue(0xFF);
    auto [unknownMessage, unknownArg, unknownLevel, unknownEventId,
          unknownImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("Unknown", unknownArg);
    EXPECT_TRUE(unknownEventId.empty());
    EXPECT_TRUE(unknownImpacted.empty());
    EXPECT_EQ("NVLink", stateSet.getStringStateType());

    auto originalAssocs = stateSet.getAssociations();
    std::vector<pldm::dbus::PathAssociation> emptyAssociations;
    stateSet.setAssociation(emptyAssociations);
    EXPECT_EQ(originalAssocs, stateSet.getAssociations());

    std::vector<pldm::dbus::PathAssociation> blankPathAssociations{
        makeAssociation("chassis", "all_states", "")};
    stateSet.setAssociation(blankPathAssociations);
    EXPECT_EQ(originalAssocs, stateSet.getAssociations());

    std::vector<pldm::dbus::PathAssociation> associations{
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/processor/"
                        "cpu5")};
    stateSet.setAssociation(associations);
    ASSERT_EQ(1u, stateSet.getAssociations().size());
    EXPECT_EQ("/xyz/openbmc_project/inventory/system/processor/cpu5",
              std::get<2>(stateSet.getAssociations().front()));
}

TEST_F(StateSetCoverageTest, nvlinkEventFallbackAndNullAssociationCoverage)
{
    auto stateSensor = makeStateSensor(
        7, 0x1600, PLDM_ENTITY_SYS_BUS, 6, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric6");
    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric6"}, false);

    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_6";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/processor/cpu6");
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *stateSensor);

    stateSet.setValue(PLDM_STATE_SET_NVLINK_INACTIVE);
    pldm::utils::SensorEventInfo eventInfo{"NVLink_6", {}};
    auto [message, arg, level, eventId,
          impacted] = stateSet.getEventData(&eventInfo);
    EXPECT_EQ("LinkDown", arg);
    EXPECT_TRUE(eventId.empty());
    EXPECT_EQ("NVLink_6", impacted);

    stateSet.resetAssociationDefinitionsIntf();
    std::vector<pldm::dbus::PathAssociation> associations{
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/processor/"
                        "cpu6")};
    stateSet.setAssociation(associations);
}

TEST_F(StateSetCoverageTest, nvlinkManualEventAndTelemetryGuardCoverage)
{
    auto stateSensor = makeStateSensor(
        8, 0x1601, PLDM_ENTITY_SYS_BUS, 7, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric7");
    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric7"}, false);

    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_7";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/processor/cpu7");
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *stateSensor);

    stateSet.ValuePortStateIntf->linkStatus(PortLinkStatus::LinkDown);
    stateSet.ValuePortStateIntf->linkState(PortLinkStates::Disabled);
    auto [warningMessage, warningArg, warningLevel, warningEventId,
          warningImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedWarning", warningMessage);
    EXPECT_EQ("LinkDown", warningArg);
    EXPECT_EQ(Level::Informational, warningLevel);
    EXPECT_TRUE(warningEventId.empty());
    EXPECT_TRUE(warningImpacted.empty());

    stateSet.ValuePortStateIntf->linkStatus(PortLinkStatus::NoLink);
    stateSet.ValuePortStateIntf->linkState(PortLinkStates::Error);
    auto [criticalMessage, criticalArg, criticalLevel, criticalEventId,
          criticalImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("ResourceEvent.1.0.ResourceStatusChangedCritical",
              criticalMessage);
    EXPECT_EQ("Error", criticalArg);
    EXPECT_EQ(Level::Informational, criticalLevel);
    EXPECT_TRUE(criticalEventId.empty());
    EXPECT_TRUE(criticalImpacted.empty());

    stateSet.setAssociations({{"chassis", "all_states",
                               "/xyz/openbmc_project/inventory/system/"
                               "processor/cpu7"}});
    stateSet.updateShmemReading("LinkState");
    stateSet.updateShmemReading("LinkStatus");

    stateSet.setAssociations({{"chassis", "all_states", ""}});
    stateSet.updateShmemReading("LinkState");

    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric7"}, true);
    stateSet.setAssociations({{"chassis", "all_states",
                               "/xyz/openbmc_project/inventory/system/"
                               "processor/cpu7"}});
    stateSet.updateShmemReading("LinkStatus");

    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric7"}, false);
    stateSet.setAssociations({{"processor", "all_states",
                               "/xyz/openbmc_project/inventory/system/"
                               "processor/cpu7"}});
    stateSet.updateShmemReading("LinkState");
}

TEST_F(StateSetCoverageTest, memoryPerformanceTelemetryGuardCoverage)
{
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_performance_telemetry";
    auto association =
        makeAssociation("memory", "all_states",
                        "/xyz/openbmc_project/inventory/system/memory/memory8");
    StateSetMemoryPerformanceCoverage stateSet(PLDM_STATESET_ID_PERFORMANCE, 0,
                                               path, association);

    stateSet.setAssociations(
        {{"memory", "all_states",
          "/xyz/openbmc_project/inventory/system/memory/memory8"}});
    stateSet.setValue(PLDM_STATESET_PERFORMANCE_NORMAL);

    stateSet.setAssociations({{"memory", "all_states", ""}});
    stateSet.setValue(PLDM_STATESET_PERFORMANCE_THROTTLED);

    stateSet.setAssociations({{"chassis", "all_states",
                               "/xyz/openbmc_project/inventory/system/"
                               "memory/memory8"}});
    stateSet.setValue(0xFF);

    stateSet.setAssociations({{"memory", "reverse_only",
                               "/xyz/openbmc_project/inventory/system/"
                               "memory/memory8"}});
    stateSet.setValue(PLDM_STATESET_PERFORMANCE_NORMAL);
}

TEST_F(StateSetCoverageTest, memorySpareChannelTelemetryGuardCoverage)
{
    auto stateSensor = makeStateSensor(
        9, 0x1700, PLDM_ENTITY_PROC, 8, PLDM_STATESET_ID_PRESENCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu8");
    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu8"}, false);

    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_spare_channel_telemetry";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/processor/cpu8");
    StateSetMemorySpareChannelCoverage stateSet(
        PLDM_STATESET_ID_PRESENCE, 0, path, association, *stateSensor);

    stateSet.setAssociations({{"chassis", "all_states",
                               "/xyz/openbmc_project/inventory/system/"
                               "processor/cpu8"}});
    stateSet.setValue(PLDM_STATESET_PRESENCE_PRESENT);

    stateSet.setAssociations({{"chassis", "all_states", ""}});
    stateSet.setValue(PLDM_STATESET_PRESENCE_NOT_PRESENT);

    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu8"}, true);
    stateSet.setAssociations({{"chassis", "all_states",
                               "/xyz/openbmc_project/inventory/system/"
                               "processor/cpu8"}});
    stateSet.setValue(PLDM_STATESET_PRESENCE_PRESENT);

    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu8"}, false);
    stateSet.setAssociations({{"processor", "all_states",
                               "/xyz/openbmc_project/inventory/system/"
                               "processor/cpu8"}});
    stateSet.setValue(0xFF);

    stateSet.setAssociations({{"chassis", "reverse_only",
                               "/xyz/openbmc_project/inventory/system/"
                               "processor/cpu8"}});
    stateSet.setValue(PLDM_STATESET_PRESENCE_PRESENT);
}

TEST_F(StateSetCoverageTest, processorPowerBreakTelemetryGuardCoverage)
{
    auto stateSensor =
        makeStateSensor(10, STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC, 9,
                        PLDM_STATESET_ID_PERFORMANCE,
                        "/xyz/openbmc_project/inventory/system/processor/cpu9");
    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu9"}, false);

    std::string path =
        "/xyz/openbmc_project/state/coverage/processor_power_break_telemetry";
    auto association =
        makeAssociation("memory", "all_states",
                        "/xyz/openbmc_project/inventory/system/memory/memory9");
    StateSetProcessorPowerBreakCoverage stateSet(
        PLDM_STATESET_ID_PERFORMANCE, 0, path, association, *stateSensor);

    stateSet.setAssociations(
        {{"memory", "all_states",
          "/xyz/openbmc_project/inventory/system/memory/memory9"}});
    stateSet.setValue(PLDM_STATESET_PERFORMANCE_NORMAL);

    stateSet.setAssociations({{"memory", "all_states", ""}});
    stateSet.setValue(PLDM_STATESET_PERFORMANCE_THROTTLED);

    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu9"}, true);
    stateSet.setAssociations(
        {{"memory", "all_states",
          "/xyz/openbmc_project/inventory/system/memory/memory9"}});
    stateSet.setValue(PLDM_STATESET_PERFORMANCE_NORMAL);

    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu9"}, false);
    stateSet.setAssociations({{"processor", "all_states",
                               "/xyz/openbmc_project/inventory/system/"
                               "memory/memory9"}});
    stateSet.setValue(0xFF);

    stateSet.setAssociations({{"memory", "reverse_only",
                               "/xyz/openbmc_project/inventory/system/"
                               "memory/memory9"}});
    stateSet.setValue(PLDM_STATESET_PERFORMANCE_NORMAL);
}

TEST_F(StateSetCoverageTest,
       memoryPerformanceTelemetryScansMixedAssociationsBeforeMatch)
{
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_performance_scan";
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory10");
    StateSetMemoryPerformanceCoverage stateSet(PLDM_STATESET_ID_PERFORMANCE, 0,
                                               path, association);

    stateSet.setAssociations(
        {{"memory", "reverse_only",
          "/xyz/openbmc_project/inventory/system/memory/memory10"},
         {"processor", "all_states",
          "/xyz/openbmc_project/inventory/system/processor/cpu10"},
         {"memory", "all_states",
          "/xyz/openbmc_project/inventory/system/memory/memory10"}});

    EXPECT_NO_THROW(stateSet.setValue(PLDM_STATESET_PERFORMANCE_NORMAL));
}

TEST_F(StateSetCoverageTest,
       memorySpareChannelTelemetryScansMixedAssociationsBeforeMatch)
{
    auto stateSensor = makeStateSensor(
        12, 0x1710, PLDM_ENTITY_PROC, 10, PLDM_STATESET_ID_PRESENCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu10");
    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu10"}, false);

    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_spare_channel_scan";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/processor/cpu10");
    StateSetMemorySpareChannelCoverage stateSet(
        PLDM_STATESET_ID_PRESENCE, 0, path, association, *stateSensor);

    stateSet.setAssociations(
        {{"chassis", "reverse_only",
          "/xyz/openbmc_project/inventory/system/processor/cpu10"},
         {"processor", "all_states",
          "/xyz/openbmc_project/inventory/system/processor/cpu10"},
         {"chassis", "all_states",
          "/xyz/openbmc_project/inventory/system/processor/cpu10"}});

    EXPECT_NO_THROW(stateSet.setValue(PLDM_STATESET_PRESENCE_PRESENT));
}

TEST_F(StateSetCoverageTest,
       processorPowerBreakTelemetryScansMixedAssociationsBeforeMatch)
{
    auto stateSensor = makeStateSensor(
        13, STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC, 11,
        PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu11");
    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu11"}, false);

    std::string path =
        "/xyz/openbmc_project/state/coverage/processor_power_break_scan";
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory11");
    StateSetProcessorPowerBreakCoverage stateSet(
        PLDM_STATESET_ID_PERFORMANCE, 0, path, association, *stateSensor);

    stateSet.setAssociations(
        {{"memory", "reverse_only",
          "/xyz/openbmc_project/inventory/system/memory/memory11"},
         {"processor", "all_states",
          "/xyz/openbmc_project/inventory/system/processor/cpu11"},
         {"memory", "all_states",
          "/xyz/openbmc_project/inventory/system/memory/memory11"}});

    EXPECT_NO_THROW(stateSet.setValue(PLDM_STATESET_PERFORMANCE_NORMAL));
}

TEST_F(StateSetCoverageTest, nvlinkSetAssociationMatrixCoverage)
{
    auto stateSensor = makeStateSensor(
        11, 0x1804, PLDM_ENTITY_SYS_BUS, 10, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric10");
    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric10"}, false);

    std::string instancePath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis910";
    ASSERT_NO_THROW(ensureMapperServiceRequested(bus));
    NvlinkObjectMapper mapper(bus, pldm::utils::mapperPath,
                              pldm::utils::mapperService, instancePath);
    sdbusplus::server::xyz::openbmc_project::inventory::item::Chassis chassis(
        bus, instancePath.c_str());
    oem_nvidia::InstanceIntf parentInstance(bus, instancePath.c_str());
    parentInstance.instanceNumber(10);
    while (bus.process_discard() > 0)
    {}

    auto validAssociation = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis910/CPU_0");

    std::string basePath = "/xyz/openbmc_project/state/coverage/nvlink_assoc";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, basePath,
                                    validAssociation, *stateSensor);

    std::string ctorReversePath =
        "/xyz/openbmc_project/state/coverage/nvlink_ctor_reverse";
    auto ctorReverseAssociation = makeAssociation(
        "chassis", "reverse_only",
        "/xyz/openbmc_project/inventory/system/chassis/chassis910/CPU_0");
    StateSetNvlinkCoverage ctorReverseState(
        PLDM_NVIDIA_OEM_STATE_SET_NVLINK, ctorReversePath,
        ctorReverseAssociation, *stateSensor);

    std::string ctorEmptyPath =
        "/xyz/openbmc_project/state/coverage/nvlink_ctor_empty";
    auto ctorEmptyAssociation = makeAssociation("chassis", "all_states", "");
    StateSetNvlinkCoverage ctorEmptyState(
        PLDM_NVIDIA_OEM_STATE_SET_NVLINK, ctorEmptyPath, ctorEmptyAssociation,
        *stateSensor);

    std::vector<pldm::dbus::PathAssociation> emptyAssociations;
    stateSet.setAssociation(emptyAssociations);

    std::string noDefPath =
        "/xyz/openbmc_project/state/coverage/nvlink_assoc_nodef";
    StateSetNvlinkCoverage noDefinitionsState(
        PLDM_NVIDIA_OEM_STATE_SET_NVLINK, noDefPath, validAssociation,
        *stateSensor);
    noDefinitionsState.resetAssociationDefinitionsIntf();
    std::vector<pldm::dbus::PathAssociation> validAssociations{
        makeAssociation("chassis", "all_states", instancePath),
        validAssociation};
    noDefinitionsState.setAssociation(validAssociations);

    stateSet.setAssociations({{"chassis", "reverse_only",
                               "/xyz/openbmc_project/inventory/system/"
                               "chassis/chassis910/CPU_0"}});
    stateSet.updateShmemReading("UnexpectedProperty");

    std::vector<pldm::dbus::PathAssociation> malformedAssociations{
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/"
                        "invalid path/cpu10")};
    EXPECT_NO_THROW(stateSet.setAssociation(malformedAssociations));
    EXPECT_EQ(nullptr, stateSet.endpointIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointInstanceIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointAssociationDefinitionsIntf.get());

    EXPECT_NO_THROW(stateSet.setAssociation(validAssociations));
    auto* endpointIntf = stateSet.endpointIntf.get();
    auto* endpointInstanceIntf = stateSet.endpointInstanceIntf.get();
    auto* endpointAssociationDefinitionsIntf =
        stateSet.endpointAssociationDefinitionsIntf.get();
    EXPECT_NO_THROW(stateSet.setAssociation(validAssociations));
    EXPECT_EQ(endpointIntf, stateSet.endpointIntf.get());
    EXPECT_EQ(endpointInstanceIntf, stateSet.endpointInstanceIntf.get());
    EXPECT_EQ(endpointAssociationDefinitionsIntf,
              stateSet.endpointAssociationDefinitionsIntf.get());

    std::vector<pldm::dbus::PathAssociation> chassisAssociations{
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/chassis/"
                        "chassis10/slot0")};
    EXPECT_NO_THROW(stateSet.setAssociation(chassisAssociations));
}

TEST_F(StateSetCoverageTest,
       nvlinkSetAssociationWithoutChassisMapperUsesOriginalPathCoverage)
{
    auto stateSensor = makeStateSensor(
        14, 0x1805, PLDM_ENTITY_SYS_BUS, 12, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric12");
    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric12"}, false);

    ASSERT_NO_THROW(ensureMapperServiceRequested(bus));
    NvlinkObjectMapper mapper(
        bus, pldm::utils::mapperPath, pldm::utils::mapperService,
        "/xyz/openbmc_project/inventory/system/chassis/"
        "unrelated12");

    std::string parentPath =
        "/xyz/openbmc_project/inventory/system/processor_group12";
    oem_nvidia::InstanceIntf parentInstance(bus, parentPath.c_str());
    parentInstance.instanceNumber(12);
    while (bus.process_discard() > 0)
    {}

    auto association =
        makeAssociation("chassis", "all_states", parentPath + "/CPU_1");
    std::string path =
        "/xyz/openbmc_project/state/coverage/nvlink_assoc_no_chassis_match";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *stateSensor);

    std::vector<pldm::dbus::PathAssociation> associations{association};
    EXPECT_NO_THROW(stateSet.setAssociation(associations));
    ASSERT_EQ(1u, stateSet.getAssociations().size());
    EXPECT_EQ(association.path,
              std::get<2>(stateSet.getAssociations().front()));
    EXPECT_EQ(nullptr, stateSet.endpointIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointInstanceIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointAssociationDefinitionsIntf.get());
}

TEST_F(StateSetCoverageTest,
       nvlinkSetAssociationMissingInstancePropertyCoverage)
{
    auto stateSensor = makeStateSensor(
        15, 0x1806, PLDM_ENTITY_SYS_BUS, 13, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric13");
    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric13"}, false);

    ASSERT_NO_THROW(ensureMapperServiceRequested(bus));
    NvlinkObjectMapper mapper(
        bus, pldm::utils::mapperPath, pldm::utils::mapperService,
        "/xyz/openbmc_project/inventory/system/chassis/"
        "unrelated13");

    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/processor_group13/CPU_2");
    std::string path =
        "/xyz/openbmc_project/state/coverage/nvlink_assoc_missing_instance";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *stateSensor);

    std::vector<pldm::dbus::PathAssociation> associations{association};
    EXPECT_NO_THROW(stateSet.setAssociation(associations));
    EXPECT_EQ(nullptr, stateSet.endpointIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointInstanceIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointAssociationDefinitionsIntf.get());
}

TEST_F(StateSetCoverageTest, nvlinkSetAssociationEndpointCollisionCoverage)
{
    auto stateSensor = makeStateSensor(
        16, 0x1807, PLDM_ENTITY_SYS_BUS, 14, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric14");
    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric14"}, false);

    std::string instancePath =
        "/xyz/openbmc_project/inventory/system/chassis/chassis914";
    ASSERT_NO_THROW(ensureMapperServiceRequested(bus));
    NvlinkObjectMapper mapper(bus, pldm::utils::mapperPath,
                              pldm::utils::mapperService, instancePath);
    sdbusplus::server::xyz::openbmc_project::inventory::item::Chassis chassis(
        bus, instancePath.c_str());
    oem_nvidia::InstanceIntf parentInstance(bus, instancePath.c_str());
    parentInstance.instanceNumber(14);

    std::string endpointPath = "/xyz/openbmc_project/inventory/system/fabrics/"
                               "C2CLinkFabric_14/Endpoints/CPU_0";
    oem_nvidia::EndpointIntf conflictingEndpoint(bus, endpointPath.c_str());
    while (bus.process_discard() > 0)
    {}

    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis914/CPU_0");
    std::string path =
        "/xyz/openbmc_project/state/coverage/nvlink_assoc_collision";
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *stateSensor);

    std::vector<pldm::dbus::PathAssociation> associations{
        makeAssociation("chassis", "all_states", instancePath), association};
    EXPECT_NO_THROW(stateSet.setAssociation(associations));
    EXPECT_EQ(nullptr, stateSet.endpointIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointInstanceIntf.get());
    EXPECT_EQ(nullptr, stateSet.endpointAssociationDefinitionsIntf.get());
}

TEST_F(StateSetCoverageTest,
       memoryPerformanceTelemetryIgnoresAllNonMatchingAssociationsCoverage)
{
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_performance_all_miss";
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory12");
    StateSetMemoryPerformanceCoverage stateSet(PLDM_STATESET_ID_PERFORMANCE, 0,
                                               path, association);

    stateSet.setAssociations(
        {{"processor", "all_states",
          "/xyz/openbmc_project/inventory/system/processor/cpu12"},
         {"memory", "reverse_only",
          "/xyz/openbmc_project/inventory/system/memory/memory12"}});
    EXPECT_NO_THROW(stateSet.setValue(PLDM_STATESET_PERFORMANCE_THROTTLED));
}

TEST_F(StateSetCoverageTest,
       memoryPerformanceTelemetryIgnoresEmptyAssociationsCoverage)
{
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_performance_empty_assocs";
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory12_empty");
    StateSetMemoryPerformanceCoverage stateSet(PLDM_STATESET_ID_PERFORMANCE, 0,
                                               path, association);

    stateSet.setAssociations({});
    EXPECT_NO_THROW(stateSet.updateShmemReading("Value"));
    EXPECT_NO_THROW(stateSet.setValue(PLDM_STATESET_PERFORMANCE_NORMAL));
}

TEST_F(StateSetCoverageTest,
       memorySpareChannelTelemetryIgnoresAllNonMatchingAssociationsCoverage)
{
    auto stateSensor = makeStateSensor(
        17, 0x1712, PLDM_ENTITY_PROC, 12, PLDM_STATESET_ID_PRESENCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu12");
    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu12"}, false);

    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_spare_channel_all_miss";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/processor/cpu12");
    StateSetMemorySpareChannelCoverage stateSet(
        PLDM_STATESET_ID_PRESENCE, 0, path, association, *stateSensor);

    stateSet.setAssociations(
        {{"processor", "all_states",
          "/xyz/openbmc_project/inventory/system/processor/cpu12"},
         {"chassis", "reverse_only",
          "/xyz/openbmc_project/inventory/system/processor/cpu12"}});
    EXPECT_NO_THROW(stateSet.setValue(PLDM_STATESET_PRESENCE_NOT_PRESENT));
}

TEST_F(StateSetCoverageTest,
       memorySpareChannelTelemetryIgnoresEmptyAssociationsCoverage)
{
    auto stateSensor = makeStateSensor(
        17, 0x1713, PLDM_ENTITY_PROC, 14, PLDM_STATESET_ID_PRESENCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu14");
    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu14"}, false);

    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_spare_channel_empty_assocs";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/processor/cpu14");
    StateSetMemorySpareChannelCoverage stateSet(
        PLDM_STATESET_ID_PRESENCE, 0, path, association, *stateSensor);

    stateSet.setAssociations({});
    EXPECT_NO_THROW(stateSet.updateShmemReading("MemorySpareChannelPresence"));
    EXPECT_NO_THROW(stateSet.setValue(PLDM_STATESET_PRESENCE_PRESENT));
}

TEST_F(StateSetCoverageTest,
       processorPowerBreakTelemetryIgnoresAllNonMatchingAssociationsCoverage)
{
    auto stateSensor = makeStateSensor(
        18, STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC, 13,
        PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu13");
    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu13"}, false);

    std::string path =
        "/xyz/openbmc_project/state/coverage/processor_power_break_all_miss";
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory13");
    StateSetProcessorPowerBreakCoverage stateSet(
        PLDM_STATESET_ID_PERFORMANCE, 0, path, association, *stateSensor);

    stateSet.setAssociations(
        {{"processor", "all_states",
          "/xyz/openbmc_project/inventory/system/processor/cpu13"},
         {"memory", "reverse_only",
          "/xyz/openbmc_project/inventory/system/memory/memory13"}});
    EXPECT_NO_THROW(stateSet.setValue(PLDM_STATESET_PERFORMANCE_THROTTLED));
}

TEST_F(StateSetCoverageTest,
       processorPowerBreakTelemetryIgnoresEmptyAssociationsCoverage)
{
    auto stateSensor = makeStateSensor(
        18, STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC, 14,
        PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu14");
    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu14"}, false);

    std::string path =
        "/xyz/openbmc_project/state/coverage/processor_power_break_empty_assocs";
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory14");
    StateSetProcessorPowerBreakCoverage stateSet(
        PLDM_STATESET_ID_PERFORMANCE, 0, path, association, *stateSensor);

    stateSet.setAssociations({});
    EXPECT_NO_THROW(stateSet.updateShmemReading("Value"));
    EXPECT_NO_THROW(stateSet.setValue(PLDM_STATESET_PERFORMANCE_NORMAL));
}

TEST_F(StateSetCoverageTest, nvlinkTelemetryIgnoresEmptyAssociationsCoverage)
{
    auto stateSensor = makeStateSensor(
        19, 0x1810, PLDM_ENTITY_SYS_BUS, 15, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric15_empty_assocs");
    stateSensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/fabrics/fabric15_empty_assocs"},
        false);

    std::string path =
        "/xyz/openbmc_project/state/coverage/nvlink_empty_associations";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/chassis/chassis15/CPU_0");
    StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                    association, *stateSensor);

    stateSet.setAssociations({});
    EXPECT_NO_THROW(stateSet.updateShmemReading("LinkState"));
    EXPECT_NO_THROW(stateSet.updateShmemReading("LinkStatus"));
    EXPECT_NO_THROW(stateSet.setValue(PLDM_STATE_SET_NVLINK_ACTIVE));
}

TEST_F(StateSetCoverageTest, nvlinkCtorThrowsWhenValueInterfacePathConflicts)
{
    auto stateSensor = makeStateSensor(
        19, 0x1810, PLDM_ENTITY_SYS_BUS, 15, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric15");
    std::string path =
        "/xyz/openbmc_project/state/coverage/nvlink_ctor_conflict";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/processor/cpu15");

    PortIntf conflictingPort(bus, path.c_str());
    while (bus.process_discard() > 0)
    {}

    EXPECT_ANY_THROW((StateSetNvlinkCoverage(PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
                                             path, association, *stateSensor)));
}

TEST_F(StateSetCoverageTest,
       nvlinkCtorThrowsWhenAssociationDefinitionsPathConflicts)
{
    auto stateSensor = makeStateSensor(
        19, 0x1814, PLDM_ENTITY_SYS_BUS, 16, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric16");
    std::string path =
        "/xyz/openbmc_project/state/coverage/nvlink_assoc_ctor_conflict";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/processor/cpu16");

    AssociationDefinitionsInft conflictingAssociations(bus, path.c_str());
    while (bus.process_discard() > 0)
    {}

    EXPECT_ANY_THROW((StateSetNvlinkCoverage(PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
                                             path, association, *stateSensor)));
}

TEST_F(StateSetCoverageTest,
       memoryPerformanceCtorThrowsWhenValueInterfacePathConflicts)
{
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_performance_ctor_conflict";
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory15");

    MemoryPerformanceIntf conflictingIntf(bus, path.c_str());
    while (bus.process_discard() > 0)
    {}

    EXPECT_ANY_THROW((StateSetMemoryPerformanceCoverage(
        PLDM_STATESET_ID_PERFORMANCE, 0, path, association)));
}

TEST_F(StateSetCoverageTest,
       memoryPerformanceCtorThrowsWhenAssociationDefinitionsPathConflicts)
{
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_performance_assoc_ctor_conflict";
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory16");

    AssociationDefinitionsInft conflictingAssociations(bus, path.c_str());
    while (bus.process_discard() > 0)
    {}

    EXPECT_ANY_THROW((StateSetMemoryPerformanceCoverage(
        PLDM_STATESET_ID_PERFORMANCE, 0, path, association)));
}

TEST_F(StateSetCoverageTest,
       memorySpareChannelCtorThrowsWhenValueInterfacePathConflicts)
{
    auto stateSensor = makeStateSensor(
        20, 0x1811, PLDM_ENTITY_PROC, 16, PLDM_STATESET_ID_PRESENCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu16");
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_spare_channel_ctor_conflict";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/processor/cpu16");

    MemorySpareChannelIntf conflictingIntf(bus, path.c_str());
    while (bus.process_discard() > 0)
    {}

    EXPECT_ANY_THROW((StateSetMemorySpareChannelCoverage(
        PLDM_STATESET_ID_PRESENCE, 0, path, association, *stateSensor)));
}

TEST_F(StateSetCoverageTest,
       memorySpareChannelCtorThrowsWhenAssociationDefinitionsPathConflicts)
{
    auto stateSensor = makeStateSensor(
        20, 0x1815, PLDM_ENTITY_PROC, 17, PLDM_STATESET_ID_PRESENCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu17_assoc");
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_spare_assoc_ctor_conflict";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/processor/cpu17_assoc");

    AssociationDefinitionsInft conflictingAssociations(bus, path.c_str());
    while (bus.process_discard() > 0)
    {}

    EXPECT_ANY_THROW((StateSetMemorySpareChannelCoverage(
        PLDM_STATESET_ID_PRESENCE, 0, path, association, *stateSensor)));
}

TEST_F(StateSetCoverageTest,
       processorPowerBreakCtorThrowsWhenValueInterfacePathConflicts)
{
    auto stateSensor = makeStateSensor(
        21, STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC, 17,
        PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu17");
    std::string path =
        "/xyz/openbmc_project/state/coverage/processor_power_break_ctor_conflict";
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory17");

    ProcessorPowerBreakIntf conflictingIntf(bus, path.c_str());
    while (bus.process_discard() > 0)
    {}

    EXPECT_ANY_THROW((StateSetProcessorPowerBreakCoverage(
        PLDM_STATESET_ID_PERFORMANCE, 0, path, association, *stateSensor)));
}

TEST_F(StateSetCoverageTest,
       processorPowerBreakCtorThrowsWhenAssociationDefinitionsPathConflicts)
{
    auto stateSensor = makeStateSensor(
        21, STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC, 18,
        PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu18_assoc");
    std::string path =
        "/xyz/openbmc_project/state/coverage/processor_power_break_assoc_ctor_conflict";
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory18_assoc");

    AssociationDefinitionsInft conflictingAssociations(bus, path.c_str());
    while (bus.process_discard() > 0)
    {}

    EXPECT_ANY_THROW((StateSetProcessorPowerBreakCoverage(
        PLDM_STATESET_ID_PERFORMANCE, 0, path, association, *stateSensor)));
}

TEST_F(StateSetCoverageTest, nvlinkCtorBadAllocCoverage)
{
    auto stateSensor = makeStateSensor(
        22, 0x1812, PLDM_ENTITY_SYS_BUS, 18, PLDM_NVIDIA_OEM_STATE_SET_NVLINK,
        "/xyz/openbmc_project/inventory/system/fabrics/fabric18");
    std::string path = "/xyz/openbmc_project/state/coverage/nvlink_ctor_alloc";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/processor/cpu18");

    EXPECT_TRUE(exerciseBadAlloc([&]() {
        StateSetNvlinkCoverage stateSet(PLDM_NVIDIA_OEM_STATE_SET_NVLINK, path,
                                        association, *stateSensor);
    }));
}

TEST_F(StateSetCoverageTest, memoryPerformanceCtorBadAllocCoverage)
{
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_performance_ctor_alloc";
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory18");

    EXPECT_TRUE(exerciseBadAlloc([&]() {
        StateSetMemoryPerformanceCoverage stateSet(PLDM_STATESET_ID_PERFORMANCE,
                                                   0, path, association);
    }));
}

TEST_F(StateSetCoverageTest, memorySpareChannelCtorBadAllocCoverage)
{
    auto stateSensor = makeStateSensor(
        23, 0x1813, PLDM_ENTITY_PROC, 19, PLDM_STATESET_ID_PRESENCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu19");
    std::string path =
        "/xyz/openbmc_project/state/coverage/memory_spare_channel_ctor_alloc";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/processor/cpu19");

    EXPECT_TRUE(exerciseBadAlloc([&]() {
        StateSetMemorySpareChannelCoverage stateSet(
            PLDM_STATESET_ID_PRESENCE, 0, path, association, *stateSensor);
    }));
}

TEST_F(StateSetCoverageTest, processorPowerBreakCtorBadAllocCoverage)
{
    auto stateSensor = makeStateSensor(
        24, STATE_SENSOR_CPU_POWER_BREAK, PLDM_ENTITY_PROC, 20,
        PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu20");
    std::string path =
        "/xyz/openbmc_project/state/coverage/processor_power_break_ctor_alloc";
    auto association = makeAssociation(
        "memory", "all_states",
        "/xyz/openbmc_project/inventory/system/memory/memory20");

    EXPECT_TRUE(exerciseBadAlloc([&]() {
        StateSetProcessorPowerBreakCoverage stateSet(
            PLDM_STATESET_ID_PERFORMANCE, 0, path, association, *stateSensor);
    }));
}

TEST_F(StateSetCoverageTest, ethIbPortLinkStateGuardCoverage)
{
    std::string path = "/xyz/openbmc_project/state/coverage/eth_port_7";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/network/eth7");
    StateSetEthIBPortLinkState stateSet(PLDM_STATESET_ID_LINKSTATE, 0, path,
                                        association, 7);

    EXPECT_FALSE(stateSet.isDerivedSensorAssociated());

    auto mismatchedSensor =
        makeNumericSensor(9, 0x1800, PLDM_ENTITY_ETHERNET, 8,
                          PLDM_SENSOR_UNIT_BITS, "bits_other_port");
    std::vector<std::shared_ptr<NumericSensor>> mismatchedSensors{
        mismatchedSensor};
    stateSet.associateNumericSensor(EntityInfo{1, PLDM_ENTITY_ETHERNET, 7},
                                    mismatchedSensors);
    EXPECT_EQ(nullptr, stateSet.linkSpeedSensor);

    auto wrongTypeSensor =
        makeNumericSensor(9, 0x1801, PLDM_ENTITY_SYS_BOARD, 7,
                          PLDM_SENSOR_UNIT_BITS, "bits_wrong_type");
    std::vector<std::shared_ptr<NumericSensor>> wrongTypeSensors{
        wrongTypeSensor};
    stateSet.associateNumericSensor(EntityInfo{1, PLDM_ENTITY_SYS_BOARD, 7},
                                    wrongTypeSensors);
    EXPECT_EQ(nullptr, stateSet.linkSpeedSensor);

    auto wrongUnitSensor =
        makeNumericSensor(9, 0x1802, PLDM_ENTITY_ETHERNET, 7,
                          PLDM_SENSOR_UNIT_WATTS, "watts_same_port");
    std::vector<std::shared_ptr<NumericSensor>> wrongUnitSensors{
        wrongUnitSensor};
    stateSet.associateNumericSensor(EntityInfo{1, PLDM_ENTITY_ETHERNET, 7},
                                    wrongUnitSensors);
    EXPECT_EQ(nullptr, stateSet.linkSpeedSensor);

    auto linkSpeedSensor =
        makeNumericSensor(9, 0x1803, PLDM_ENTITY_ETHERNET, 7,
                          PLDM_SENSOR_UNIT_BITS, "bits_same_port");
    linkSpeedSensor->updateReading(true, true, 40000000000.0);
    std::vector<std::shared_ptr<NumericSensor>> validSensors{linkSpeedSensor};
    stateSet.associateNumericSensor(EntityInfo{1, PLDM_ENTITY_ETHERNET, 7},
                                    validSensors);
    ASSERT_NE(nullptr, stateSet.linkSpeedSensor);

    std::string switchType{
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet"};
    std::vector<std::string> switchProtocols{
        "xyz.openbmc_project.Inventory.Item.Switch.SwitchType.Ethernet"};
    std::vector<pldm::dbus::PathAssociation> switchAssociations{association};
    auto switchBandwidthSensor =
        std::make_shared<oem_nvidia::SwitchBandwidthSensor>(
            9, "switch_bw_7", switchType, switchProtocols, switchAssociations);
    stateSet.associateDerivedSensor(switchBandwidthSensor);
    EXPECT_TRUE(stateSet.isDerivedSensorAssociated());

    stateSet.setValue(PLDM_STATESET_LINK_STATE_CONNECTED);
    auto firstSpeed = stateSet.ValuePortInfoIntf->currentSpeed();
    stateSet.setValue(PLDM_STATESET_LINK_STATE_CONNECTED);
    EXPECT_DOUBLE_EQ(firstSpeed, stateSet.ValuePortInfoIntf->currentSpeed());

    std::string nullInfoPath = "/xyz/openbmc_project/state/coverage/eth_port_8";
    StateSetEthIBPortLinkState nullInfoStateSet(PLDM_STATESET_ID_LINKSTATE, 1,
                                                nullInfoPath, association, 8);
    nullInfoStateSet.ValuePortInfoIntf.reset();
    nullInfoStateSet.associateDerivedSensor(switchBandwidthSensor);
    EXPECT_TRUE(nullInfoStateSet.isDerivedSensorAssociated());

    stateSet.associationDefinitionsIntf.reset();
    stateSet.ValuePortIntf.reset();
    stateSet.updateSensorName("Ethernet Port 7 Renamed");
    EXPECT_EQ("Ethernet Port 7 Renamed", stateSet.objectName);
}

TEST_F(StateSetCoverageTest, ethIbPortLinkStateInfinibandAssociationCoverage)
{
    std::string path = "/xyz/openbmc_project/state/coverage/ib_port_12";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/network/ib12");
    StateSetEthIBPortLinkState stateSet(PLDM_STATESET_ID_LINKSTATE, 0, path,
                                        association, 12);

    auto ibSensor = makeNumericSensor(12, 0x1812, PLDM_ENTITY_INFINIBAND, 12,
                                      PLDM_SENSOR_UNIT_BITS, "bits_ib_port");
    ibSensor->updateReading(true, true, 100000000000.0);

    std::vector<std::shared_ptr<NumericSensor>> sensors{ibSensor};
    stateSet.associateNumericSensor(EntityInfo{1, PLDM_ENTITY_INFINIBAND, 12},
                                    sensors);
    ASSERT_NE(nullptr, stateSet.linkSpeedSensor);

    stateSet.setMaxSpeedValue(200.0);
    stateSet.setValue(PLDM_STATESET_LINK_STATE_CONNECTED);

    EXPECT_DOUBLE_EQ(100.0, stateSet.ValuePortInfoIntf->currentSpeed());
    EXPECT_DOUBLE_EQ(200.0, stateSet.ValuePortInfoIntf->maxSpeed());

    auto [message, arg, level, eventId,
          impacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("LinkUp", arg);
    EXPECT_TRUE(eventId.empty());
    EXPECT_TRUE(impacted.empty());
}

TEST_F(StateSetCoverageTest, ethIbPortLinkStateLinkDownMissingEventIdCoverage)
{
    std::string path = "/xyz/openbmc_project/state/coverage/eth_port_13";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/network/eth13");
    StateSetEthIBPortLinkState stateSet(PLDM_STATESET_ID_LINKSTATE, 0, path,
                                        association, 13);

    auto sensorEventInfo = std::make_unique<pldm::utils::SensorEventInfo>();
    sensorEventInfo->impactedComponent = "Switch13";

    stateSet.setValue(PLDM_STATESET_LINK_STATE_DISCONNECTED);
    auto [message, arg, level, eventId,
          impacted] = stateSet.getEventData(sensorEventInfo.get());
    EXPECT_EQ("LinkDown", arg);
    EXPECT_TRUE(eventId.empty());
    EXPECT_EQ("Switch13", impacted);
}

TEST_F(StateSetCoverageTest, ethIbPortLinkStateAddAssociationEmptyCoverage)
{
    std::string path = "/xyz/openbmc_project/state/coverage/eth_port_14";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/network/eth14");
    StateSetEthIBPortLinkState stateSet(PLDM_STATESET_ID_LINKSTATE, 0, path,
                                        association, 14);

    stateSet.addAssociation({});
    EXPECT_TRUE(stateSet.associationDefinitionsIntf->associations().empty());
}

TEST_F(StateSetCoverageTest, ethIbPortLinkStateEventMatrixCoverage)
{
    std::string path = "/xyz/openbmc_project/state/coverage/eth_port_15";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/network/eth15");
    StateSetEthIBPortLinkState stateSet(PLDM_STATESET_ID_LINKSTATE, 0, path,
                                        association, 15);

    EXPECT_TRUE(stateSet.getStringStateType().empty());

    stateSet.ValuePortStateIntf->linkState(PortLinkStates::Error);
    stateSet.ValuePortStateIntf->linkStatus(PortLinkStatus::NoLink);
    auto [errorMessage, errorArg, errorLevel, errorEventId,
          errorImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("Error", errorArg);
    EXPECT_TRUE(errorEventId.empty());
    EXPECT_TRUE(errorImpacted.empty());

    stateSet.ValuePortStateIntf->linkState(PortLinkStates::Unknown);
    auto [unknownMessage, unknownArg, unknownLevel, unknownEventId,
          unknownImpacted] = stateSet.getEventData(nullptr);
    EXPECT_EQ("Unknown", unknownArg);
    EXPECT_TRUE(unknownEventId.empty());
    EXPECT_TRUE(unknownImpacted.empty());
}

TEST_F(StateSetCoverageTest, ethIbPortLinkStateAddAssociationMatrixCoverage)
{
    std::string path = "/xyz/openbmc_project/state/coverage/eth_port_16";
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/network/eth16");
    StateSetEthIBPortLinkState stateSet(PLDM_STATESET_ID_LINKSTATE, 0, path,
                                        association, 16);

    std::vector<pldm::dbus::PathAssociation> associations{
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/network/eth16"),
        makeAssociation("inventory", "monitored_by",
                        "/xyz/openbmc_project/inventory/system/chassis/"
                        "chassis16")};

    stateSet.addAssociation(associations);

    auto storedAssociations =
        stateSet.associationDefinitionsIntf->associations();
    ASSERT_EQ(2u, storedAssociations.size());
    EXPECT_EQ("chassis", std::get<0>(storedAssociations[0]));
    EXPECT_EQ("inventory", std::get<0>(storedAssociations[1]));
}

TEST_F(StateSetCoverageTest, ethIbPortLinkStateRenameSameNameCoverage)
{
    std::string path = "/xyz/openbmc_project/state/coverage/eth_port_same_name";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/network/eth_same_name");
    StateSetEthIBPortLinkStateRenameCoverage stateSet(
        PLDM_STATESET_ID_LINKSTATE, 0, path, association, 15);

    auto* associationIntf = stateSet.associationDefinitionsIntf.get();
    auto* portIntf = stateSet.ValuePortIntf.get();
    auto* portInfoIntf = stateSet.ValuePortInfoIntf.get();
    auto* portStateIntf = stateSet.ValuePortStateIntf.get();

    stateSet.updateSensorName("eth_port_same_name");

    EXPECT_EQ("eth_port_same_name", stateSet.objectName);
    EXPECT_EQ(associationIntf, stateSet.associationDefinitionsIntf.get());
    EXPECT_EQ(portIntf, stateSet.ValuePortIntf.get());
    EXPECT_EQ(portInfoIntf, stateSet.ValuePortInfoIntf.get());
    EXPECT_EQ(portStateIntf, stateSet.ValuePortStateIntf.get());
}

TEST_F(StateSetCoverageTest, ethIbPortLinkStateRenameNullInterfacesCoverage)
{
    std::string path = "/xyz/openbmc_project/state/coverage/eth_port_guard";
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/network/eth_guard");
    StateSetEthIBPortLinkStateRenameCoverage stateSet(
        PLDM_STATESET_ID_LINKSTATE, 0, path, association, 16);

    stateSet.resetAssociationDefinitionsIntf();
    stateSet.resetPortIntf();
    stateSet.resetPortInfoIntf();
    stateSet.resetPortStateIntf();

    EXPECT_NO_THROW(stateSet.updateSensorName("Eth Port 16+Guard"));
    EXPECT_EQ("Eth Port 16+Guard", stateSet.getStringStateType());
}

TEST_F(StateSetCoverageTest, performanceCtorBadAllocCoverage)
{
    auto sensor = makeStateSensor(
        25, 0x1825, PLDM_ENTITY_PROC, 21, PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu21/" +
            std::string(96, 'p'));
    std::string path =
        "/xyz/openbmc_project/state/coverage/performance_ctor_alloc_" +
        std::string(96, 'p');
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/processor/cpu21/" +
            std::string(96, 'a'));

    EXPECT_TRUE(exerciseBadAlloc([&]() {
        StateSetPerformanceCoverage stateSet(PLDM_STATESET_ID_PERFORMANCE, 0,
                                             path, association, *sensor);
    }));
}

TEST_F(StateSetCoverageTest, performanceUpdateShmemReadingBadAllocCoverage)
{
    auto sensor = makeStateSensor(
        26, 0x1826, PLDM_ENTITY_PROC, 22, PLDM_STATESET_ID_PERFORMANCE,
        "/xyz/openbmc_project/inventory/system/processor/cpu22");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/processor/cpu22"}, false);

    std::string path =
        "/xyz/openbmc_project/state/coverage/performance_update_alloc_" +
        std::string(96, 'u');
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/processor/cpu22");
    StateSetPerformanceCoverage stateSet(PLDM_STATESET_ID_PERFORMANCE, 0, path,
                                         association, *sensor);
    Associations associations{
        {"chassis", "all_states",
         "/xyz/openbmc_project/inventory/system/processor/cpu22/" +
             std::string(96, 'e')}};
    stateSet.setAssociations(associations);

    EXPECT_TRUE(
        exerciseBadAlloc([&]() { stateSet.updateShmemReading("Value"); }));
}

TEST_F(StateSetCoverageTest, powerSupplyInputCtorBadAllocCoverage)
{
    auto sensor = makeStateSensor(
        27, 0x1827, PLDM_ENTITY_POWER_SUPPLY, 23, PLDM_STATESET_ID_POWERSUPPLY,
        "/xyz/openbmc_project/inventory/system/powersupply/psu23/" +
            std::string(96, 's'));
    std::string path =
        "/xyz/openbmc_project/state/coverage/power_supply_ctor_alloc_" +
        std::string(96, 'p');
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/powersupply/psu23/" +
            std::string(96, 'a'));

    EXPECT_TRUE(exerciseBadAlloc([&]() {
        StateSetPowerSupplyInputCoverage stateSet(
            PLDM_STATESET_ID_POWERSUPPLY, 0, path, association, *sensor);
    }));
}

TEST_F(StateSetCoverageTest, powerSupplyInputUpdateShmemReadingBadAllocCoverage)
{
    auto sensor = makeStateSensor(
        28, 0x1828, PLDM_ENTITY_POWER_SUPPLY, 24, PLDM_STATESET_ID_POWERSUPPLY,
        "/xyz/openbmc_project/inventory/system/powersupply/psu24");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/powersupply/psu24"}, false);

    std::string path =
        "/xyz/openbmc_project/state/coverage/power_supply_update_alloc_" +
        std::string(96, 'u');
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/powersupply/psu24");
    StateSetPowerSupplyInputCoverage stateSet(PLDM_STATESET_ID_POWERSUPPLY, 0,
                                              path, association, *sensor);
    Associations associations{
        {"chassis", "all_states",
         "/xyz/openbmc_project/inventory/system/powersupply/psu24/" +
             std::string(96, 'e')}};
    stateSet.setAssociations(associations);

    EXPECT_TRUE(
        exerciseBadAlloc([&]() { stateSet.updateShmemReading("Status"); }));
}

TEST_F(StateSetCoverageTest, pciePortLinkStateCtorBadAllocCoverage)
{
    auto sensor = makeStateSensor(
        29, 0x1829, PLDM_ENTITY_PCI_EXPRESS_BUS, 25, PLDM_STATESET_ID_LINKSTATE,
        "/xyz/openbmc_project/inventory/system/pcie/slot25/" +
            std::string(96, 'p'));
    std::string path = "/xyz/openbmc_project/state/coverage/pcie_ctor_alloc_" +
                       std::string(96, 'p');
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/pcie/slot25/" +
                            std::string(96, 'a'));

    EXPECT_TRUE(exerciseBadAlloc([&]() {
        StateSetPciePortLinkStateCoverage stateSet(
            PLDM_STATESET_ID_LINKSTATE, 0, path, association, *sensor);
    }));
}

TEST_F(StateSetCoverageTest,
       pciePortLinkStateUpdateShmemReadingBadAllocCoverage)
{
    auto sensor = makeStateSensor(
        30, 0x1830, PLDM_ENTITY_PCI_EXPRESS_BUS, 26, PLDM_STATESET_ID_LINKSTATE,
        "/xyz/openbmc_project/inventory/system/pcie/slot26");
    sensor->setInventoryPaths(
        {"/xyz/openbmc_project/inventory/system/pcie/slot26"}, false);

    std::string path =
        "/xyz/openbmc_project/state/coverage/pcie_update_alloc_" +
        std::string(96, 'u');
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/pcie/slot26");
    StateSetPciePortLinkStateCoverage stateSet(PLDM_STATESET_ID_LINKSTATE, 0,
                                               path, association, *sensor);
    Associations associations{
        {"chassis", "all_states",
         "/xyz/openbmc_project/inventory/system/pcie/slot26/" +
             std::string(96, 'e')}};
    stateSet.setAssociations(associations);

    EXPECT_TRUE(
        exerciseBadAlloc([&]() { stateSet.updateShmemReading("LinkState"); }));
}

TEST_F(StateSetCoverageTest, ethIbPortLinkStateCtorBadAllocCoverage)
{
    std::string path = "/xyz/openbmc_project/state/coverage/eth_ctor_alloc_" +
                       std::string(96, 'e');
    auto association = makeAssociation(
        "chassis", "all_states",
        "/xyz/openbmc_project/inventory/system/network/eth_ctor/" +
            std::string(96, 'a'));

    EXPECT_TRUE(exerciseBadAlloc([&]() {
        StateSetEthIBPortLinkState stateSet(PLDM_STATESET_ID_LINKSTATE, 0, path,
                                            association, 27);
    }));
}

TEST_F(StateSetCoverageTest,
       ethIbPortLinkStateStringAndTelemetryBadAllocCoverage)
{
    std::string path = "/xyz/openbmc_project/state/coverage/eth_string_alloc_" +
                       std::string(96, 'e');
    auto association =
        makeAssociation("chassis", "all_states",
                        "/xyz/openbmc_project/inventory/system/network/eth28");
    StateSetEthIBPortLinkStateRenameCoverage stateSet(
        PLDM_STATESET_ID_LINKSTATE, 0, path, association, 28);

    stateSet.objectName = "eth_port_" + std::string(96, 'n');
    stateSet.addSharedMemObjectPath(
        "/xyz/openbmc_project/inventory/system/network/eth28/" +
        std::string(96, 's'));

    EXPECT_TRUE(exerciseBadAlloc([&]() {
        auto copy = stateSet.getStringStateType();
        (void)copy;
    }));

    EXPECT_TRUE(exerciseBadAlloc([&]() { stateSet.updateSharedMemory(); }));
}

} // namespace
