#include "update.hpp"

#include "activation.hpp"
#include "update_manager.hpp"

#include <unistd.h>

#include <phosphor-logging/lg2.hpp>
#include <xyz/openbmc_project/Common/error.hpp>

#include <stdexcept>

PHOSPHOR_LOG2_USING;
namespace pldm
{
namespace fw_update
{

sdbusplus::message::object_path Update::startUpdate(
    sdbusplus::message::unix_fd image,
    ApplyTimeIntf::RequestedApplyTimes applyTime, bool forceUpdate,
    std::vector<sdbusplus::message::object_path> targets)
{
    updateManager->clearExistingActivation();
    updateManager->setRequestedApplyTime(applyTime);

    info("Starting update for image {FD}", "FD", image.fd);

    int imageFd = dup(image.fd);
    if (imageFd < 0)
    {
        throw std::runtime_error("Failed to duplicate image file descriptor");
    }

    if (!mmapFile.map(imageFd, true /* take ownership of fd */))
    {
        close(imageFd);
        throw std::runtime_error("Failed to memory map firmware image");
    }

    mmapStream =
        std::make_unique<pldm::MmapStream>(mmapFile.data(), mmapFile.size());

    if (!mmapStream->good())
    {
        throw std::runtime_error(
            "Failed to create stream from memory-mapped data");
    }

    auto packageSize = mmapFile.size();

    return sdbusplus::message::object_path(updateManager->processStreamDefer(
        *mmapStream, packageSize, forceUpdate, targets));
}

} // namespace fw_update
} // namespace pldm
