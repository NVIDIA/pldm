#pragma once

#include "common/mmap_stream.hpp"

#include <unistd.h>

#include <xyz/openbmc_project/Software/ApplyTime/server.hpp>
#include <xyz/openbmc_project/Software/Update/server.hpp>

#include <memory>
namespace pldm
{

namespace fw_update
{

class UpdateManager;

using UpdateIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Software::server::Update>;
using ApplyTimeIntf =
    sdbusplus::xyz::openbmc_project::Software::server::ApplyTime;

/** @class Update
 *
 *  Concrete implementation of xyz.openbmc_project.Software.Update D-Bus
 *  interface
 */
class Update : public UpdateIntf
{
  public:
    /** @brief Constructor
     *
     *  @param[in] bus - Bus to attach to
     *  @param[in] objPath - D-Bus object path
     *  @param[in] updateManager - Reference to FW update manager
     */
    Update(sdbusplus::bus_t& bus, const std::string& path,
           UpdateManager* updateManager) :
        UpdateIntf(bus, path.c_str()), updateManager(updateManager),
        objPath(path)
    {
        allowedForceUpdate(true);
        // Supported only on platforms with a known updatable device scope.
        // On develop that scope (EM configurations, MCTP static config,
        // discovered endpoints) arrives asynchronously after construction,
        // so start at false; the firmware update Manager republishes it via
        // UpdateManager::refreshAllowedPreUpdateValidation() whenever the
        // scope state changes.
        allowedPreUpdateValidation(false);
        allowedTargets(true);
    }

    sdbusplus::object_path startUpdate(
        sdbusplus::message::unix_fd image,
        ApplyTimeIntf::RequestedApplyTimes applyTime, bool forceUpdate,
        std::vector<sdbusplus::object_path> targets,
        bool preUpdateValidation) override;

    /** @brief Release the mapped firmware image
     *
     *  Release resources by unmapping the firmware image and closing
     *  the file descriptor. This prevents memory leaks and ensures proper
     *  cleanup after firmware update completion. Any pointer previously
     *  returned by getImageData() dangles after this call.
     */
    void clearImageData()
    {
        mmapFile.unmap();
    }

    /** @brief Get pointer to the memory-mapped firmware image data
     *
     *  @return Pointer to the memory-mapped firmware image data, providing
     *          zero-copy access without relying on /proc filesystem.
     */
    const uint8_t* getImageData() const
    {
        return static_cast<const uint8_t*>(mmapFile.data());
    }

    /** @brief Get the size of the memory-mapped firmware image data
     *
     *  @return Size of the memory-mapped firmware image data in bytes
     */
    size_t getImageSize() const
    {
        return mmapFile.size();
    }

    ~Update() noexcept override
    {
        clearImageData();
    }

  private:
    UpdateManager* updateManager;
    const std::string objPath;
    pldm::MmapFile mmapFile;
};

} // namespace fw_update

} // namespace pldm
