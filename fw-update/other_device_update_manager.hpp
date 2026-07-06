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

#include "common/types.hpp"
#include "common/utils.hpp"

#include <exec/async_scope.hpp>
#include <exec/task.hpp>
#include <sdbusplus/async.hpp>
#include <sdbusplus/timer.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/Asset/server.hpp>
#include <xyz/openbmc_project/Software/Activation/server.hpp>
#include <xyz/openbmc_project/Software/ActivationProgress/server.hpp>

#include <unordered_map>

namespace pldm
{

namespace fw_update
{

namespace MatchRules = sdbusplus::bus::match::rules;

namespace Server = sdbusplus::xyz::openbmc_project::Software::server;

/* dead component identifier*/
const uint16_t deadComponent = 0xDEAD;

/** @enum TransferPackageState
 *
 * @brief Enumeration to represent the state of the last known image transfer
 *
 * @var FAILED indicates that the transfer of the package failed either due to a
 * truncated component or due to one of the component transfer image being
 * skipped or failing
 * @var SKIPPED indicates that the transfer of the package was skipped when the
 *              component is labelled as a deadcomponent
 * @var SUCCESS indicates the successful transfer of a package
 *
 *
 */
enum class TransferPackageState : uint8_t
{
    FAILED = 0x0,
    SKIPPED = 0x1,
    SUCCESS = 0x2,
};

/**
 * @brief Other device activation information used for storing the activation
 *        state of each of the non-pldm updates currently occuring. Mirrors
 *        the dbus state.
 *
 */
struct OtherDeviceUpdateActivation
{
    std::string uuid;
    Server::Activation::Activations activationState;
    Server::Activation::RequestedActivations requestedActivation;
};

/**
 * @brief ComponentMap contains version and component name. This information
 *        will be used in non pldm message registry mapping.
 */
struct ComponentMap
{
    std::string version;
    std::string componentName;
};

constexpr const uint8_t forceUpdateBit =
    0; // force update bit in component option

class UpdateManager;

/**
 * @brief Other Device manager
 *          Following are the funtionalities
 *          1) process pldm pkg and extract them to destination
 *          2) activate image
 *          3) Give progress state of the each of the other device
 */
class OtherDeviceUpdateManager
{
  public:
    /**
     * @brief Activation interface creation timeout in seconds
     *
     */
    auto static constexpr UPDATER_ACTIVATION_WAIT_PER_IMAGE_SEC = 3;

    OtherDeviceUpdateManager() = delete;
    OtherDeviceUpdateManager(const OtherDeviceUpdateManager&) = delete;
    OtherDeviceUpdateManager(OtherDeviceUpdateManager&&) = delete;
    OtherDeviceUpdateManager& operator=(const OtherDeviceUpdateManager&) =
        delete;
    OtherDeviceUpdateManager& operator=(OtherDeviceUpdateManager&&) = delete;

    /**
     * @brief Drain in-flight coroutines so suspended frames see `*this`
     *        alive through their full await chain (same pattern as
     *        DebugToken::tokenScope). Stop is requested first so nothing
     *        schedules further work; timeoutScope drains before
     *        completionScope because the completion coroutines await
     *        timeoutScope.on_empty().
     */
    ~OtherDeviceUpdateManager()
    {
        timeoutScope.request_stop();
        completionScope.request_stop();
        stdexec::sync_wait(timeoutScope.on_empty());
        stdexec::sync_wait(completionScope.on_empty());
    }

    /**
     * @brief Construct a new Other Device Update Manager object
     *
     * @param bus sdbusplus referance
     * @param createActivationObjectCallback call back to create the object
     */
    explicit OtherDeviceUpdateManager(
        sdbusplus::bus_t& bus, UpdateManager* upMan,
        std::vector<sdbusplus::object_path> targets,
        pldm::utils::DBusHandlerInterface& dbusHandler = defaultDbusHandler()) :
        updateManager(upMan), dbusHandler(dbusHandler), validTargetCount(0),
        bus(bus), timer(nullptr), targets(targets)
    {
        /* cache number of valid targets */
        updateValidTargets();
    }

    /**
     * @brief Activates all other devices
     */
    void activate();

    /**
     * @brief Async call to monitor the activate change in d-bus
     *
     * @param msg msg
     */
    void onActivationChangedMsg(sdbusplus::message::message& msg);

    /**
     * @brief parser activation message
     *
     * @param objPath path of the activation object
     * @param properties propertis of the activation object
     */
    void onActivationChanged(const std::string& objPath,
                             const pldm::dbus::PropertyMap& properties);

    /**
     * @brief Set the Update Policy object asynchronously.
     *        On failure, marks the image as not processed so the timeout
     *        handler can log a transfer failure.
     *
     * @param path - other software object path
     * @param uuid - UUID of the device (used to update isImageFileProcessed)
     */
    void setUpdatePolicy(const std::string& path, const std::string& uuid);
    /**
     * @brief method to add the dbus activation object paths to dbus watch
     *
     * @param m message
     */
    void interfaceAdded(sdbusplus::message::message& m);

    /**
     * @brief From pldm image extracts the other device images and copies to
     * respective location. Drives D-Bus property reads asynchronously.
     *
     * @param fwDeviceIDRecords - Device records
     * @param componentImageInfos - Image info like offset, size
     * @param package - pldm image input stream
     * @return exec::task yielding the number of other device images
     */
    exec::task<size_t> extractOtherDevicePkgs(
        const FirmwareDeviceIDRecords& fwDeviceIDRecords,
        const ComponentImageInfos& componentImageInfos, std::istream& package);

    /**
     * @brief Get the Number Of Processed Images object
     *
     * @return int
     */
    int getNumberOfProcessedImages();

    /**
     * @brief Get the number of valid UUIDs for non-pldm updates
     *
     * @return count of valid targets
     *
     */
    size_t getValidTargets(void);

    /**
     * @brief Return the cached max UpdateTimeout (seconds) advertised by the
     *        Item Updaters processing the current package. The cache is folded
     *        asynchronously in interfaceAdded() as each Item Updater publishes
     *        its activation object, so this synchronous getter never blocks
     *        the event loop.
     *
     * @return 0 if no Item Updater publishes the property; caller falls
     *         back to FIRMWARE_UPDATE_TIME.
     */
    uint64_t getMaxItemUpdaterTimeoutSec() const;

  private:
    static pldm::utils::DBusHandlerInterface& defaultDbusHandler();

    /**
     * @brief Start timer for interface addition
     *
     */
    void startTimer(int timerExpiryTime);

    /**
     * @brief This method queries all valid D-Bus paths for software objects,
     * extracts the UUID and SKU properties, and stores them along with the
     * corresponding directory path and D-Bus object path in the
     * otherDeviceDescriptorMap.
     *
     */
    exec::task<void> buildDeviceDescriptorMap();

    /**
     * @brief Interface Addition monitoring
     *
     */
    void startWatchingInterfaceAddition();

    /**
     * @brief Coroutine that reads com.nvidia.Software.UpdateTimeout.Timeout
     *        from an Item Updater activation object and folds the value into
     *        maxItemUpdaterTimeoutCacheSec. Spawned into timeoutScope from
     *        interfaceAdded() for each newly tracked object. The completion
     *        coroutines await timeoutScope.on_empty() before signaling
     *        UpdateManager, so every read is guaranteed to have folded
     *        before activation can consume the cache. Objects that don't
     *        publish the interface (older nvidia-code-mgmt) are skipped
     *        silently; read failures resolve to 0 inside the awaitables and
     *        fold as no-ops.
     *
     * @param path - Item Updater activation object path (by value: the
     *               caller's string may be destroyed across suspension)
     */
    exec::task<void> fetchItemUpdaterTimeout(const std::string path);

    /**
     * @brief Coroutine spawned into completionScope when interfaceAdded()
     *        observes all pending images processed. Awaits
     *        timeoutScope.on_empty() so every fetchItemUpdaterTimeout()
     *        read has folded into the cache, then notifies UpdateManager —
     *        which unblocks activation and the cache consumer,
     *        UpdateManager::activatePackage().
     */
    exec::task<void> notifyOtherDeviceComponents();

    /**
     * @brief Coroutine spawned into completionScope when the activation
     *        wait timer expires with images still pending. Awaits
     *        timeoutScope.on_empty() like notifyOtherDeviceComponents(),
     *        then notifies UpdateManager and logs the devices that never
     *        published an activation object.
     */
    exec::task<void> handleUpdaterActivationTimeout();

    /**
     * @brief Get Activation State of all other devices
     *          if any one to the activation state is activation then it
     *          returns State as activating otherwise Fail / Active
     *
     * @return activation State
     */
    Server::Activation::Activations getOverAllActivationState();

    /**
     * @brief updates the valid target count.
     *
     * TODO: This is the only remaining synchronous D-Bus read in this class.
     * Its caller (processStreamDefer) is not a coroutine and inspects
     * getValidTargets() synchronously immediately after construction, so
     * converting it requires restructuring processStreamDefer. Tracked
     * separately.
     */
    void updateValidTargets(void);

    /**
     * @brief Fetches UUID and SKU from the package
     *
     * @param fwDeviceIDRecord - Firmware Record of the current image
     * @return pair of UUID and SKU. If SKU descriptor not present in package,
     * SKU is empty. Returns std::nullopt if SKU descriptor is present but
     * corrupted.
     */
    std::optional<std::pair<UUID, SKU>> fetchDescriptorsFromPackage(
        const FirmwareDeviceIDRecord& fwDeviceIDRecord);

    /**
     * @brief Transfers the component image to the location at filepath
     *
     * @param filePath - Path to the destination of the component image
     * @param componentImageInfo - Image info of the component to transfer
     * @param package - input stream of the package
     */
    TransferPackageState txComponentImage(
        const std::string& filePath,
        const ComponentImageInfo& componentImageInfo, std::istream& package);

    /**
     * @brief Handles the transfers of a single component
     *
     * @param dirPath - Path to the directory destination of the component image
     * @param componentImageInfo - Image info of the component to transfer
     * @param package - input stream of the package
     * @param objPath - Object Path of the Item Updater
     * @param uuid - UUID of the ItemUpdater
     */
    TransferPackageState txSingleComponent(
        const std::string& dirPath,
        const ComponentImageInfo& componentImageInfo, std::istream& package,
        const std::string& objPath, const UUID& uuid);

    /**
     * @brief Handles the transfers of multiple components
     *
     * @param dirPath - Path to the directory destination of the component
     * images
     * @param applicableCompVec - Vector of components to transfer
     * @param componentImageInfos - Vector of Image info of the components to
     * transfer
     * @param package - input stream of the package
     * @param objPath - Object Path of the Item Updater
     * @param uuid - UUID of the ItemUpdater
     */
    TransferPackageState txMultipleComponents(
        const std::string& dirPath,
        const ApplicableComponents& applicableCompVec,
        const ComponentImageInfos& componentImageInfos, std::istream& package,
        const std::string& objPath, const UUID& uuid);

    UpdateManager* updateManager;
    pldm::utils::DBusHandlerInterface& dbusHandler;

    /**
     * @brief Cache of the valid targets for non-pldm updates
     *
     */
    size_t validTargetCount;

    /**
     * @brief Cached max UpdateTimeout (seconds) across the Item Updater
     *        activation objects tracked for the current package. Reset at the
     *        start of extractOtherDevicePkgs and folded asynchronously by
     *        fetchItemUpdaterTimeout as each object appears.
     */
    uint64_t maxItemUpdaterTimeoutCacheSec{0};

    /**
     * @brief D-Bus object referance
     *
     */
    sdbusplus::bus_t& bus;

    /**
     * @brief Map conatining sw dbus object state
     *
     */
    std::unordered_map<std::string,
                       std::unique_ptr<OtherDeviceUpdateActivation>>
        otherDevices;

    /**
     * @brief Map used to store the association between a device's UUID and SKU
     * and its corresponding directory path and D-Bus object path.
     *
     */
    std::map<std::pair<UUID, SKU>, std::pair<std::string, std::string>>
        otherDeviceDescriptorMap;

    /**
     * @brief Indicates image process state by item updater
     *
     */
    std::unordered_map<std::string, bool> isImageFileProcessed;

    /**
     * @brief matcher rule to check for activation dbus object change
     *
     */
    std::vector<sdbusplus::bus::match_t> activationMatches;

    /**
     * @brief Timer to wait for interface addition
     *
     */
    std::unique_ptr<sdbusplus::Timer> timer;
    /**
     * @brief Software object path matcher for interface addition
     *
     */
    std::unique_ptr<sdbusplus::bus::match_t> interfaceAddedMatch;

    /**
     * @brief List of states which are valid for a FW update to be
     *        considered done.
     *
     */
    static constexpr std::array<Server::Activation::Activations, 2>
        validTerminalActivationStates = {
            Server::Activation::Activations::Active,
            Server::Activation::Activations::Failed};
    /**
     * @brief map to match uuid to version string and component name
     *
     */
    std::unordered_map<std::string, ComponentMap> uuidMappings;
    std::vector<sdbusplus::object_path> targets;

    /** @brief Owns in-flight fetchItemUpdaterTimeout() coroutines. The
     *  destructor sync_waits this scope before any member is torn down, so
     *  a suspended coroutine always sees a live `this` for the full chain
     *  (same pattern as DebugToken::tokenScope). */
    exec::async_scope timeoutScope;

    /** @brief Owns the completion coroutines
     *  (notifyOtherDeviceComponents / handleUpdaterActivationTimeout),
     *  which await timeoutScope.on_empty() and therefore cannot live in
     *  timeoutScope itself. Drained by the destructor after timeoutScope. */
    exec::async_scope completionScope;

    /** @brief Liveness sentinel for async callbacks. Captured as weak_ptr;
     *  expires when this object is destroyed, gating safe use of `this`.
     *  MUST remain the last data member so it is destroyed first, ensuring
     *  any in-flight callbacks see the weak_ptr as expired before the rest
     *  of the object is torn down. */
    std::shared_ptr<bool> aliveFlag = std::make_shared<bool>(true);
};

} // namespace fw_update

} // namespace pldm
