#pragma once

#include "libpldm/firmware_update.h"
#include "common/utils.hpp"
#include "common/types.hpp"

#include "fw-update/device_updater.hpp"
#include "fw-update/package_parser.hpp"
#include "fw-update/update_manager.hpp"

namespace software = sdbusplus::xyz::openbmc_project::Software::server;

namespace testing
{

software::Activation::Activations updateManagerActivatePackageResult = 
    software::Activation::Activations::Active;

class FakeUpdateManager
{
    public:
    bool fwDebug = true;

    software::Activation::Activations activatePackage()
    {
        return updateManagerActivatePackageResult;
    }

    void clearActivationInfo()
    {
        return;
    }

    void resetActivationBlocksTransition()
    {
        return;      
    }
    
    void clearFirmwareUpdatePackage()
    {
        return;    
    }
    void clearStagedPackage()
    {
        return;
    }
    int processPackage([
        [maybe_unused]] const std::filesystem::path& packageFilePath)
    {
        return 0;
    }
    std::string stagedObjPath;
    std::filesystem::path stagedfwPackageFilePath;
};
}//namespace testing

#define UpdateManager testing::FakeUpdateManager