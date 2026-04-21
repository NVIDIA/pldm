#pragma once

#include "libpldmresponder/platform.hpp"
#include "platform-mc/manager.hpp"

namespace pldm
{
namespace oem_nvidia
{

[[maybe_unused]] static constexpr uint8_t
    PLDM_OEM_NVIDIA_LEGACY_CPER_EVENT_CLASS = 0xFA;

/**
 * @class OemNVIDIA
 *
 * @brief class for creating all the OEM NVIDIA handlers
 */
class OemNVIDIA
{
  public:
    OemNVIDIA() = delete;
    OemNVIDIA& operator=(const OemNVIDIA&) = delete;
    OemNVIDIA(OemNVIDIA&&) = delete;
    OemNVIDIA& operator=(OemNVIDIA&&) = delete;

  public:
    /** Constructs OemNVIDIA object
     *
     * @param[in] platformHandler - platformHandler handler
     * @param[in] platformManager - Platform Manager
     */
    explicit OemNVIDIA(responder::platform::Handler* platformHandler,
                       platform_mc::Manager* platformManager)
    {
        createOemEventHandler(platformHandler, platformManager);
    }

  private:
    /** @brief Method for creating OEM event handlers
     *
     *  Legacy NVIDIA OEM CPER event handling is already wired through the
     *  merged platform-mc event path, including both pushed and polled events.
     *  Keep this compatibility object present without re-registering duplicate
     *  handlers.
     */
    void createOemEventHandler(
        [[maybe_unused]] responder::platform::Handler* platformHandler,
        [[maybe_unused]] platform_mc::Manager* platformManager)
    {
    }
};

} // namespace oem_nvidia
} // namespace pldm
