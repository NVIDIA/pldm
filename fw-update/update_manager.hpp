#pragma once

#include "libpldm/base.h"
#include "libpldm/requester/pldm.h"

#include "common/types.hpp"
#include "device_updater.hpp"
#include "error_handling.hpp"
#include "other_device_update_manager.hpp"
#include "package_parser.hpp"
#include "pldmd/dbus_impl_requester.hpp"
#include "requester/handler.hpp"
#include "watch.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <tuple>
#include <unordered_map>

#ifdef OEM_NVIDIA
#include "oem-nvidia/debug_token.hpp"
#endif

namespace pldm
{

namespace fw_update
{

using namespace sdeventplus;
using namespace sdeventplus::source;
using namespace pldm::dbus_api;
using namespace pldm;
namespace software = sdbusplus::xyz::openbmc_project::Software::server;

using DeviceIDRecordOffset = size_t;
using DeviceUpdaterInfo = std::pair<mctp_eid_t, DeviceIDRecordOffset>;
using DeviceUpdaterInfos = std::vector<DeviceUpdaterInfo>;
using TotalComponentUpdates = size_t;

class Activation;
class ActivationProgress;
class UpdatePolicy;
class ActivationBlocksTransition;

class UpdateManager
{
  public:
    UpdateManager() = delete;
    UpdateManager(const UpdateManager&) = delete;
    UpdateManager(UpdateManager&&) = delete;
    UpdateManager& operator=(const UpdateManager&) = delete;
    UpdateManager& operator=(UpdateManager&&) = delete;
    ~UpdateManager();

    explicit UpdateManager(
        Event& event,
        pldm::requester::Handler<pldm::requester::Request>& handler,
        Requester& requester, const DescriptorMap& descriptorMap,
        const ComponentInfoMap& componentInfoMap,
        ComponentNameMap& componentNameMap, bool fwDebug);

    /** @brief Handle PLDM request for the commands in the FW update
     *         specification
     *
     *  @param[in] eid - Remote MCTP Endpoint ID
     *  @param[in] command - PLDM command code
     *  @param[in] request - PLDM request message
     *  @param[in] requestLen - PLDM request message length
     *
     *  @return PLDM response message
     */
    Response handleRequest(mctp_eid_t eid, uint8_t command,
                           const pldm_msg* request, size_t reqMsgLen);

    int processPackage(const std::filesystem::path& packageFilePath);

    /** @brief Update firmware update completion status of each device
     *
     *  @param[in] eid - Remote MCTP Endpoint ID
     *  @param[in] status - True to indicate success and false for failure
     *  @param[in] successCompNames - Name of components successfully updated
     */
    void updateDeviceCompletion(
        mctp_eid_t eid, bool status,
        const std::vector<ComponentName>& successCompNames = {});

    /**
     * @brief Increments completed updates and refreshes the reported progress
     *
     */
    void updateActivationProgress();

    /** @brief Callback function that will be invoked when the
     *         RequestedActivation will be set to active in the Activation
     *         interface
     *         Handles activation for PLDM and non PLDM devices , in case
     *         of No device detections we will set default to Active state
     * @return returns Activations state
     */
    software::Activation::Activations activatePackage();

    void clearActivationInfo();

    /** @brief Associate firmware update package to devices and components that
     *         will be updated. The package to firmware device association is
     *         as per DSP0267. The target filtering can be used to override the
     *         devices intended to be updated by the package. It calculates the
     *         total number of components to be updated.
     *
     *  @param[in] inFwDeviceIDRecords - Firmware device descriptors from the
     *                                 package
     *  @param[in] descriptorMap - Descriptor information of all the discovered
     *                             MCTP endpoints
     *  @param[in] compImageInfos - Component image information in the package
     *  @param[in] componentNameMap - Match components on a device to component
     *                                name and will be used for target filtering
     *  @param[in] objectPaths - Software object paths used for target filtering
     *  @param[out] outFwDeviceIDRecords - Firmware device descriptors derived
     *                                     from the package after applying
     *                                     target filtering
     *  @param[out] totalNumComponentUpdates - Total number of component updates
     *
     *  @return If there are devices to be updated with the package, return
     *          all the EIDs to be updated and the matching firmware device
     *          descriptors in outFwDeviceIDRecords
     */
    DeviceUpdaterInfos associatePkgToDevices(
        const FirmwareDeviceIDRecords& inFwDeviceIDRecords,
        const DescriptorMap& descriptorMap,
        const ComponentImageInfos& compImageInfos,
        const ComponentNameMap& componentNameMap,
        const std::vector<sdbusplus::message::object_path>& objectPaths,
        FirmwareDeviceIDRecords& outFwDeviceIDRecords,
        TotalComponentUpdates& totalNumComponentUpdates);

    /** @brief Translate the RequestedComponentActivationMethod in PLDM spec to
     *         a human readable string. Multiple activation methods can be
     *         supported by the component, in which case "or" is used to link
     *         multiple methods. For example "AC power cycle or DC power cycle"
     *
     *  @param[in] componentActivationMethod - Component activation method
     *
     *  @return Component activation methods as std::string
     */
    std::string getActivationMethod(bitfield16_t compActivationModification);

    /** @brief Create message registry for firmware update
     *
     *  @param[in] eid - Remote MCTP Endpoint ID
     *  @param[in] fwDeviceIDRecord - FirmwareDeviceIDRecord in the fw update
     *                                package that matches the firmware device
     *  @param[in] compIndex - component index
     *  @param[in] messageID - messageID string
     *  @param[in] resolution - resolution field for the message registry
     *                          (optional)
     *  @param[in] commandType - pldm command type (optional). Default is 0 - no
     * oem messages will be logged.
     *  @param[in] errorCode - error code (optional)
     */
    void
        createMessageRegistry(mctp_eid_t eid,
                              const FirmwareDeviceIDRecord& fwDeviceIDRecord,
                              size_t compIndex, const std::string& messageID,
                              const std::string& resolution = {},
                              const pldm_firmware_update_commands commandType =
                                  static_cast<pldm_firmware_update_commands>(0),
                              const uint8_t errorCode = 0);

    /**
     * @brief Create a Message Registry for Resource Errors
     *
     * @param[in] eid - Remote MCTP Endpoint ID
     * @param[in] fwDeviceIDRecord - FirmwareDeviceIDRecord in the fw update
     *                                package that matches the firmware device
     * @param[in] compIndex - component index
     * @param[in] messageID - messageID string
     * @param[in] messageError - error indicating exact reason for failure. Ex:
     * background copy
     * @param resolution - resolution field for the message registry[Optional]
     */
    void createMessageRegistryResourceErrors(
        mctp_eid_t eid, const FirmwareDeviceIDRecord& fwDeviceIDRecord,
        size_t compIndex, const std::string& messageID,
        const std::string& messageError, const std::string& resolution);

    const std::string swRootPath{"/xyz/openbmc_project/software/"};
    Event& event; //!< reference to PLDM daemon's main event loop
    /** @brief PLDM request handler */
    pldm::requester::Handler<pldm::requester::Request>& handler;
    Requester& requester; //!< reference to Requester object

    /**
     * @brief Create a Activation Object object
     *
     * @return bool true if successfully created
     */
    bool createActivationObject();

    /**
     * @brief Callback to be called by other device manager to signal that all
     *        other devices are ready for the activation object to be created
     *
     * @param otherDeviceMap Map of UUID to boolean indicating if update
     *                       initialization was successful.
     */
    void updateOtherDeviceComponents(
        std::unordered_map<std::string, bool>& otherDeviceMap);

    /**
     * @brief Callback to indicate that an other device has completed updating
     *
     * @param uuid UUID of the other device
     * @param status true if successful, false if failed
     * @param successCompNames - Name of components successfully updated
     */
    void updateOtherDeviceCompletion(std::string uuid, bool status,
                                     const ComponentName& successCompName = {});

    /**
     * @brief Checks that the completion map is full and if there were any
     *        failures.
     *
     * @tparam T type for completion map ID
     * @param nDevices number of expected devices
     * @param completionMap map of devices to completion status (false =
     * failure)
     * @return auto Active if all updates successful, Activating if map not
     * full, Failed if one failed.
     */
    template <class T>
    auto checkUpdateCompletionMap(size_t nDevices,
                                  std::unordered_map<T, bool>& completionMap)
    {
        namespace software = sdbusplus::xyz::openbmc_project::Software::server;
        if (nDevices == completionMap.size())
        {
            /* verify nothing failed */
            for (const auto& [id, status] : completionMap)
            {
                if (!status)
                {
                    return software::Activation::Activations::Failed;
                    break;
                }
            }
            return software::Activation::Activations::Active;
        }
        return software::Activation::Activations::Activating;
    }

    /** @brief PLDM package can consist of PLDM devices and non-pldm devices and
     *         this function checks the completion status of both set of devices
     *         and updates the Software.Activation interface accordingly. If all
     *         the devices are updated successfully Activation is set to
     *         Activations.Active, otherwise Activations.Failed
     */
    void updatePackageCompletion();

    /**
     * @brief reset activation block transition to disable bmc reboot guard
     *
     */
    void resetActivationBlocksTransition();

    /**
     * @brief Remove firmware update package from update directory
     *
     */
    void clearFirmwareUpdatePackage();

    bool fwDebug;
    /**
     * @brief start pldm firmware update
     *
     */
    void startPLDMUpdate();
    /**
     * @brief start non-pldm firmware update
     *
     * @return software::Activation::Activations
     */
    software::Activation::Activations startNonPLDMUpdate();
    /**
     * @brief Set activation status
     *
     * @param[in] state - activation state
     */
    void setActivationStatus(const software::Activation::Activations& state);

    /**
     * @brief Get the component name corresponding to the input params
     *
     * @param[in] eid - Remote MCTP Endpoint ID
     * @param[in] fwDeviceIDRecord - FirmwareDeviceIDRecord in the fw update
     *                                package that matches the firmware device
     * @param[in] compIndex - component index
     *
     * @return On success return the component name and empty stricng on no
     *         match
     */
    ComponentName
        getComponentName(mctp_eid_t eid,
                         const FirmwareDeviceIDRecord& fwDeviceIDRecord,
                         size_t compIndex);

    bool verifyPackage();

  private:
    /** @brief Device identifiers of the managed FDs */
    const DescriptorMap& descriptorMap;
    /** @brief Component information needed for the update of the managed FDs */
    const ComponentInfoMap& componentInfoMap;
    /** @brief Component information needed for the update of the managed FDs */
    const ComponentNameMap& componentNameMap;
    Watch watch;
    std::unique_ptr<Activation> activation;
    std::unique_ptr<ActivationProgress> activationProgress;
    std::unique_ptr<ActivationBlocksTransition> activationBlocksTransition;
    std::unique_ptr<UpdatePolicy> updatePolicy;
    std::string objPath;

    std::filesystem::path fwPackageFilePath;
    std::unique_ptr<PackageParser> parser;
    std::ifstream package;

    std::unordered_map<mctp_eid_t, std::unique_ptr<DeviceUpdater>>
        deviceUpdaterMap;
    std::unordered_map<mctp_eid_t, bool> deviceUpdateCompletionMap;

    /* for other devices associated UUID maps to if it has prepared the
       activation interface */
    std::unordered_map<std::string, bool> otherDeviceComponents;
    /* UUID -> update completed successfully map for other devices */
    std::unordered_map<std::string, bool> otherDeviceCompleted;
    FirmwareDeviceIDRecords fwDeviceIDRecords;

    /** @brief Total number of component updates to calculate the progress of
     *         the Firmware activation
     */
    size_t totalNumComponentUpdates;
    /** @brief FW update package can contain updates for multiple firmware
     *         devices and each device can have multiple components. Once
     *         each component is updated (Transfer completed, Verified and
     *         Applied) ActivationProgress is updated.
     */
    size_t compUpdateCompletedCount;
    decltype(std::chrono::steady_clock::now()) startTime;

    std::unique_ptr<OtherDeviceUpdateManager> otherDeviceUpdateManager;
#ifdef OEM_NVIDIA
    std::unique_ptr<DebugToken> debugToken;
#endif

    /** @brief List of components successfully updated. The component names are
     *         separated by space and to be published in the summary log.
     */
    std::string listCompNames;

    /**
     * @brief timer to update progress percent
     *
     */
    std::unique_ptr<phosphor::Timer> progressTimer;
    /**
     * @brief Counter to keep track of update progress interval
     *
     */
    uint8_t updateInterval;

    /**
     * @brief Total intervals to update progress percent
     *
     */
    uint8_t totalInterval = static_cast<uint8_t>(
        std::floor((FIRMWARE_UPDATE_TIME / PROGRESS_UPDATE_INTERVAL)));

    /**
     * @brief Create a Progress Update Timer. This timer updates progress
     * percent at regular interval based on firmware-update-time and
     * progress-percent-updater-interval options.
     *
     */
    void createProgressUpdateTimer();
};

} // namespace fw_update

} // namespace pldm
