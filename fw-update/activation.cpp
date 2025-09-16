#include "fw-update/activation.hpp"

#include "fw-update/update_manager.hpp"

namespace pldm
{
namespace fw_update
{

ActivationIntf::Activations Activation::activation(
    ActivationIntf::Activations value) override
{
        if (value == Activations::Activating)
        {
            deleteImpl.reset();
            namespace software =
                sdbusplus::xyz::openbmc_project::Software::server;
            updateManager->performSecurityChecksAsync(
                [this, updateManager(updateManager)](bool securityCheck) {
                    if (!securityCheck)
                    {
                        lg2::error(
                            "Security checks failed setting activation to fail");
                        updateManager->resetActivationBlocksTransition();
                        updateManager->clearFirmwareUpdatePackage();

                        ActivationIntf::activation(
                            software::Activation::Activations::Failed);
                    }
                    else
                    {
                        auto state = updateManager->activatePackage();

                        if (state == Activations::Failed)
                        {
                            lg2::error(
                                "Activation failed setting activation to fail");
                            updateManager->resetActivationBlocksTransition();
                            updateManager->clearFirmwareUpdatePackage();
                        }
                        else if (state == Activations::Active)
                        {
                            lg2::info("Activation set to active");
                            updateManager->clearFirmwareUpdatePackage();
                        }
                    }
                },
                [this,
                 updateManager(updateManager)](const std::string& errorMsg) {
                    lg2::error(
                        "Security checks failed setting activation to fail");
                    lg2::error(
                        "Exception during activation security check: {ERRORMSG}",
                        "ERRORMSG", errorMsg);
                    updateManager->resetActivationBlocksTransition();
                    updateManager->clearFirmwareUpdatePackage();

                    ActivationIntf::activation(
                        software::Activation::Activations::Failed);
                });
        }
        else if (value == Activations::Active || value == Activations::Failed)
        {
            if (!deleteImpl)
            {
                deleteImpl =
                    std::make_unique<Delete>(bus, objPath, updateManager);
            }
        }
        return ActivationIntf::activation(value);
}

void Delete::delete_() override
{
    updateManager->clearActivationInfo();
}
} // namespace fw_update
} // namespace pldm
