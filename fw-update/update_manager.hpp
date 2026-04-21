/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include "common/instance_id.hpp"
#include "common/types.hpp"
#include "device_updater.hpp"
#include "fw-update/update.hpp"
#include "other_device_update_manager.hpp"

#ifdef FW_UPDATE_INOTIFY_ENABLED
#include "fw-update/watch.hpp"
#endif

#include "package_parser.hpp"
#include "package_signature.hpp"
#include "requester/handler.hpp"

#include <libpldm/base.h>
#include <libpldm/pldm.h>

#include <xyz/openbmc_project/Software/ApplyTime/server.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <tuple>
#include <unordered_map>
#include <vector>

#ifdef OEM_NVIDIA
#include "oem-nvidia/debug_token.hpp"
#endif

namespace pldm
{

namespace fw_update
{

using namespace sdeventplus;
using namespace sdeventplus::source;
using namespace pldm;
namespace software = sdbusplus::xyz::openbmc_project::Software::server;

using DeviceIDRecordOffset = size_t;
using DeviceUpdaterInfo = std::pair<mctp_eid_t, DeviceIDRecordOffset>;
using DeviceUpdaterInfos = std::vector<DeviceUpdaterInfo>;
using TotalComponentUpdates = size_t;
using DescriptorMatchCount = size_t;
using RefreshSingleEndpointCallback =
    std::function<exec::task<int>(mctp_eid_t, bool)>;

class Activation;
class ActivationProgress;
class ActivationBlocksTransition;

/** @enum Enumeration to represent the types of security checks
 */
enum class SecurityCheckType
{
    Disabled,
    Integrity,
    Authentication
};

class UpdateManager
{
  public:
    UpdateManager() = delete;
    UpdateManager(const UpdateManager&) = delete;
    UpdateManager(UpdateManager&&) = delete;
    UpdateManager& operator=(const UpdateManager&) = delete;
    UpdateManager& operator=(UpdateManager&&) = delete;
    virtual ~UpdateManager();

    explicit UpdateManager(
        Event& event,
        pldm::requester::Handler<pldm::requester::Request>& handler,
        InstanceIdDb& instanceIdDb, const DescriptorMap& descriptorMap,
        const ComponentInfoMap& componentInfoMap,
        ComponentNameMap& componentNameMap, bool fwDebug,
        RefreshSingleEndpointCallback refreshSingleEndpointCallback);

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
    virtual Response handleRequest(mctp_eid_t eid, uint8_t command,
                                   const pldm_msg* request, size_t reqMsgLen);

    int processPackage(const std::filesystem::path& packageFilePath);

    /** @brief Notify that a FW update response was sent (or failed).
     *         Delegates to the DeviceUpdater for the given EID so that
     *         post-response actions (e.g. GetStatus) can be triggered
     *         only after confirmed delivery.
     *
     *  @param[in] eid - Remote MCTP Endpoint ID
     *  @param[in] success - true if sendMsg succeeded
     */
    void onResponseSendComplete(mctp_eid_t eid, bool success);

    /** @brief Process the firmware update package stream
     *
     *  Parses the firmware package header, validates the package format,
     *  associates firmware components with target devices, and initiates
     *  the update process for matched devices.
     *
     *  @param[in] packageStream - Input file stream containing the firmware
     *                             package data
     *  @param[in] packageSize - Total size of the firmware package in bytes
     *  @param[in] targets - Optional list of specific target components to
     *                       update. Empty list means update all compatible
     *                       components.
     */
    exec::task<void> processStream(
        std::istream& packageStream, uintmax_t packageSize,
        std::vector<sdbusplus::message::object_path> targets = {});

    /** @brief Defers processing of the package stream to the event loop
     *
     *  Creates the Update D-Bus interface and schedules the actual package
     *  processing asynchronously. This allows the D-Bus method call to
     *  return immediately while processing continues in the background.
     *
     *  @param[in] packageStream - Input file stream containing the firmware
     *                             package data
     *  @param[in] packageSize - Total size of the firmware package in bytes
     *  @param[in] forceUpdate - If true, bypasses version checks and forces
     *                           the update even if target has same/newer
     *                           version
     *  @param[in] targets - Optional list of specific target components to
     *                       update
     *
     *  @return D-Bus object path of the created Software update object
     */
    std::string processStreamDefer(
        std::istream& packageStream, uintmax_t packageSize, bool forceUpdate,
        std::vector<sdbusplus::message::object_path> targets);

    /** @brief Set the RequestedApplyTime for the current update session
     *
     *  @param[in] applyTime - The requested apply time from StartUpdate
     */
    void setRequestedApplyTime(
        sdbusplus::xyz::openbmc_project::Software::server::ApplyTime::
            RequestedApplyTimes applyTime)
    {
        requestedApplyTime = applyTime;
    }

    /** @brief Check if RequestedApplyTime is set to Immediate
     *
     *  @return true if apply time is Immediate, false otherwise
     */
    bool isApplyTimeImmediate() const
    {
        return requestedApplyTime ==
               sdbusplus::xyz::openbmc_project::Software::server::ApplyTime::
                   RequestedApplyTimes::Immediate;
    }

    /** @brief Update firmware update completion status of each device
     *
     *  @param[in] eid - Remote MCTP Endpoint ID
     *  @param[in] status - True to indicate success and false for failure
     *  @param[in] successCompNames - Name of components successfully updated
     */
    void updateDeviceCompletion(
        mctp_eid_t eid, bool status,
        const std::vector<ComponentName>& successCompNames = {});

    /** @brief Increments completed updates and refreshes the reported progress
     */
    void updateActivationProgress();

    /** @brief Refresh activation progress using per-device progress data
     */
    void refreshActivationProgress();

    /** @brief Callback function that will be invoked when the
     *         RequestedActivation will be set to active in the Activation
     *         interface
     *         Handles activation for PLDM and non PLDM devices, in case
     *         of no device detections we will set default to Active state
     * @return returns Activations state
     */
    software::Activation::Activations activatePackage();

    void clearActivationInfo();

    /** @brief Get the list of component targets for each EID based on object
     *         paths
     */
    ComponentTargetList getComponentTargetList(
        const ComponentNameMap& componentNameMap,
        const std::vector<sdbusplus::message::object_path>& objectPaths);

    /** @brief Associate firmware update package to devices and components that
     *         will be updated
     */
    DeviceUpdaterInfos associatePkgToDevices(
        const FirmwareDeviceIDRecords& inFwDeviceIDRecords,
        const DescriptorMap& descriptorMap,
        const ComponentImageInfos& compImageInfos,
        const ComponentTargetList& compTargetList,
        const std::vector<sdbusplus::message::object_path>& objectPaths,
        FirmwareDeviceIDRecords& outFwDeviceIDRecords,
        TotalComponentUpdates& totalNumComponentUpdates);

    /** @brief Translate the RequestedComponentActivationMethod in PLDM spec to
     *         a human readable string
     */
    std::string getActivationMethod(bitfield16_t compActivationModification);

    /** @brief Create message registry for firmware update
     */
    void createMessageRegistry(
        mctp_eid_t eid, const FirmwareDeviceIDRecord& fwDeviceIDRecord,
        size_t compIndex, const std::string& messageID,
        const std::string& resolution = {},
        const pldm_firmware_update_commands commandType =
            static_cast<pldm_firmware_update_commands>(0),
        const uint8_t errorCode = 0);

    /** @brief Create a Message Registry for Resource Errors
     */
    void createMessageRegistryResourceErrors(
        mctp_eid_t eid, const FirmwareDeviceIDRecord& fwDeviceIDRecord,
        size_t compIndex, const std::string& messageID,
        const std::string& messageError, const std::string& resolution);

    /** @brief Generate a unique software ID based on current timestamp
     */
    static std::string getSwId();

    const std::string swRootPath{"/xyz/openbmc_project/software/"};
    Event& event;
    pldm::requester::Handler<pldm::requester::Request>& handler;
    InstanceIdDb& instanceIdDb;

    /** @brief Callback to be called by other device manager to signal that all
     *        other devices are ready for the activation object to be created
     */
    void updateOtherDeviceComponents(
        std::unordered_map<std::string, bool>& otherDeviceMap);

    /** @brief Callback to indicate that an other device has completed updating
     */
    void updateOtherDeviceCompletion(std::string uuid, bool status,
                                     const ComponentName& successCompName = {});

    /**
     * @brief Checks that the completion map is full and if there were any
     *        failures.
     */
    template <class T>
    auto checkUpdateCompletionMap(size_t nDevices,
                                  std::unordered_map<T, bool>& completionMap)
    {
        namespace software = sdbusplus::xyz::openbmc_project::Software::server;
        if (nDevices == completionMap.size())
        {
            for (const auto& [id, status] : completionMap)
            {
                if (!status)
                {
                    return software::Activation::Activations::Failed;
                }
            }
            return software::Activation::Activations::Active;
        }
        return software::Activation::Activations::Activating;
    }

    /** @brief PLDM package can consist of PLDM devices and non-pldm devices and
     *         this function checks the completion status of both set of devices
     *         and updates the Software.Activation interface accordingly.
     */
    void updatePackageCompletion();

    /** @brief reset activation block transition to disable bmc reboot guard */
    void resetActivationBlocksTransition();

    /** @brief Clear the firmware update package stream and free resources */
    void clearFirmwareUpdatePackage();

    /** @brief Stores the force update flag set on update policy */
    bool forceUpdate;

    bool fwDebug;

    /** @brief start pldm firmware update */
    void startPLDMUpdate();

    /** @brief start non-pldm firmware update */
    software::Activation::Activations startNonPLDMUpdate();

    /** @brief Set activation status */
    void setActivationStatus(const software::Activation::Activations& state);

    /** @brief Get the component name corresponding to the input params */
    ComponentName getComponentName(
        mctp_eid_t eid, const FirmwareDeviceIDRecord& fwDeviceIDRecord,
        size_t compIndex);

    /** @brief performs package verification checks asynchronously */
    void verifyPackageAsync(
        std::function<void(bool)> onComplete,
        std::function<void(const std::string& errorMsg)> onError);

    /** @brief integrity check of firmware package */
    void packageIntegrityCheckAsync(
        std::function<void(bool)> onComplete,
        std::function<void(const std::string& errorMsg)> onError);

    /** @brief perform security checks */
    void performSecurityChecksAsync(
        std::function<void(bool)> onComplete,
        std::function<void(const std::string& errorMsg)> onError);

    /** @brief Clear any existing activation if present */
    void clearExistingActivation();

    std::unique_ptr<PackageSignature> packageSignatureParser;

    /** @brief Callback to refresh a single endpoint's descriptors */
    RefreshSingleEndpointCallback refreshSingleEndpointCallback;

  private:
    sdbusplus::xyz::openbmc_project::Software::server::ApplyTime::
        RequestedApplyTimes requestedApplyTime;

    const DescriptorMap& descriptorMap;
    const ComponentInfoMap& componentInfoMap;
    const ComponentNameMap& componentNameMap;

    std::unique_ptr<Activation> activation;
#ifdef FW_UPDATE_INOTIFY_ENABLED
    Watch watch;
#else
    std::unique_ptr<Update> updater;
#endif
    std::unique_ptr<ActivationProgress> activationProgress;
    std::unique_ptr<ActivationBlocksTransition> activationBlocksTransition;
    std::string objPath;

    std::unique_ptr<PackageParser> parser;

    std::unordered_map<mctp_eid_t, std::unique_ptr<DeviceUpdater>>
        deviceUpdaterMap;
    std::unordered_map<mctp_eid_t, bool> deviceUpdateCompletionMap;

    std::unordered_map<std::string, bool> otherDeviceComponents;
    std::unordered_map<std::string, bool> otherDeviceCompleted;
    FirmwareDeviceIDRecords fwDeviceIDRecords;

    size_t totalNumComponentUpdates = 0;
    size_t compUpdateCompletedCount = 0;
    decltype(std::chrono::steady_clock::now()) startTime;

    std::unique_ptr<OtherDeviceUpdateManager> otherDeviceUpdateManager;
#ifdef OEM_NVIDIA
    std::unique_ptr<DebugToken> debugToken;
#endif

    /** @brief List of components successfully updated for summary logging */
    std::string listCompNames;

    /** @brief timer to update progress percent */
    std::unique_ptr<sdbusplus::Timer> progressTimer;

    /** @brief Counter to keep track of update progress interval */
    uint8_t updateInterval;

    /** @brief Total intervals to update progress percent */
    uint8_t totalInterval = static_cast<uint8_t>(
        std::floor((FIRMWARE_UPDATE_TIME / PROGRESS_UPDATE_INTERVAL)));

    /** @brief Create a Progress Update Timer */
    void createProgressUpdateTimer();

    /** @brief Defer handler for update */
    std::unique_ptr<sdeventplus::source::Defer> updateDeferHandler;

    /** @brief Handle invalid firmware package error by logging and setting
     *         activation state
     */
    void handleInvalidPackageError();

    /** @brief Handle Payload checksum validation failure by logging and setting
     *         the activation state
     */
    void handlePayloadChecksumError();

    /** @brief Handle header parse failures by logging and setting the
     *         activation state
     */
    void handleInvalidPackageHeaderError();

    /** @brief Log resource errors for all components when multiple descriptor
     *         records in the package match the same device
     */
    void handleDuplicateDescriptorMatch(
        mctp_eid_t eid, const FirmwareDeviceIDRecord& fwDeviceIDRecord);

    /** @brief The last progress that was calculated. Used to avoid spamming
     *         D-Bus updates.
     */
    uint8_t lastProgress = 0;
};

} // namespace fw_update

} // namespace pldm
