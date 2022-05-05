#pragma once

#include "libpldm/base.h"
#include "libpldm/requester/pldm.h"

#include "common/types.hpp"
#include "device_updater.hpp"
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

namespace pldm
{

namespace fw_update
{

using namespace sdeventplus;
using namespace sdeventplus::source;
using namespace pldm::dbus_api;
using namespace pldm;

using DeviceIDRecordOffset = size_t;
using DeviceUpdaterInfo = std::pair<mctp_eid_t, DeviceIDRecordOffset>;
using DeviceUpdaterInfos = std::vector<DeviceUpdaterInfo>;
using TotalComponentUpdates = size_t;

class Activation;
class ActivationProgress;
class UpdatePolicy;

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
        ComponentNameMap& componentNameMap,
        const ComponentSkipList& compSkipList);

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
     */
    void updateDeviceCompletion(mctp_eid_t eid, bool status);

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
    inline auto activatePackage()
    {
        namespace software = sdbusplus::xyz::openbmc_project::Software::server;
        // In case no device found set activation stage to active to complete
        // task.
        if ((deviceUpdaterMap.size() == 0) &&
            (otherDeviceUpdateManager->getNumberOfProcessedImages() == 0))
        {
            std::cout
                << "Nothing to activate, Setting Activations state to Active!\n";
            return software::Activation::Activations::Active;
        }

        startTime = std::chrono::steady_clock::now();
        for (const auto& [eid, deviceUpdaterPtr] : deviceUpdaterMap)
        {
            const auto& applicableComponents = std::get<ApplicableComponents>(
                deviceUpdaterPtr->fwDeviceIDRecord);
            for (size_t compIndex = 0; compIndex < applicableComponents.size();
                 compIndex++)
            {
                createMessageRegistry(eid, deviceUpdaterPtr->fwDeviceIDRecord,
                                      compIndex, targetDetermined);
            }
            deviceUpdaterPtr->startFwUpdateFlow();
        }
        // Initiate the activate of non-pldm
        if (!otherDeviceUpdateManager->activate())
        {
            return software::Activation::Activations::Failed;
        }
        return software::Activation::Activations::Activating;
    }

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
     *  @param[in] componentSkipList - List of components to skip
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
        const ComponentSkipList& compSkipList,
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

    /** @brief Create the D-Bus log entry for message registry
     *
     *  @param[in] messageID - Message ID
     *  @param[in] compName - Component name
     *  @param[in] compVersion - Component version
     *  @param[in] resolution - Resolution field
     */
    void createLogEntry(const std::string& messageID,
                        const std::string& compName,
                        const std::string& compVersion,
                        const std::string& resolution);

    /** @brief Create message registry for firmware update
     *
     *  @param[in] eid - Remote MCTP Endpoint ID
     *  @param[in] fwDeviceIDRecord - FirmwareDeviceIDRecord in the fw update
     *                                package that matches the firmware device
     *  @param[in] compIndex - component index
     *  @param[in] messageID - messageID string
     *  @param[in] resolution - resolution field for the message registry
     *                          (optional)
     */
    void createMessageRegistry(mctp_eid_t eid,
                               const FirmwareDeviceIDRecord& fwDeviceIDRecord,
                               size_t compIndex, const std::string& messageID,
                               const std::string& resolution = {});

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
     */
    void updateOtherDeviceCompletion(std::string uuid, bool status);

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

    const std::string transferFailed{"Update.1.0.TransferFailed"};
    const std::string transferringToComponent{
        "Update.1.0.TransferringToComponent"};
    const std::string verificationFailed{"Update.1.0.VerificationFailed"};
    const std::string updateSuccessful{"Update.1.0.UpdateSuccessful"};
    const std::string awaitToActivate{"Update.1.0.AwaitToActivate"};
    const std::string applyFailed{"Update.1.0.ApplyFailed"};
    const std::string activateFailed{"Update.1.0.ActivateFailed"};
    const std::string targetDetermined{"Update.1.0.TargetDetermined"};

  private:
    /** @brief Device identifiers of the managed FDs */
    const DescriptorMap& descriptorMap;
    /** @brief Component information needed for the update of the managed FDs */
    const ComponentInfoMap& componentInfoMap;
    /** @brief Component information needed for the update of the managed FDs */
    const ComponentNameMap& componentNameMap;
    /** @brief Component skip list */
    const ComponentSkipList& compSkipList;
    Watch watch;

    std::unique_ptr<Activation> activation;
    std::unique_ptr<ActivationProgress> activationProgress;
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
};

} // namespace fw_update

} // namespace pldm
