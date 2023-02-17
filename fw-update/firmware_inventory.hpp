#pragma once

#include "common/types.hpp"
#include "common/utils.hpp"

#include <sdbusplus/server.hpp>
#include <sdbusplus/server/object.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/Asset/server.hpp>
#include <xyz/openbmc_project/Software/Version/server.hpp>

namespace pldm::fw_update::fw_inventory
{

using VersionIntf = sdbusplus::xyz::openbmc_project::Software::server::Version;
using AssociationIntf =
    sdbusplus::xyz::openbmc_project::Association::server::Definitions;
using DecoratorAssetIntf =
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::Asset;

using Ifaces = sdbusplus::server::object::object<VersionIntf, AssociationIntf,
                                                 DecoratorAssetIntf>;

/** @class Entry
 *
 *  Implementation of firmware inventory D-Bus object implementing the D-Bus
 *  interfaces.
 *
 *  a) xyz.openbmc_project.Software.Version
 *  b) xyz.openbmc_project.Association.Definitions
 *  c) xyz.openbmc_project.Inventory.Decorator.Asset
 */
class Entry : public Ifaces
{
  public:
    Entry() = delete;
    ~Entry() = default;
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
    Entry(Entry&&) = default;
    Entry& operator=(Entry&&) = default;

    const std::string upFwdAssociation = "software_version";
    const std::string upRevAssociation = "updateable";

    /** @brief Constructor
     *
     *  @param[in] bus  - Bus to attach to
     *  @param[in] objPath - D-Bus object path
     *  @param[in] version - Version string
     *  @param[in] swId - Software ID
     */
    explicit Entry(sdbusplus::bus::bus& bus, const std::string& objPath,
                   const std::string& versionStr, const std::string& swId);
                   
    /** @brief Create association {"software_version", "updateable"} between
     * software version object and "/xyz/openbmc_project/software"
     *
     *  @param[in] swObjPath - "/xyz/openbmc_project/software"
     */
    void createUpdateableAssociation(const std::string& swObjPath);

    /** @brief Create association defined in parameters
     *
     *  @param[in] fwdAssociation - Association forward
     *  @param[in] revAssociation - Association reverse
     *  @param[in] objPath - D-Bus object path
     */
    void createAssociation(const std::string fwdAssociation, 
                            const std::string revAssociation,
                            const std::string& objPath);
};

/** @class Manager
 *
 *  Object manager for firmware inventory objects
 */
class Manager
{
  public:
    Manager() = delete;
    ~Manager() = default;
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;
    Manager(Manager&&) = default;
    Manager& operator=(Manager&&) = default;

    /** @brief Constructor
     *
     *  @param[in] bus  - Bus to attach to
     *  @param[in] firmwareInventoryInfo - Config info for firmware inventory
     *  @param[in] componentInfoMap - Component information of managed FDs
     */
    explicit Manager(sdbusplus::bus::bus& bus,
                     const FirmwareInventoryInfo& firmwareInventoryInfo,
                     const ComponentInfoMap& componentInfoMap,
                     utils::DBusHandlerInterface* dBusHandlerIntf);

    /** @brief Create firmware inventory object
     *
     *  @param[in] eid - MCTP endpointID
     *  @param[in] uuid - MCTP UUID
     *  @param[in] deviceObjPath - Object path of the device inventory object
     */
    void createEntry(pldm::EID eid, const pldm::UUID& uuid);

    const std::string swBasePath = "/xyz/openbmc_project/software";

  private:
    sdbusplus::bus::bus& bus;

    /** @brief Config info for firmware inventory */
    const FirmwareInventoryInfo& firmwareInventoryInfo;

    /** @brief Component information needed for the update of the managed FDs */
    const ComponentInfoMap& componentInfoMap;

    /** @brief Map to store firmware inventory objects */
    std::map<std::pair<EID, CompIdentifier>, std::unique_ptr<Entry>>
        firmwareInventoryMap;

    /** @brief Interface to make D-Bus client calls */
    utils::DBusHandlerInterface* dBusHandlerIntf;

    /** @brief D-Bus signal match for objects to be updated with SoftwareID*/
    std::vector<sdbusplus::bus::match_t> updateFwMatch;

    /** @brief Lookup table to find the SoftwareID for the input D-Bus object
     */
    std::unordered_map<dbus::ObjectPath, std::string> compIdentifierLookup;

    /** @brief Update SoftwareID on the D-Bus object and register for
     *         InterfaceAdded signal to update if the D-Bus object is created
     *         again.
     *
     *  @param[in] objPath - D-Bus object path
     *  @param[in] compId - Component Identifier
     */
    void updateSwId(const dbus::ObjectPath& objPath, const std::string& compId);

    /** @brief Update SoftwareID on the D-Bus object
     *
     *  @param[in] msg - D-Bus message
     */
    void updateSwIdOnSignal(sdbusplus::message::message& msg);
};

} // namespace pldm::fw_update::fw_inventory