#pragma once

#include "libpldm/base.h"
#include "libpldm/requester/pldm.h"

#include "common/types.hpp"
#include "device_updater.hpp"
#include "package_parser.hpp"
#include "pldmd/dbus_impl_requester.hpp"
#include "requester/handler.hpp"
#include "watch.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <tuple>
#include <unordered_map>

#include "other_device_update_manager.hpp"

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

class UpdateManager
{
  public:
    UpdateManager() = delete;
    UpdateManager(const UpdateManager&) = delete;
    UpdateManager(UpdateManager&&) = delete;
    UpdateManager& operator=(const UpdateManager&) = delete;
    UpdateManager& operator=(UpdateManager&&) = delete;
    ~UpdateManager() = default;

    explicit UpdateManager(
        Event& event,
        pldm::requester::Handler<pldm::requester::Request>& handler,
        Requester& requester, const DescriptorMap& descriptorMap,
        const ComponentInfoMap& componentInfoMap) :
        event(event),
        handler(handler), requester(requester), descriptorMap(descriptorMap),
        componentInfoMap(componentInfoMap),
        watch(event.get(),
              std::bind_front(&UpdateManager::processPackage, this))
    {}

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

    void updateDeviceCompletion(mctp_eid_t eid, bool status);

    /**
     * @brief Increments completed updates and refreshes the reported progress
     *
     */
    void updateActivationProgress();

    /** @brief Callback function that will be invoked when the
     *         RequestedActivation will be set to active in the Activation
     *         interface
     * @return returns true if package activation starts, false otherwise
     */
    bool activatePackage();

    void clearActivationInfo();

    /** @brief
     *
     */
    DeviceUpdaterInfos
        associatePkgToDevices(const FirmwareDeviceIDRecords& fwDeviceIDRecords,
                              const DescriptorMap& descriptorMap,
                              TotalComponentUpdates& totalNumComponentUpdates);

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
    void updateOtherDeviceComponents(std::unordered_map<std::string, bool> &otherDeviceMap);

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
     * @param completionMap map of devices to completion status (false = failure)
     * @return auto Active if all updates successful, Activating if map not full, Failed if one failed.
     */
    template<class T>
    auto checkUpdateCompletionMap(size_t nDevices,
            std::unordered_map<T, bool> &completionMap) {
        namespace software = sdbusplus::xyz::openbmc_project::Software::server;
        if (nDevices == completionMap.size()) {
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

  private:
    /** @brief Device identifiers of the managed FDs */
    const DescriptorMap& descriptorMap;
    /** @brief Component information needed for the update of the managed FDs */
    const ComponentInfoMap& componentInfoMap;
    Watch watch;

    std::unique_ptr<Activation> activation;
    std::unique_ptr<ActivationProgress> activationProgress;
    std::string objPath;

    std::filesystem::path fwPackageFilePath;
    std::unique_ptr<PackageParser> parser;
    std::ifstream package;

    std::unordered_map<mctp_eid_t, std::unique_ptr<DeviceUpdater>>
        deviceUpdaterMap;
    std::unordered_map<mctp_eid_t, bool> deviceUpdateCompletionMap;

    /* for other devices associated UUID maps to if it has prepared the activation
       interface */
    std::unordered_map<std::string, bool> otherDeviceComponents;
    /* UUID -> update completed successfully map for other devices */
    std::unordered_map<std::string, bool> otherDeviceCompleted;

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
