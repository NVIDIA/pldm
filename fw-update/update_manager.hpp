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
#include "common/mmap_stream.hpp"
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
#include <functional>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
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

/** @brief Refresh one endpoint's firmware inventory.
 *
 *  Arguments: (eid, isTarget, preUpdateValidation). isTarget logs a
 *  refresh failure at error (rather than warning) severity;
 *  preUpdateValidation emits the existing failure messages at
 *  Critical severity, since the failure rejects the whole request.
 */
using RefreshSingleEndpointCallback =
    std::function<exec::task<int>(mctp_eid_t, bool, bool)>;

/** @brief Default (empty) expected-component map for callers that construct
 *         an UpdateManager without a parsed firmware update config.
 */
inline const ExpectedComponentIdsByEid emptyExpectedComponentIds{};

/** @brief Whether an EID is excluded from PLDM firmware update by
 *         entity-manager configuration (see
 *         Manager::isEidExcludedFromFwUpdate()).
 */
using IsEidExcludedCallback = std::function<bool(mctp_eid_t)>;

/** @brief Resolve an endpoint to its entity-manager device name (the
 *         MCTPTargetName join key); empty when not resolvable.
 */
using TargetNameCallback = std::function<std::string(mctp_eid_t)>;

/** @brief The names of the configured, non-excluded PLDM firmware devices
 *         with updatable components (see
 *         em_config::fetchConfiguredFirmwareTargets()).
 */
using ConfiguredTargetsCallback = std::function<std::set<std::string>()>;

/** @brief Device names of the statically addressed MCTP transport
 *         configurations, keyed by StaticEndpointID (see
 *         em_config::fetchStaticEidTargetNames()).
 */
using StaticEidTargetsCallback =
    std::function<std::map<mctp_eid_t, std::set<std::string>>()>;

class Activation;
class ActivationProgress;
class ActivationBlocksTransition;

class UpdateManagerBase
{
  public:
    virtual ~UpdateManagerBase() = default;

    UpdateManagerBase() = delete;
    UpdateManagerBase(const UpdateManagerBase&) = delete;
    UpdateManagerBase(UpdateManagerBase&&) = delete;
    UpdateManagerBase& operator=(const UpdateManagerBase&) = delete;
    UpdateManagerBase& operator=(UpdateManagerBase&&) = delete;

    UpdateManagerBase(
        Event& event,
        pldm::requester::Handler<pldm::requester::Request>& handler,
        InstanceIdDb& instanceIdDb, bool fwDebug = false) :
        fwDebug(fwDebug), event(event), handler(handler),
        instanceIdDb(instanceIdDb)
    {}

    virtual void updateDeviceCompletion(
        mctp_eid_t eid, bool status,
        std::vector<std::string> compNames = {}) = 0;
    virtual void markComponentUpdateCompleted() = 0;
    virtual software::Activation::Activations activatePackage() = 0;
    virtual void resetActivationState() = 0;

    virtual bool isApplyTimeImmediate() const
    {
        return false;
    }
    virtual std::string getActivationMethod(
        bitfield16_t /*compActivationModification*/)
    {
        return {};
    }
    virtual ComponentName getComponentName(
        mctp_eid_t /*eid*/, const FirmwareDeviceIDRecord& /*fwDeviceIDRecord*/,
        size_t /*compIndex*/)
    {
        return {};
    }
    virtual void createMessageRegistry(
        mctp_eid_t /*eid*/, const FirmwareDeviceIDRecord& /*fwDeviceIDRecord*/,
        size_t /*compIndex*/, const std::string& /*messageID*/,
        const std::string& /*resolution*/ = {},
        const pldm_firmware_update_commands /*commandType*/ =
            static_cast<pldm_firmware_update_commands>(0),
        const uint8_t /*errorCode*/ = 0)
    {}
    virtual void createMessageRegistryResourceErrors(
        mctp_eid_t /*eid*/, const FirmwareDeviceIDRecord& /*fwDeviceIDRecord*/,
        size_t /*compIndex*/, const std::string& /*messageID*/,
        const std::string& /*messageError*/, const std::string& /*resolution*/,
        bool /*overrideSeverity*/ = false)
    {}
    virtual void resetActivationBlocksTransition() {}
    virtual void clearFirmwareUpdatePackage() {}
    virtual void clearActivationInfo() {}
    virtual void performSecurityChecksAsync(
        std::function<void(bool)> /*onComplete*/,
        std::function<void(const std::string&)> /*onError*/)
    {}

    bool fwDebug = false;
    bool forceUpdate = false;
    RefreshSingleEndpointCallback refreshSingleEndpointCallback;

    Event& event;
    pldm::requester::Handler<pldm::requester::Request>& handler;
    InstanceIdDb& instanceIdDb;
};

/** @enum Enumeration to represent the types of security checks
 */
enum class SecurityCheckType
{
    Disabled,
    Integrity,
    Authentication
};

class UpdateManager : public UpdateManagerBase
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
        RefreshSingleEndpointCallback refreshSingleEndpointCallback,
        const ExpectedComponentIdsByEid& expectedComponentIdsByEid =
            emptyExpectedComponentIds,
        IsEidExcludedCallback isEidExcludedCallback = {},
        TargetNameCallback targetNameCallback = {},
        ConfiguredTargetsCallback configuredTargetsCallback = {},
        StaticEidTargetsCallback staticEidTargetsCallback = {});

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

    /** @brief Process the firmware update package data
     *
     *  Parses the firmware package header, validates the package format,
     *  associates firmware components with target devices, and initiates
     *  the update process for matched devices.
     *
     *  @param[in] packageData - Pointer to the firmware package data in
     *                           memory
     *  @param[in] packageSize - Total size of the firmware package in bytes
     *  @param[in] targets - Optional list of specific target components to
     *                       update. Empty list means update all compatible
     *                       components.
     */
    exec::task<void> processPackageData(
        const uint8_t* packageData, size_t packageSize,
        std::vector<sdbusplus::object_path> targets = {},
        bool preUpdateValidation = false);

    /** @brief Defers processing of the package data to the event loop
     *
     *  Creates the Update D-Bus interface and schedules the actual package
     *  processing asynchronously. This allows the D-Bus method call to
     *  return immediately while processing continues in the background.
     *
     *  @param[in] packageData - Pointer to the firmware package data in
     *                           memory
     *  @param[in] packageSize - Total size of the firmware package in bytes
     *  @param[in] forceUpdate - If true, bypasses version checks and forces
     *                           the update even if target has same/newer
     *                           version
     *  @param[in] targets - Optional list of specific target components to
     *                       update
     *  @param[in] preUpdateValidation - If true, run the preUpdateValidation
     * readiness gate: reject the entire update before any transfer if any
     * configured device is unavailable
     *
     *  @return D-Bus object path of the created Software update object
     */
    std::string processPackageDataDefer(
        const uint8_t* packageData, size_t packageSize, bool forceUpdate,
        std::vector<sdbusplus::object_path> targets,
        bool preUpdateValidation = false);

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
    bool isApplyTimeImmediate() const override
    {
        return requestedApplyTime ==
               sdbusplus::xyz::openbmc_project::Software::server::ApplyTime::
                   RequestedApplyTimes::Immediate;
    }

    /** @brief Recompute and publish the AllowedPreUpdateValidation
     *         capability on the Software.Update interface.
     *
     *  True iff this BMC has a known updatable device scope: an endpoint in
     *  the live descriptorMap or a non-excluded statically-configured device
     *  EID from the MCTP transport config (the same scope processStream()
     *  gates). On develop this knowledge arrives asynchronously (EM
     *  configuration delivery, MCTP discovery), so the firmware update
     *  Manager calls this whenever that state changes, instead of the
     *  release-branch one-shot derivation from fw_update_config.json at
     *  Update construction.
     */
    void refreshAllowedPreUpdateValidation();

    /** @brief Update firmware update completion status of each device
     *
     *  @param[in] eid - Remote MCTP Endpoint ID
     *  @param[in] status - True to indicate success and false for failure
     *  @param[in] successCompNames - Name of components successfully updated
     */
    void updateDeviceCompletion(
        mctp_eid_t eid, bool status,
        std::vector<std::string> compNames = {}) override;

    /**
     * @brief Increments completed updates and stops the progress timer once
     *        every component update has completed
     *
     * Called when a component update finishes, whether it succeeded or failed,
     * so that a failed component still advances the count and the timer stops.
     */
    void markComponentUpdateCompleted() override;

    /** @brief Callback function that will be invoked when the
     *         RequestedActivation will be set to active in the Activation
     *         interface
     *         Handles activation for PLDM and non PLDM devices, in case
     *         of no device detections we will set default to Active state
     * @return returns Activations state
     */
    software::Activation::Activations activatePackage() override;

    void clearActivationInfo() override;

    /** @brief Get the list of component targets for each EID based on object
     *         paths
     */
    ComponentTargetList getComponentTargetList(
        const ComponentNameMap& componentNameMap,
        const std::vector<sdbusplus::object_path>& objectPaths);

    /** @brief Associate firmware update package to devices and components that
     *         will be updated
     */
    DeviceUpdaterInfos associatePkgToDevices(
        const FirmwareDeviceIDRecords& inFwDeviceIDRecords,
        const DescriptorMap& descriptorMap,
        const ComponentImageInfos& compImageInfos,
        const ComponentTargetList& compTargetList,
        const std::vector<sdbusplus::object_path>& objectPaths,
        FirmwareDeviceIDRecords& outFwDeviceIDRecords,
        TotalComponentUpdates& totalNumComponentUpdates);

    /** @brief Translate the RequestedComponentActivationMethod in PLDM spec to
     *         a human readable string
     */
    std::string getActivationMethod(
        bitfield16_t compActivationModification) override;

    /** @brief Create message registry for firmware update
     */
    void createMessageRegistry(
        mctp_eid_t eid, const FirmwareDeviceIDRecord& fwDeviceIDRecord,
        size_t compIndex, const std::string& messageID,
        const std::string& resolution = {},
        const pldm_firmware_update_commands commandType =
            static_cast<pldm_firmware_update_commands>(0),
        const uint8_t errorCode = 0) override;

    /** @brief Create a Message Registry for Resource Errors
     */
    void createMessageRegistryResourceErrors(
        mctp_eid_t eid, const FirmwareDeviceIDRecord& fwDeviceIDRecord,
        size_t compIndex, const std::string& messageID,
        const std::string& messageError, const std::string& resolution,
        bool overrideSeverity = false) override;

    /** @brief Emit an Info-severity ResourceErrorsDetected entry per
     *         applicable component when multiple package records match
     *         the same device.
     *
     *  The first matching record is used for the update; this records the
     *  duplicate in Redfish for visibility/audit without flagging it as a
     *  failure.
     *
     *  @param[in] eid - Remote MCTP Endpoint ID
     *  @param[in] fwDeviceIDRecord - duplicate (skipped) firmware device ID
     *                                record
     */
    void handleDuplicateDescriptorMatch(
        mctp_eid_t eid, const FirmwareDeviceIDRecord& fwDeviceIDRecord);

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
    void resetActivationBlocksTransition() override;

    /** @brief Clear the firmware update package stream and free resources */
    void clearFirmwareUpdatePackage() override;

    /** @brief preUpdateValidation flag for the current update session */
    bool preUpdateValidation{};

    /** @brief start pldm firmware update */
    void startPLDMUpdate();

    /** @brief start non-pldm firmware update */
    software::Activation::Activations startNonPLDMUpdate();

    /**
     * @brief Publish the final activation status once the message registry
     *        entries already queued on this connection have been emitted
     *
     * Entries reach a client only after phosphor-log-manager emits
     * InterfacesAdded for them, while activation state is observed directly on
     * this connection. Publishing from the reply keeps the final state, and
     * the completion percentage reported with it, behind the entries that
     * explain the outcome.
     *
     * @param[in] state - final activation state
     * @param[in] onPublished - optional work to run once the state is published
     */
    void publishFinalActivationStatus(software::Activation::Activations state,
                                      std::function<void()> onPublished = {});

    /** @brief Get the component name corresponding to the input params */
    ComponentName getComponentName(
        mctp_eid_t eid, const FirmwareDeviceIDRecord& fwDeviceIDRecord,
        size_t compIndex) override;

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
        std::function<void(const std::string& errorMsg)> onError) override;

    /** @brief Clear any existing activation if present */
    void resetActivationState() override;
    void clearExistingActivation();

    std::unique_ptr<PackageSignature> packageSignatureParser;

    /** @brief Callback to refresh a single endpoint's descriptors */
    RefreshSingleEndpointCallback refreshSingleEndpointCallback;

  private:
    /** @brief Record explicitly targeted endpoints that are no longer present
     *         in the live descriptor map after refresh.
     */
    void recordUnavailableTargetEids(const ComponentTargetList& compTargetList);

    /** @brief Emit a Critical ResourceErrorsDetected log entry for each
     *         user-requested PLDM target whose image is not in the package.
     *         Called right after associatePkgToDevices so the "no matching
     *         image" entries are dispatched before any PLDM transfer begins
     *         (and therefore before bmcweb's TaskStatus evaluation on the
     *         terminal Activation transition).
     *
     *  @param[in] deviceUpdaterInfos - Targets that will be scheduled for
     *                                  update; any requested target EID not
     *                                  in this set is treated as having no
     *                                  matching package image.
     */
    void logUnupdatedTargets(const DeviceUpdaterInfos& deviceUpdaterInfos);

    /** @brief Run the pre-update validation readiness gate.
     *
     *  Rejects the whole bundle before any transfer when a configured PLDM
     *  device is unreachable after the pre-update refresh or the package
     *  lacks an image for an expected component. On rejection the coverage
     *  and summary messages are emitted, the package is released, and
     *  Activation is published as Failed - the same terminal handling as
     *  the package checksum failure path. Whole-system only; non-PLDM
     *  (item-updater) components are outside the gate.
     *
     *  @param[in] configEids - The validated device scope: the pre-update
     *                           refresh set (live descriptorMap keys union
     *                           the static-config seed) minus excluded EIDs
     *  @param[in] deviceUpdaterInfos - Package-to-PLDM-device associations
     *  @param[in] unansweredTargets - Configured device names that no live
     *                                  endpoint answers for after the
     *                                  refresh (e.g. a device that left MCTP)
     *  @return true when validation passed and the update may proceed
     */
    bool runPreUpdateValidationGate(
        const std::vector<mctp_eid_t>& configEids,
        const DeviceUpdaterInfos& deviceUpdaterInfos,
        const std::set<std::string>& unansweredTargets = {});

    /** @brief Device names an endpoint answers for.
     *
     *  The configured_by-resolved name, plus the names of the static
     *  transport configurations at this EID - so a reachable endpoint whose
     *  configured_by association was not (re)published still answers for
     *  its device.
     *
     *  @param[in] eid - MCTP endpoint
     *  @param[in] staticTargets - StaticEndpointID → transport config Names
     *  @return the device names, empty when none resolves
     */
    std::set<std::string> namesForEid(
        mctp_eid_t eid,
        const std::map<mctp_eid_t, std::set<std::string>>& staticTargets) const;

    /** @brief Configured firmware devices that no endpoint in liveEids
     *         answers for.
     *
     *  @param[in] configuredTargets - Configured device names
     *  @param[in] liveEids - Endpoints reachable after the refresh
     *  @param[in] staticTargets - StaticEndpointID → transport config Names
     *  @return the configured names with no reachable endpoint
     */
    std::set<std::string> findUnansweredTargets(
        const std::set<std::string>& configuredTargets,
        const std::set<mctp_eid_t>& liveEids,
        const std::map<mctp_eid_t, std::set<std::string>>& staticTargets = {})
        const;

    /** @brief Drop EIDs excluded from PLDM firmware update from a
     *         pre-update validation device scope.
     *
     *  Only EIDs absent from descriptorMap are checked: Manager filters
     *  exclusions before discovery, so a discovered EID is never excluded,
     *  and this keeps the per-EID D-Bus exclusion lookup off the common
     *  path.
     *
     *  @param[in,out] scopeEids - device scope to filter
     */
    void eraseExcludedEids(std::set<mctp_eid_t>& scopeEids) const;

    /** @brief Requested apply time for the current update session */
    sdbusplus::xyz::openbmc_project::Software::server::ApplyTime::
        RequestedApplyTimes requestedApplyTime;

    const DescriptorMap& descriptorMap;
    const ComponentInfoMap& componentInfoMap;
    const ComponentNameMap& componentNameMap;

    /** @brief Expected (updatable) component identifiers per endpoint,
     *         derived from the entity-manager
     *         Configuration.PLDMFirmwareDevice.Components metadata;
     *         consumed by the pre-update validation gate */
    const ExpectedComponentIdsByEid& expectedComponentIdsByEid;

    /** @brief Exclusion check for the pre-update validation device scope.
     *
     *  descriptorMap never holds an excluded EID (Manager filters them
     *  before discovery), but the static-config seed can, so the scope is
     *  filtered through this before gating. Unset means nothing is excluded.
     */
    IsEidExcludedCallback isEidExcludedCallback;

    /** @brief EID → entity-manager device name, for the pre-update
     *         validation gate. Unset means names are not resolvable. */
    TargetNameCallback targetNameCallback;

    /** @brief Configured firmware devices the pre-update validation gate
     *         requires to be reachable, including devices no longer on
     *         MCTP. Unset means the scope is the discovered and statically
     *         configured endpoints only. */
    ConfiguredTargetsCallback configuredTargetsCallback;

    /** @brief StaticEndpointID → device names, the fallback that resolves a
     *         reachable static endpoint whose configured_by association is
     *         missing. Unset means no fallback. */
    StaticEidTargetsCallback staticEidTargetsCallback;

    /** @brief The static-EID names read for the current gated request, so
     *         the gate names an unreachable static endpoint the same way. */
    std::map<mctp_eid_t, std::set<std::string>> gateStaticTargets;

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

    /** @brief Memory-mapped firmware update package backing the file-path
     *         entry point (processPackage). Must be a member, not a local
     *         in processPackage(): DeviceUpdater retains the raw package
     *         pointer for the asynchronous RequestFirmwareData exchanges
     *         that follow package processing, so the mapping has to stay
     *         alive past processPackage()'s return. Released in
     *         clearFirmwareUpdatePackage(). */
    pldm::MmapFile packageMmapFile;

    std::unordered_map<mctp_eid_t, std::unique_ptr<DeviceUpdater>>
        deviceUpdaterMap;
    std::unordered_map<mctp_eid_t, bool> deviceUpdateCompletionMap;
    std::unordered_set<mctp_eid_t> unavailableTargetEids;

    /** @brief User-requested PLDM targets, keyed by the name supplied in the
     *         Redfish target path (preserved verbatim so the log entry
     *         references the exact component the user asked for rather than
     *         a differently-named component on the same EID). Each name is
     *         resolved to its owning PLDM EID once at processPackageData time
     *         from configured metadata, with the live componentNameMap as a
     *         fallback; names that don't resolve to a PLDM EID (e.g. non-PLDM
     *         targets) are intentionally omitted. Drained by
     *         logUnupdatedTargets() at terminal state.
     */
    std::unordered_map<std::string, mctp_eid_t> requestedTargets;

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

    /**
     * @brief Total intervals to update progress percent.
     *
     * Set per-update by activatePackage() based on
     * computeEffectiveTimeoutSec(); not const.
     */
    uint8_t totalInterval = 0;

    /**
     * @brief Per-package effective timeout in seconds. Lower-bounded by
     *        FIRMWARE_UPDATE_TIME, raised by the max UpdateTimeout
     *        advertised by descriptor-matched Item Updaters.
     *
     *        bmcweb's per-task watchdog is reset by every
     *        ActivationProgress.Progress signal, so the cadence of
     *        synthetic progress ticks (PROGRESS_UPDATE_INTERVAL) must
     *        stay smaller than BMCWEB_UPDATE_SERVICE_TASK_TIMEOUT
     *        regardless of how large this value grows.
     */
    uint64_t computeEffectiveTimeoutSec() const;

    /** @brief Create a Progress Update Timer */
    void createProgressUpdateTimer();

    /**
     * @brief Cancel all in-progress firmware updates due to timeout.
     *
     * This method is called when the firmware update timeout is reached.
     * It sends CancelUpdate requests to all devices that are still in-progress
     * (not in deviceUpdateCompletionMap) asynchronously and cleans up
     * resources.
     */
    void cancelAllUpdates();

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
};

} // namespace fw_update

} // namespace pldm
