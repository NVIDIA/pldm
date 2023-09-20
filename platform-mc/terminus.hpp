#pragma once

#include "libpldm/entity.h"
#include "libpldm/platform.h"

#include "common/types.hpp"
#include "entity.hpp"
#include "numeric_effecter.hpp"
#include "numeric_sensor.hpp"
#include "state_effecter.hpp"
#include "state_sensor.hpp"

#include <sdbusplus/server/object.hpp>
#include <sdeventplus/event.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/Area/server.hpp>

#include <coroutine>

using namespace pldm::pdr;

namespace pldm
{
namespace platform_mc
{
using SensorCnt = uint8_t;
using EffecterCnt = SensorCnt;
using NameLanguageTag = std::string;
using SensorName = std::string;
using EffecterName = SensorName;
using SensorAuxiliaryNames = std::tuple<
    SensorID, SensorCnt,
    std::vector<std::vector<std::pair<NameLanguageTag, SensorName>>>>;
using EffecterAuxiliaryNames = SensorAuxiliaryNames;
using EnitityAssociations =
    std::map<ContainerID, std::pair<EntityInfo, std::set<EntityInfo>>>;
using PhysicalContextType = sdbusplus::xyz::openbmc_project::Inventory::
    Decorator::server::Area::PhysicalContextType;
using InventoryDecoratorAreaIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::Area>;
using OemRecordId = uint16_t;
using VendorSpecificData = std::vector<uint8_t>;
using OemPdr = std::tuple<VendorIANA, OemRecordId, VendorSpecificData>;
using AssociationDefinitionsIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Association::server::Definitions>;
class TerminusManager;

constexpr auto instanceInterface =
    "xyz.openbmc_project.Inventory.Decorator.Instance";
constexpr auto instanceProperty = "InstanceNumber";
constexpr auto overallSystemInterface =
    "xyz.openbmc_project.Inventory.Item.System";
constexpr auto chassisInterface = "xyz.openbmc_project.Inventory.Item.Chassis";
constexpr ContainerID overallSystemCotainerId = 0;
static const std::map<EntityType, std::string_view> entityInterfaces = {
    {PLDM_ENTITY_PHYSCIAL | PLDM_ENTITY_PROC_IO_MODULE,
     "xyz.openbmc_project.Inventory.Item.ProcessorModule"},
    {PLDM_ENTITY_LOGICAL | PLDM_ENTITY_PROC,
     "xyz.openbmc_project.Inventory.Item.Cpu"}};

/**
 * @brief Terminus
 *
 * Terminus class holds the TID, supported PLDM Type or PDRs which are needed by
 * other manager class for sensor monitoring and control.
 */
class Terminus
{
  public:
    Terminus(tid_t tid, uint64_t supportedPLDMTypes,
             TerminusManager& terminusManager);

    /** @brief Check if the terminus supports the PLDM type message
     *
     *  @param[in] type - PLDM Type
     */
    bool doesSupport(uint8_t type);

    /** @brief Parse the PDRs stored in the member variable, pdrs.
     *
     *  @return False if any unsupported PDR is detected.
     */
    bool parsePDRs();

    /** @brief The getter to return terminus's TID */
    tid_t getTid()
    {
        return tid;
    }

    /** @brief Look for the inventory which this entity should associate
     * with */
    std::string findInventory(EntityInfo entityInfo, bool findClosest = true);

    /** @brief Find the EntityInfo from the Container ID, and pass it to
     * findInventory(EntityInfo) */
    std::string findInventory(ContainerID contianerId, bool findClosest = true);

    /** @brief A list of PDRs fetched from Terminus */
    std::vector<std::vector<uint8_t>> pdrs{};

    /** @brief A list of numericSensors */
    std::vector<std::shared_ptr<NumericSensor>> numericSensors{};

    /** @brief A list of numericEffecters */
    std::vector<std::shared_ptr<NumericEffecter>> numericEffecters{};

    /** @brief A list of state Sensors */
    std::vector<std::shared_ptr<StateSensor>> stateSensors{};

    /** @brief A list of state Effecters */
    std::vector<std::shared_ptr<StateEffecter>> stateEffecters{};

    /** @brief A list of parsed numeric sensor PDRs */
    std::vector<std::shared_ptr<pldm_numeric_sensor_value_pdr>>
        numericSensorPdrs{};

    /** @brief A list of parsed numeric effecter PDRs */
    std::vector<std::shared_ptr<pldm_numeric_effecter_value_pdr>>
        numericEffecterPdrs{};

    /** @brief A list of parsed state sensor PDRs */
    std::vector<std::tuple<SensorID, StateSetInfo>> stateSensorPdrs{};

    /** @brief A list of parsed OEM PDRs */
    std::vector<OemPdr> oemPdrs{};

    /** @brief A map of EntityInfo to Entity informaiton */
    std::map<EntityInfo, Entity> entities;

    /** @brief A list of parsed state effecter PDRs */
    std::vector<std::tuple<EffecterID, StateSetInfo>> stateEffecterPdrs{};

    /** @brief Get Sensor Auxiliary Names by sensorID
     *
     *  @param[in] id - sensor ID
     *  @return sensor auxiliary names
     */
    std::shared_ptr<SensorAuxiliaryNames> getSensorAuxiliaryNames(SensorID id);

    /** @brief Get Effecter Auxiliary Names by effecterID
     *
     *  @param[in] id - effecter ID
     *  @return effecter auxiliary names
     */
    std::shared_ptr<EffecterAuxiliaryNames>
        getEffecterAuxiliaryNames(EffecterID id);

    void parseEntityAssociationPDR(const std::vector<uint8_t>& pdrData);

    bool scanInventories();

    void updateAssociations();

    void addNumericSensor(
        const std::shared_ptr<pldm_numeric_sensor_value_pdr> pdr);

    void addStateSensor(SensorID sId, StateSetInfo sensorInfo);

    void addNumericEffecter(
        const std::shared_ptr<pldm_numeric_effecter_value_pdr> pdr);

    void addStateEffecter(EffecterID eId, StateSetInfo effecterInfo);

    /** @brief maximum buffer size the terminus can send and receive */
    uint16_t maxBufferSize;

    void interfaceAdded(sdbusplus::message::message& m);

    /** @brief The flag indicates whether the terminus has been initialized
     * by terminusManaer */
    bool initalized;

    /** @brief The flag indicates that the terminus FIFO contains a large
     * message that will require a multipart transfer via the
     * PollForPlatformEvent command */
    bool pollEvent;

    /** @brief This value indicates the event messaging styles supported by the
     * terminus */
    uint8_t synchronyConfigurationSupported;

  private:
    std::shared_ptr<pldm_numeric_sensor_value_pdr>
        parseNumericSensorPDR(const std::vector<uint8_t>& pdrData);

    std::shared_ptr<pldm_numeric_effecter_value_pdr>
        parseNumericEffecterPDR(const std::vector<uint8_t>& pdrData);

    std::shared_ptr<SensorAuxiliaryNames>
        parseSensorAuxiliaryNamesPDR(const std::vector<uint8_t>& pdrData);

    std::shared_ptr<EffecterAuxiliaryNames>
        parseEffecterAuxiliaryNamesPDR(const std::vector<uint8_t>& pdrData);

    std::tuple<SensorID, StateSetInfo>
        parseStateSensorPDR(std::vector<uint8_t>& pdr);

    void parseStateSetInfo(const unsigned char* statesPtr,
                           uint8_t compositeSensorCount,
                           std::vector<StateSetData>& stateSets);

    std::tuple<EffecterID, StateSetInfo>
        parseStateEffecterPDR(std::vector<uint8_t>& stateEffecterPdr);

    OemPdr parseOemPDR(const std::vector<uint8_t>& oemPdr);

    /** @brief Convert EntityType to PhysicalContextType
     *
     *  @param[in] EntityType - entityType
     *  @return PhysicalContextType
     */
    PhysicalContextType toPhysicalContextType(const EntityType entityType);

    tid_t tid;

    std::bitset<64> supportedTypes;

    std::vector<std::shared_ptr<SensorAuxiliaryNames>>
        sensorAuxiliaryNamesTbl{};

    std::vector<std::shared_ptr<EffecterAuxiliaryNames>>
        effecterAuxiliaryNamesTbl{};

    std::string systemInventoryPath;

    std::vector<std::tuple<dbus::ObjectPath, EntityType, EntityInstance>>
        inventories;

    std::unique_ptr<sdbusplus::bus::match_t> interfaceAddedMatch;

    EnitityAssociations entityAssociations;

    TerminusManager& terminusManager;
};
} // namespace platform_mc
} // namespace pldm
