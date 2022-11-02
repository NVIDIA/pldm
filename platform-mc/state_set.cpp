#include "state_set.hpp"

#ifdef OEM_NVIDIA
#include "oem/nvidia/platform-mc/state_set_oem.hpp"
#endif

namespace pldm
{
namespace platform_mc
{

std::unique_ptr<StateSet>
    StateSetCreator::create(uint16_t stateSetId, std::string& path,
                            dbus::PathAssociation& stateAssociation)
{
    if (stateSetId == PLDM_STATESET_ID_PERFORMANCE)
    {
        return std::make_unique<StateSetPerformance>(stateSetId, path,
                                                     stateAssociation);
    }
    else if (stateSetId == PLDM_STATESET_ID_POWERSUPPLY)
    {
        return std::make_unique<StateSetPowerSupplyInput>(stateSetId, path,
                                                          stateAssociation);
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
        std::cerr << "State Sensor PDR Info, Set ID is unknown\n";
        return nullptr;
    }
}

} // namespace platform_mc
} // namespace pldm
