#include "state_set.hpp"

#ifdef OEM_NVIDIA
#include "oem/nvidia/platform-mc/state_set_oem.hpp"
#endif
#include "libpldm/platform.h"

#include "state_effecter.hpp"

namespace pldm
{
namespace platform_mc
{

std::unique_ptr<StateSet>
    StateSetCreator::createSensor(uint16_t stateSetId, uint8_t compId,
                                  std::string& path,
                                  dbus::PathAssociation& stateAssociation)
{
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
    else if (stateSetId == PLDM_STATESET_ID_LINKSTATE)
    {
        return std::make_unique<StateSetRemoteDebug>(stateSetId, compId, path,
                                                     stateAssociation, nullptr);
    }
#ifdef OEM_NVIDIA
    else if (stateSetId == PLDM_NVIDIA_OEM_STATE_SET_NVLINK)
    {
        return std::make_unique<oem_nvidia::StateSetNvlink>(stateSetId, path,
                                                            stateAssociation);
    }
#endif
    else
    {
        std::cerr << "State Sensor PDR Info, Set ID is unknown Id: "
                  << unsigned(stateSetId) << "\n";
        return nullptr;
    }
}

std::unique_ptr<StateSet> StateSetCreator::createEffecter(
    uint16_t stateSetId, uint8_t compId, std::string& path,
    dbus::PathAssociation& stateAssociation, StateEffecter* effecter)
{
    if (effecter == nullptr)
    {
        std::cerr << "Invalid State Effecter Parameter, Set ID is : "
                  << unsigned(stateSetId) << "\n";
        return nullptr;
    }

    if (stateSetId == PLDM_STATESET_ID_LINKSTATE)
    {
        return std::make_unique<StateSetRemoteDebug>(
            stateSetId, compId, path, stateAssociation, effecter);
    }
    else
    {
        std::cerr << "State Effecter PDR Info, Set ID is unknown Id: "
                  << unsigned(stateSetId) << "\n";
        return nullptr;
    }
}

void RemoteDebugEffecterIntf::update(bool value)
{
    RemoteDebugInterfaceIntf::enabled(value);
}

bool RemoteDebugEffecterIntf::enabled(bool value)
{
    uint8_t requestState = 0;
    if (value)
    {
        requestState = PLDM_STATESET_LINK_STATE_CONNECTED;
    }
    else
    {
        requestState = PLDM_STATESET_LINK_STATE_DISCONNECTED;
    }

    effecter.setStateEffecterStates(compId, requestState).detach();
    return value;
}

} // namespace platform_mc
} // namespace pldm
