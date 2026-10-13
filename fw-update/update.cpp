#include "update.hpp"

#include "activation.hpp"
#include "update_manager.hpp"

#include <unistd.h>

#include <phosphor-logging/lg2.hpp>
#include <xyz/openbmc_project/Common/error.hpp>
#include <xyz/openbmc_project/Software/Update/error.hpp>

#include <stdexcept>

PHOSPHOR_LOG2_USING;
namespace pldm
{
namespace fw_update
{

using InvalidImage =
    sdbusplus::xyz::openbmc_project::Software::Update::Error::InvalidImage;

sdbusplus::object_path Update::startUpdate(
    sdbusplus::message::unix_fd image,
    ApplyTimeIntf::RequestedApplyTimes applyTime, bool forceUpdate,
    std::vector<sdbusplus::object_path> targets, bool preUpdateValidation)
{
    updateManager->clearExistingActivation();
    updateManager->setRequestedApplyTime(applyTime);

    info("Starting update for image {FD}", "FD", image.fd);

    int imageFd = dup(image.fd);
    if (imageFd < 0)
    {
        throw std::runtime_error("Failed to duplicate image file descriptor");
    }

    off_t imageSize = lseek(imageFd, 0, SEEK_END);
    if (imageSize <= 0)
    {
        error("Rejecting firmware image of size {SIZE}", "SIZE", imageSize);
        close(imageFd);
        throw InvalidImage();
    }

    if (!mmapFile.map(imageFd, true /* take ownership of fd */))
    {
        // map() has already closed imageFd on every failure path because
        // ownership was handed over; closing again here would risk closing
        // an unrelated descriptor opened in the meantime.
        throw std::runtime_error("Failed to memory map firmware image");
    }

    auto packageSize = mmapFile.size();

    return sdbusplus::object_path(updateManager->processPackageDataDefer(
        static_cast<const uint8_t*>(mmapFile.data()), packageSize, forceUpdate,
        targets, preUpdateValidation));
}

} // namespace fw_update
} // namespace pldm
