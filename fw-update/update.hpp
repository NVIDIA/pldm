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
    Update(sdbusplus::bus::bus& bus, const std::string& path,
           UpdateManager* updateManager) :
        UpdateIntf(bus, path.c_str()), updateManager(updateManager),
        objPath(path)
    {
        allowedForceUpdate(true);
        allowedTargets(true);
    }

    virtual sdbusplus::message::object_path startUpdate(
        sdbusplus::message::unix_fd image,
        ApplyTimeIntf::RequestedApplyTimes applyTime, bool forceUpdate,
        std::vector<sdbusplus::message::object_path> targets) override;

    /** @brief Close the image stream and release the mmap
     *
     *  Release resources by unmapping the firmware image and closing
     *  the file descriptor. This prevents memory leaks and ensures proper
     *  cleanup after firmware update completion.
     */
    void clearImageStream()
    {
        mmapStream.reset();
        mmapFile.unmap();
    }

    /** @brief Get reference to the image stream for reading
     *
     *  @return Reference to the istream containing the firmware image data.
     *          The stream reads directly from memory-mapped data, providing
     *          zero-copy access without relying on /proc filesystem.
     */
    std::istream& getImageStream()
    {
        return *mmapStream;
    }

    ~Update() noexcept override
    {
        clearImageStream();
    }

  private:
    UpdateManager* updateManager;
    const std::string objPath;
    pldm::MmapFile mmapFile;
    std::unique_ptr<pldm::MmapStream> mmapStream;
};

} // namespace fw_update

} // namespace pldm
