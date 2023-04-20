#ifndef STATE_SET_OEM_NVIDIA_H
#define STATE_SET_OEM_NVIDIA_H

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief NVIDIA OEM State Set IDs */
    enum nvidia_oem_pldm_state_set_ids
    {
        PLDM_NVIDIA_OEM_STATE_SET_NVLINK = 0x8000,
        PLDM_NVIDIA_OEM_STATE_SET_DEBUG_STATE = 0x8001
    };

    /** @brief PLDM state set ID 0x8000 NVLINK values  */
    enum nvidia_oem_pldm_state_set_nvlink_values
    {
        PLDM_STATE_SET_NVLINK_INACTIVE = 1,
        PLDM_STATE_SET_NVLINK_ACTIVE = 2,
        PLDM_STATE_SET_NVLINK_ERROR = 3
    };

    /** @brief PLDM state set ID 0x8001 debug inteface values  */
    enum pldm_state_set_debug_interface_values
    {
        /* always enabled or toggled on */
        PLDM_STATE_SET_DEBUG_STATE_ENABLED = 1,

        /* disabled but toggleable */
        PLDM_STATE_SET_DEBUG_STATE_DISABLED = 2,

        /* offline, cannot be enabled */
        PLDM_STATE_SET_DEBUG_STATE_OFFLINE = 3
    };

#ifdef __cplusplus
}
#endif

#endif /* STATE_SET_OEM_NVIDIA_H */
