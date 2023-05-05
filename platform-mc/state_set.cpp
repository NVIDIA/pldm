#include "state_set.hpp"

#ifdef OEM_NVIDIA
#include "oem/nvidia/platform-mc/state_set/memorySpareChannel.hpp"
#include "oem/nvidia/platform-mc/state_set/nvlink.hpp"
#endif
#include "libpldm/entity.h"
#include "libpldm/platform.h"

#include "state_effecter.hpp"
#include "state_sensor.hpp"
#include "state_set/clearNonVolatileVariables.hpp"
#include "state_set/pciePortLinkState.hpp"
#include "state_set/performance.hpp"
#include "state_set/powerSupplyInput.hpp"
#include "state_set/presenceState.hpp"

namespace pldm
{
namespace platform_mc
{

std::unique_ptr<StateSet> StateSetCreator::createSensor(
    uint16_t stateSetId, uint8_t compId, std::string& path,
    dbus::PathAssociation& stateAssociation, StateSensor* sensor)
{
    if (!sensor)
    {
        return nullptr;
    }

    auto& [containerId, entityType, entityInstance] =
        std::get<0>(sensor->sensorInfo);

#ifdef OEM_NVIDIA
    if (stateSetId == PLDM_STATESET_ID_PRESENCE &&
        (entityType == PLDM_ENTITY_PROC ||
         entityType == PLDM_ENTITY_MEMORY_CONTROLLER))
    {
        return std::make_unique<StateSetMemorySpareChannel>(
            stateSetId, compId, path, stateAssociation);
    }
    else if (stateSetId == PLDM_NVIDIA_OEM_STATE_SET_NVLINK &&
             entityType == PLDM_ENTITY_SYS_BUS)
    {
        return std::make_unique<oem_nvidia::StateSetNvlink>(stateSetId, path,
                                                            stateAssociation);
    }
#endif

    if (stateSetId == PLDM_STATESET_ID_PERFORMANCE)
    {
        return std::make_unique<StateSetPerformance>(stateSetId, compId, path,
                                                     stateAssociation);
    }
    else if (stateSetId == PLDM_STATESET_ID_POWERSUPPLY)
    {
        return std::make_unique<StateSetPowerSupplyInput>(
            stateSetId, compId, path, stateAssociation);
    }
    else if (stateSetId == PLDM_STATESET_ID_LINKSTATE &&
             entityType == PLDM_ENTITY_PCI_EXPRESS_BUS)
    {
        return std::make_unique<StateSetPciePortLinkState>(
            stateSetId, compId, path, stateAssociation);
    }
    else if (stateSetId == PLDM_STATESET_ID_BOOT_REQUEST)
    {
        return std::make_unique<StateSetClearNonvolatileVariable>(
            stateSetId, compId, path, stateAssociation, nullptr);
    }

    lg2::error(
        "State Sensor PDR Info, Composite ID:{COMPID} Set ID is unknown Id:{STATESETID}.",
        "COMPID", compId, "STATESETID", stateSetId);
    return nullptr;
}

std::unique_ptr<StateSet> StateSetCreator::createEffecter(
    uint16_t stateSetId, uint8_t compId, std::string& path,
    dbus::PathAssociation& stateAssociation, StateEffecter* effecter)
{
    if (effecter == nullptr)
    {
        lg2::error(
            "Invalid State Effecter Parameter, Set ID is : {STATESETID}.",
            "STATESETID", stateSetId);
        return nullptr;
    }

    if (stateSetId == PLDM_STATESET_ID_BOOT_REQUEST)
    {
        return std::make_unique<StateSetClearNonvolatileVariable>(
            stateSetId, compId, path, stateAssociation, effecter);
    }
    else
    {
        lg2::error(
            "State Effecter PDR Info, Composite ID:{COMPID} Set ID is unknown Id: {STATESETID}.",
            "COMPID", compId, "STATESETID", stateSetId);
        return nullptr;
    }
}

} // namespace platform_mc
} // namespace pldm
