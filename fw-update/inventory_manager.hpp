#pragma once

#include "libpldm/requester/pldm.h"

#include "common/types.hpp"
#include "fw_update_utility.hpp"
#include "pldmd/dbus_impl_requester.hpp"
#include "requester/handler.hpp"
#include "requester/mctp_endpoint_discovery.hpp"

#include <queue>

namespace pldm
{

namespace fw_update
{

using CreateInventoryCallBack = std::function<void(EID, UUID, dbus::MctpInterfaces& mctpInterfaces)>;
using UpdateFWVersionCallBack = std::function<void(EID)>;
using MctpEidMap = std::unordered_map<EID, std::tuple<UUID, MctpMedium, MctpBinding>>;

using Priority = int;
static std::unordered_map<MctpMedium, Priority> mediumPriority{
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 0},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SPI", 1},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus", 2},
};

static std::unordered_map<MctpBinding, Priority> bindingPriority{
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", 0},
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.SPI", 1},
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus", 2},
};

struct MctpEidInfo
{
    EID eid;
    MctpMedium medium;
    MctpBinding binding;

    friend bool operator<(MctpEidInfo const& lhs, MctpEidInfo const& rhs)
    {
        if (mediumPriority.at(lhs.medium) == mediumPriority.at(rhs.medium))
            return bindingPriority.at(lhs.binding) > bindingPriority.at(rhs.binding);
        else
            return mediumPriority.at(lhs.medium) > mediumPriority.at(rhs.medium);
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
     *  @param[in] requester - Managing instance ID for PLDM requests
     *  @param[in] createInventoryCallBack - Optional callback function to
     *                                       create device/firmware inventory
     *  @param[out] descriptorMap - Populate the firmware identifers for the
     *                              FDs managed by the BMC.
     *  @param[out] componentInfoMap - Populate the component info for the FDs
     *                                 managed by the BMC.
     *  @param[in] deviceInventoryInfo - device inventory info for message
     *  @param[in] numAttempts - number of command attempts
     * registry
     */
    explicit InventoryManager(
        pldm::requester::Handler<pldm::requester::Request>& handler,
        pldm::dbus_api::Requester& requester,
        CreateInventoryCallBack createInventoryCallBack,
        DescriptorMap& descriptorMap, ComponentInfoMap& componentInfoMap,
        DeviceInventoryInfo& deviceInventoryInfo,
        uint8_t numAttempts = static_cast<uint8_t>(NUMBER_OF_COMMAND_ATTEMPTS)) :
        handler(handler),
        requester(requester), createInventoryCallBack(createInventoryCallBack),
        descriptorMap(descriptorMap), componentInfoMap(componentInfoMap),
        deviceInventoryInfo(deviceInventoryInfo),
        numAttempts(numAttempts)
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
     *  @param[in] eids - MCTP endpoint ID of the FDs
     */
    void discoverFDs(const MctpInfos& mctpInfos, dbus::MctpInterfaces& mctpInterfaces);

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
    requester::Coroutine parseQueryDeviceIdentifiersResponse(
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
     *  @param[in] refreshFWVersionOnly - a boolean flag to update firmware version after receiving platform event
     */
    requester::Coroutine parseGetFWParametersResponse(
        mctp_eid_t eid, const pldm_msg* response, size_t respMsgLen,
        std::string& messageError, std::string& resolution,
        dbus::MctpInterfaces& mctpInterfaces, bool refreshFWVersionOnly = false);

    /** @brief Initiate Get Active Firmware Version
     *
     *  @param[in] eid - Remote MCTP endpoint
     *  @param[in] updateFWVersionCallback - Callback function for updating firmware version in the D-BUS
     */
    void initiateGetActiveFirmwareVersion(mctp_eid_t eid, UpdateFWVersionCallBack updateFWVersionCallback);

  private:

    /** @brief A collection of coroutine handlers used to register PLDM request message handlers */
    std::map<mctp_eid_t, std::coroutine_handle<>> inventoryCoRoutineHandlers;

    /** @brief Starts firmware discovery flow
     *
     *  @param[in] eid - Remote MCTP endpoint
     */
    requester::Coroutine startFirmwareDiscoveryFlow(mctp_eid_t eid, dbus::MctpInterfaces mctpInterfaces);

    /** @brief Starts get Active Firmware Version Flow
     *
     *  @param[in] eid - Remote MCTP endpoint
     *  @param[in] mctpInterfaces - Reference to the dbus::MctpInterfaces object for MCTP communication.
     *  @param[in] updateFWVersionCallback - Callback function for updating firmware version in the D-BUS
     */
    requester::Coroutine getActiveFirmwareVersion(
        mctp_eid_t eid, dbus::MctpInterfaces& mctpInterfaces,
        UpdateFWVersionCallBack updateFWVersionCallback);

    /** @brief Cleans up mctpEidMap and descriptorMap
     *
     *  @param[in] eid - Remote MCTP endpoint
     */
    void cleanUpResources(mctp_eid_t eid);

    /** @brief Send QueryDeviceIdentifiers command request
     *
     *  @param[in] eid - Remote MCTP endpoint
     *  @param[in] messageError - message error
     *  @param[in] resolution - recommended resolution
     */
    requester::Coroutine queryDeviceIdentifiers(mctp_eid_t eid,
                                                std::string& messageError,
                                                std::string& resolution);

    /** @brief Send GetFirmwareParameters command request
     *
     *  @param[in] eid - Remote MCTP endpoint
     *  @param[in] messageError - message error
     *  @param[in] resolution - recommended resolution
     *  @param[in] refreshFWVersionOnly - a boolean flag to update firmware version after receiving platform event
     */
    requester::Coroutine getFirmwareParameters(
        mctp_eid_t eid, std::string& messageError, std::string& resolution,
        dbus::MctpInterfaces& mctpInterfaces, bool refreshFWVersionOnly = false);

    /** @brief PLDM request handler */
    pldm::requester::Handler<pldm::requester::Request>& handler;

    /** @brief D-Bus API for managing instance ID*/
    pldm::dbus_api::Requester& requester;

    /** @brief Optional callback function to create device/firmware inventory*/
    CreateInventoryCallBack createInventoryCallBack;

    /** @brief Device identifiers of the managed FDs */
    DescriptorMap& descriptorMap;

    /** @brief Component information needed for the update of the managed FDs */
    ComponentInfoMap& componentInfoMap;

    /** @brief device information to create message registries */
    DeviceInventoryInfo& deviceInventoryInfo;

    /** @brief MCTP endpoint to MCTP UUID mapping*/
    MctpEidMap mctpEidMap;

    MctpInfoMap mctpInfoMap;

    /** @brief Inventory command attempt count */
    uint8_t numAttempts;

    /**
     * @brief log devicediscovery failed messages
     *
     * @param[in] eid - mctp end point
     * @param[in] messageError - message error
     * @param[in] resolution - recommended resolution
     */
    void logDiscoveryFailedMessage(const mctp_eid_t& eid,
                                   const std::string& messageError,
                                   const std::string& resolution,
                                   dbus::MctpInterfaces mctpInterfaces);
};

} // namespace fw_update

} // namespace pldm
