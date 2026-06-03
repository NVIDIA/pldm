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
#include "fw_update_utility.hpp"
#include "requester/handler.hpp"
#include "requester/mctp_endpoint_discovery.hpp"

#include <libpldm/pldm.h>

#include <sdeventplus/event.hpp>

#include <limits>
#include <queue>

namespace pldm
{

namespace fw_update
{

using CreateInventoryCallBack =
    std::function<void(eid, UUID, dbus::MctpInterfaces& mctpInterfaces)>;
using UpdateInventoryCallBack =
    std::function<void(eid, UUID, dbus::MctpInterfaces& mctpInterfaces)>;
using UpdateFWVersionCallBack = std::function<void(eid)>;
using MctpEidMap =
    std::unordered_map<eid, std::tuple<UUID, MctpMedium, MctpBinding>>;

using Priority = int;

static std::unordered_map<MctpMedium, Priority> mediumPriority = {
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 0},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB", 1},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SPI", 2},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.I3C", 3},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.KCS", 4},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.Serial", 5},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus", 6}};

/**
 * @brief MCTP Binding Type priority table ordering by bandwidth
 */
static std::unordered_map<MctpBinding, Priority> bindingPriority = {
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", 0},
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.USB", 1},
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.SPI", 2},
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.KCS", 3},
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.Serial", 4},
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus", 5}};

/** @brief Look up the priority for a medium/binding string.
 *
 *  @param[in] table - priority table to query
 *  @param[in] key   - medium or binding string sourced from D-Bus
 *
 *  @return The priority value for the key, or
 *          std::numeric_limits<Priority>::max() when the key is not
 *          present in the table. Strings absent from the priority
 *          maps therefore rank as lowest priority.
 */
inline Priority lookupPriority(
    const std::unordered_map<std::string, Priority>& table,
    const std::string& key)
{
    auto it = table.find(key);
    return it != table.end() ? it->second
                             : std::numeric_limits<Priority>::max();
}

struct MctpEidInfo
{
    pldm::eid eid;
    MctpMedium medium;
    MctpBinding binding;

    friend bool operator<(const MctpEidInfo& lhs, const MctpEidInfo& rhs)
    {
        auto lhsMediumPrio = lookupPriority(mediumPriority, lhs.medium);
        auto rhsMediumPrio = lookupPriority(mediumPriority, rhs.medium);
        if (lhsMediumPrio == rhsMediumPrio)
        {
            return lookupPriority(bindingPriority, lhs.binding) >
                   lookupPriority(bindingPriority, rhs.binding);
        }
        return lhsMediumPrio > rhsMediumPrio;
    }
};

struct MCTPEidInfoPriorityQueue : std::priority_queue<MctpEidInfo>
{
    auto begin() const
    {
        return c.begin();
    }
    auto end() const
    {
        return c.end();
    }
};

using MctpInfoMap = std::map<UUID, MCTPEidInfoPriorityQueue>;

/** @class InventoryManager
 *
 *  InventoryManager class manages the software inventory of firmware
 * devices managed by the BMC. It discovers the firmware identifiers and the
 * component details of the FD. Firmware identifiers, component details and
 * update capabilities of FD are populated by the InventoryManager and is
 * used for the firmware update of the FDs.
 */
class InventoryManager
{
  public:
    InventoryManager() = delete;
    InventoryManager(const InventoryManager&) = delete;
    InventoryManager(InventoryManager&&) = delete;
    InventoryManager& operator=(const InventoryManager&) = delete;
    InventoryManager& operator=(InventoryManager&&) = delete;

    /** @brief Constructor
     *
     *  @param[in] handler - PLDM request handler
     *  @param[in] instanceIdDb - Managing instance ID for PLDM requests
     *  @param[in] createInventoryCallBack - Optional callback function to
     *                                       create device/firmware inventory
     *  @param[in] updateInventoryCallBack - Optional callback function to
     *                                       update device/firmware inventory
     *  @param[out] descriptorMap - Populate the firmware identifers for the
     *                              FDs managed by the BMC.
     *  @param[out] downstreamDescriptorMap - Populate the downstream
     *                                        identifiers for the FDs managed
     *                                        by the BMC.
     *  @param[out] componentInfoMap - Populate the component info for the FDs
     *                                 managed by the BMC.
     *  @param[in] numAttempts - number of command attempts
     */
    explicit InventoryManager(
        [[maybe_unused]] const pldm::utils::DBusHandler* dbusHandler,
        pldm::requester::Handler<pldm::requester::Request>& handler,
        InstanceIdDb& instanceIdDb,
        CreateInventoryCallBack createInventoryCallBack,
        UpdateInventoryCallBack updateInventoryCallBack,
        DescriptorMap& descriptorMap,
        DownstreamDescriptorMap& downstreamDescriptorMap,
        ComponentInfoMap& componentInfoMap,
        uint8_t numAttempts =
            static_cast<uint8_t>(NUMBER_OF_COMMAND_ATTEMPTS)) :
        handler(handler), instanceIdDb(instanceIdDb),
        createInventoryCallBack(createInventoryCallBack),
        updateInventoryCallBack(updateInventoryCallBack),
        descriptorMap(descriptorMap),
        downstreamDescriptorMap(downstreamDescriptorMap),
        componentInfoMap(componentInfoMap), numAttempts(numAttempts)
    {}

    /** @brief Destructor
     *
     * The main purpose of this destructor is to release all coroutine handlers
     * stored in the collection inventoryCoRoutineHandlers.
     *
     */
    ~InventoryManager()
    {
        for (const auto& [eid, cohandler] : inventoryCoRoutineHandlers)
        {
            cohandler.destroy();
        }
    }

    /** @brief Discover the firmware identifiers and component details of FDs
     *
     *  Inventory commands QueryDeviceIdentifiers and GetFirmwareParmeters
     *  commands are sent to every FD and the response is used to populate
     *  the firmware identifiers and component details of the FDs.
     *
     *  @param[in] mctpInfos - List of MCTP endpoint information
     */
    void discoverFDs(const MctpInfos& mctpInfos,
                     dbus::MctpInterfaces mctpInterfaces);
    exec::task<int> discoverFDsTask();

    /** @brief Remove cached firmware identifiers and component details of FDs.
     */
    void removeFDs(const MctpInfos& mctpInfos);

    void setConfigurations(const Configurations& updatedConfigurations)
    {
        configurations = updatedConfigurations;
    }

    /** @brief Handler for QueryDeviceIdentifiers command response
     *
     *  The response of the QueryDeviceIdentifiers is processed and firmware
     *  identifiers of the FD is updated. GetFirmwareParameters command request
     *  is sent to the FD.
     *
     *  @param[in] eid - Remote MCTP endpoint
     *  @param[in] response - PLDM response message
     *  @param[in] respMsgLen - Response message length
     *  @param[in] messageError - message error
     *  @param[in] resolution - recommended resolution
     */
    exec::task<int> parseQueryDeviceIdentifiersResponse(
        mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen,
        std::string& messageError, std::string& resolution);

    /** @brief Handler for GetFirmwareParameters command response
     *
     *  Handling the response of GetFirmwareParameters command and create
     *  software version D-Bus objects.
     *
     *  @param[in] eid - Remote MCTP endpoint
     *  @param[in] response - PLDM response message
     *  @param[in] respMsgLen - Response message length
     *  @param[in] messageError - message error
     *  @param[in] resolution - recommended resolution
     *  @param[in] refreshFWVersionOnly - a boolean flag to update firmware
     * version after receiving platform event
     */
    exec::task<int> parseGetFWParametersResponse(
        mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen,
        std::string& messageError, std::string& resolution,
        dbus::MctpInterfaces& mctpInterfaces,
        bool refreshFWVersionOnly = false);

    /** @brief Initiate Get Active Firmware Version
     *
     *  @param[in] eid - Remote MCTP endpoint
     *  @param[in] updateFWVersionCallback - Callback function for updating
     * firmware version in the D-BUS
     */
    exec::task<int> initiateGetActiveFirmwareVersion(
        mctp_eid_t eid, UpdateFWVersionCallBack updateFWVersionCallback);

    /** @brief Send getPLDMTypes command to destination eid and then return the
     *         value of supportedTypes.
     *
     *  @param[in] eid - Destination eid
     *  @param[out] supportedTypes - Supported Types returned from eid
     *  @return coroutine return_value - PLDM completion code
     */
    exec::task<int> getPLDMTypes(mctp_eid_t eid, uint64_t& supportedTypes);

    /** @brief Handler for QueryDownstreamIdentifiers command response
     *
     *  @param[in] eid - Remote MCTP endpoint
     *  @param[in] response - PLDM response message
     *  @param[in] respMsgLen - Response message length
     */
    virtual sdbusplus::async::task<int> parseQueryDownstreamIdentifiersResponse(
        mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen);

    /** @brief Refresh descriptors for a single endpoint
     *
     *  Helper function to refresh QueryDeviceIdentifiers and
     *  GetFirmwareParameters for a single endpoint. Used by parallel refresh.
     *
     *  @param[in] eid - MCTP endpoint to refresh
     *  @param[in] mctpInterfaces - MCTP interface information
     *  @param[in] isTarget - Whether this endpoint is a target for update
     *  @return PLDM_SUCCESS on success, error code otherwise
     */
    exec::task<int> refreshSingleEndpoint(
        mctp_eid_t eid, dbus::MctpInterfaces& mctpInterfaces, bool isTarget);

    /** @brief Cleans up mctpEidMap and descriptorMap
     *
     *  @param[in] eid - Remote MCTP endpoint
     */
    void cleanUpResources(mctp_eid_t eid);

  private:
    /** @brief A collection of coroutine handlers used to register PLDM request
     * message handlers */
    std::map<mctp_eid_t, std::coroutine_handle<>> inventoryCoRoutineHandlers;

    /** @brief Starts firmware discovery flow
     *
     *  @param[in] eid - Remote MCTP endpoint
     */
    exec::task<int> startFirmwareDiscoveryFlow(
        mctp_eid_t eid, dbus::MctpInterfaces mctpInterfaces);

    /** @brief Starts get Active Firmware Version Flow
     *
     *  @param[in] eid - Remote MCTP endpoint
     *  @param[in] mctpInterfaces - Reference to the dbus::MctpInterfaces object
     * for MCTP communication.
     *  @param[in] updateFWVersionCallback - Callback function for updating
     * firmware version in the D-BUS
     */
    exec::task<int> getActiveFirmwareVersion(
        mctp_eid_t eid, dbus::MctpInterfaces& mctpInterfaces,
        UpdateFWVersionCallBack updateFWVersionCallback);

    /**
     * @brief Sends QueryDownstreamDevices request
     *
     * @param[in] eid - Remote MCTP endpoint
     */
    sdbusplus::async::task<int> queryDownstreamDevices(mctp_eid_t eid);

    /**
     * @brief Sends QueryDownstreamIdentifiers request
     *
     * The request format is defined at Table 16 – QueryDownstreamIdentifiers
     * command format in DSP0267_1.1.0
     *
     * @param[in] eid - Remote MCTP endpoint
     * @param[in] dataTransferHandle - Data transfer handle
     * @param[in] transferOperationFlag - Transfer operation flag
     */
    virtual sdbusplus::async::task<int> queryDownstreamIdentifiers(
        mctp_eid_t eid, uint32_t dataTransferHandle,
        enum transfer_op_flag transferOperationFlag);

    /**
     * @brief Sends QueryDownstreamFirmwareParameters request
     *
     * @param[in] eid - Remote MCTP endpoint
     * @param[in] dataTransferHandle - Data transfer handle
     * @param[in] transferOperationFlag - Transfer operation flag
     */
    virtual sdbusplus::async::task<int> getDownstreamFirmwareParameters(
        mctp_eid_t eid, uint32_t dataTransferHandle,
        const enum transfer_op_flag transferOperationFlag);

    /** @brief Send QueryDeviceIdentifiers command request
     *
     *  @param[in] eid - Remote MCTP endpoint
     *  @param[in] messageError - message error
     *  @param[in] resolution - recommended resolution
     */
    exec::task<int> queryDeviceIdentifiers(
        mctp_eid_t eid, std::string& messageError, std::string& resolution);

    /** @brief Obtain Firmware Device Name from configuration or descriptors.
     */
    void obtainFirmwareDeviceName(pldm::eid eid,
                                  const Descriptors& descriptors);

    /** @brief Send GetFirmwareParameters command request
     *
     *  @param[in] eid - Remote MCTP endpoint
     *  @param[in] messageError - message error
     *  @param[in] resolution - recommended resolution
     *  @param[in] refreshFWVersionOnly - a boolean flag to update firmware
     * version after receiving platform event
     */
    exec::task<int> getFirmwareParameters(
        mctp_eid_t eid, std::string& messageError, std::string& resolution,
        dbus::MctpInterfaces& mctpInterfaces,
        bool refreshFWVersionOnly = false);

    /** @brief Handler for QueryDownstreamDevices command response
     *
     *  @param[in] eid - Remote MCTP endpoint
     *  @param[in] response - PLDM response message
     *  @param[in] respMsgLen - Response message length
     */
    virtual sdbusplus::async::task<int> parseQueryDownstreamDevicesResponse(
        mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen);

    /** @brief Handler for GetDownstreamFirmwareParameters command response
     *
     *  @param[in] eid - Remote MCTP endpoint
     *  @param[in] response - PLDM response message
     *  @param[in] respMsgLen - Response message length
     */
    virtual sdbusplus::async::task<int>
        parseGetDownstreamFirmwareParametersResponse(
            mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen);

    /** @brief Log device status errors with component name
     *  @param[in] eid - endpoint ID
     *  @param[in] overrideSeverity - override severity to informational
     *  @param[in] logNamespace - namespace for the log
     *  @return bool - true if errors were logged, false otherwise
     */
    bool logDeviceStatusErrors(const mctp_eid_t eid,
                               bool overrideSeverity = false,
                               const std::string& logNamespace = "");

    /**
     * @brief log devicediscovery failed messages
     *
     * @param[in] eid - mctp end point
     * @param[in] messageError - message error
     * @param[in] resolution - recommended resolution
     */
    void logDiscoveryFailedMessage(
        const mctp_eid_t eid, const std::string& messageError,
        const std::string& resolution, dbus::MctpInterfaces mctpInterfaces,
        const std::string& logNamespace = "", bool overrideSeverity = false);

    /** @brief PLDM request handler */
    pldm::requester::Handler<pldm::requester::Request>& handler;

    /** @brief Instance ID database for managing instance ID*/
    InstanceIdDb& instanceIdDb;

    /** @brief Optional callback function to create device/firmware inventory*/
    CreateInventoryCallBack createInventoryCallBack;

    /** @brief Optional callback function to update device/firmware inventory*/
    UpdateInventoryCallBack updateInventoryCallBack;

    /** @brief Device identifiers of the managed FDs */
    DescriptorMap& descriptorMap;

    /** @brief Firmware Device names of the managed FDs */
    std::map<eid, SoftwareName> firmwareDeviceNameMap;

    /** @brief Downstream Device identifiers of the managed FDs */
    DownstreamDescriptorMap& downstreamDescriptorMap;

    /** @brief Component information needed for the update of the managed FDs */
    ComponentInfoMap& componentInfoMap;

    /** @brief MCTP endpoint to MCTP UUID mapping*/
    MctpEidMap mctpEidMap;

    MctpInfoMap mctpInfoMap;

    /** @brief Inventory command attempt count */
    uint8_t numAttempts;

    /** @brief A queue of MctpInfos to be discovered **/
    std::queue<std::pair<MctpInfos, dbus::MctpInterfaces>> queuedMctpInfos{};

    /** @brief To send a PLDM request after the current command handling */
    std::unordered_map<eid, std::unique_ptr<sdeventplus::source::Defer>>
        pldmRequest;

    /** @brief coroutine handle of discoverFDsTask */
    std::optional<std::pair<exec::async_scope, std::optional<int>>>
        discoverFDsTaskHandle{};

    /** @brief Latest configuration bindings from Entity Manager */
    Configurations configurations;
};

/**
 * @brief Obtain the device name from the configurations
 *
 * @param[in] configurations - Configurations from Entity Manager
 * @param[in] eid - Remote MCTP endpoint
 *
 * @return SoftwareName - The Device name, std::nullopt if not found
 */
std::optional<SoftwareName> obtainDeviceNameFromConfigurations(
    const Configurations& configurations, pldm::eid eid);

/**
 * @brief Obtain the device name from the descriptors
 *
 * @param[in] descriptors - Descriptors of the device
 *
 * @return SoftwareName - The Device name, std::nullopt if not found
 */
std::optional<SoftwareName> obtainDeviceNameFromDescriptors(
    const Descriptors& descriptors);

} // namespace fw_update

} // namespace pldm
