#pragma once

#include "libpldm/entity.h"
#include "libpldm/platform.h"

#include "common/types.hpp"
#include "entity.hpp"
#include "numeric_sensor.hpp"
#include "state_sensor.hpp"

#include <sdbusplus/server/object.hpp>
#include <sdeventplus/event.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>

#include <coroutine>

using namespace pldm::pdr;

namespace pldm
{
namespace platform_mc
{
using SensorId = uint16_t;
using SensorCnt = uint8_t;
using NameLanguageTag = std::string;
using SensorName = std::string;
using SensorAuxiliaryNames =
    std::tuple<SensorId, SensorCnt,
               std::vector<std::pair<NameLanguageTag, SensorName>>>;
using EnitityAssociations =
    std::map<ContainerID, std::pair<EntityInfo, std::set<EntityInfo>>>;
using AssociationDefinitionsIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Association::server::Definitions>;
class TerminusManager;

constexpr auto instanceInterface =
    "xyz.openbmc_project.Inventory.Decorator.Instance";
constexpr auto instanceProperty = "InstanceNumber";
constexpr auto overallSystemInterface =
    "xyz.openbmc_project.Inventory.Item.System";
constexpr ContainerID overallSystemCotainerId = 0;
static const std::map<EntityType, std::string_view> entityInterfaces = {
    {PLDM_ENTITY_PHYSCIAL | PLDM_ENTITY_PROC_IO_MODULE,
     "xyz.openbmc_project.Inventory.Item.ProcessorModule"},
    {PLDM_ENTITY_PHYSCIAL | PLDM_ENTITY_PROC,
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
    Terminus(tid_t tid, uint64_t supportedPLDMTypes);

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

    /** @brief A list of state Sensors */
    std::vector<std::shared_ptr<StateSensor>> stateSensors{};

    /** @brief A list of parsed numeric sensor PDRs */
    std::vector<std::shared_ptr<pldm_numeric_sensor_value_pdr>>
        numericSensorPdrs{};

    /** @brief A list of parsed state sensor PDRs */
    std::vector<std::tuple<SensorID, StateSetSensorInfo>> stateSensorPdrs{};

    /** @brief A map of EntityInfo to Entity informaiton */
    std::map<EntityInfo, Entity> entities;

    /** @brief Get Sensor Auxiliary Names by sensorID
     *
     *  @param[in] id - sensor ID
     *  @return sensor auxiliary names
     */
    std::shared_ptr<SensorAuxiliaryNames> getSensorAuxiliaryNames(SensorId id);

    void parseEntityAssociationPDR(const std::vector<uint8_t>& pdrData);

    void scanInventories();
    void updateAssociations();

    void addNumericSensor(
        const std::shared_ptr<pldm_numeric_sensor_value_pdr> pdr);

    void addStateSensor(SensorID sId, StateSetSensorInfo sensorInfo);

    /** @brief maximum buffer size the terminus can send and receive */
    uint16_t maxBufferSize;

    /** @brief Handle of started pollForPlatformEventTask coroutine */
    std::coroutine_handle<> pollForPlatformEventTaskHandle;

    void interfaceAdded(sdbusplus::message::message& m);

    bool initalized;

  private:
    std::shared_ptr<pldm_numeric_sensor_value_pdr>
        parseNumericSensorPDR(const std::vector<uint8_t>& pdrData);

    std::shared_ptr<SensorAuxiliaryNames>
        parseSensorAuxiliaryNamesPDR(const std::vector<uint8_t>& pdrData);

    std::tuple<SensorID, StateSetSensorInfo>
        parseStateSensorPDR(std::vector<uint8_t>& pdr);

    tid_t tid;
    std::bitset<64> supportedTypes;

    std::vector<std::shared_ptr<SensorAuxiliaryNames>>
        sensorAuxiliaryNamesTbl{};

    std::string systemInventoryPath;
    std::vector<std::tuple<dbus::ObjectPath, EntityType, EntityInstance>>
        inventories;
    std::unique_ptr<sdbusplus::bus::match_t> interfaceAddedMatch;
    EnitityAssociations entityAssociations;
};
} // namespace platform_mc
} // namespace pldm
