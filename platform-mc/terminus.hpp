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

#include "libpldm/entity.h"
#include "libpldm/platform.h"

#ifdef OEM_NVIDIA
#include "oem/nvidia/libpldm/energy_count_numeric_sensor_oem.h"

#include "oem/nvidia/platform-mc/derived_sensor/switchBandwidthSensor.hpp"
#endif

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
#include <xyz/openbmc_project/Inventory/Decorator/PortInfo/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/PortState/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/NetworkInterface/server.hpp>

#include <coroutine>

using namespace pldm::pdr;

namespace pldm
{
namespace platform_mc
{

using PhysicalContextType = sdbusplus::xyz::openbmc_project::Inventory::
    Decorator::server::Area::PhysicalContextType;
using InventoryDecoratorAreaIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Inventory::Decorator::server::Area>;
using OemRecordId = uint16_t;
using VendorSpecificData = std::vector<uint8_t>;
using OemPdr = std::tuple<VendorIANA, OemRecordId, VendorSpecificData>;
using AssociationDefinitionsIntf = sdbusplus::server::object_t<
    sdbusplus::xyz::openbmc_project::Association::server::Definitions>;
using PortType = sdbusplus::server::xyz::openbmc_project::inventory::decorator::
    PortInfo::PortType;
using PortProtocol = sdbusplus::server::xyz::openbmc_project::inventory::
    decorator::PortInfo::PortProtocol;
using PortInfoIntf = sdbusplus::server::object_t<
    sdbusplus::server::xyz::openbmc_project::inventory::decorator::PortInfo>;

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
     "xyz.openbmc_project.Inventory.Item.Cpu"},
    {PLDM_ENTITY_PHYSCIAL | PLDM_ENTITY_ADD_IN_CARD,
     "xyz.openbmc_project.Inventory.Item.Board"}};

/**
 * @brief Terminus
 *
 * Terminus class holds the TID, supported PLDM Type or PDRs which are needed by
 * other manager class for sensor monitoring and control.
 */
class Terminus
{
  public:
    Terminus(tid_t tid, uint64_t supportedPLDMTypes, UUID& uuid,
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
    std::vector<std::string> findInventory(EntityInfo entityInfo,
                                           bool findClosest = true);

    /** @brief Find the EntityInfo from the Container ID, and pass it to
     * findInventory(EntityInfo) */
    std::vector<std::string> findInventory(ContainerID contianerId,
                                           bool findClosest = true);

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

    /** @brief priority sensor list */
    std::vector<std::shared_ptr<NumericSensor>> prioritySensors;

    /** @brief round robin sensor list */
    std::queue<std::variant<std::shared_ptr<NumericSensor>,
                            std::shared_ptr<StateSensor>>>
        roundRobinSensors;

    bool stopPolling = false;

    /** @brief coroutine handle of doSensorPollingTask */
    std::coroutine_handle<> doSensorPollingTaskHandle;

#ifdef OEM_NVIDIA
    /** @brief A list of parsed OEM energyCount numeric sensor PDRs */
    std::vector<std::shared_ptr<pldm_oem_energycount_numeric_sensor_value_pdr>>
        oemEnergyCountNumericSensorPdrs{};
#endif

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

#ifdef OEM_NVIDIA
    /** @brief Get Sensor Port type by sensorID
     *
     *  @param[in] id - sensor ID
     *  @return sensor port types
     */
    std::shared_ptr<std::tuple<PortType, PortProtocol, uint64_t,
                               std::vector<dbus::PathAssociation>>>
        getSensorPortInfo(SensorID id);

    std::shared_ptr<oem_nvidia::SwitchBandwidthSensor> switchBandwidthSensor =
        nullptr;
#endif

    void parseEntityAssociationPDR(const std::vector<uint8_t>& pdrData);

    requester::Coroutine scanInventories();

    requester::Coroutine updateAssociations();

    void addNumericSensor(
        const std::shared_ptr<pldm_numeric_sensor_value_pdr> pdr);

#ifdef OEM_NVIDIA
    void addOEMEnergyCountNumericSensor(
        const std::shared_ptr<pldm_oem_energycount_numeric_sensor_value_pdr>
            pdr);
#endif

    void addStateSensor(SensorID sId, StateSetInfo sensorInfo);

    void addNumericEffecter(
        const std::shared_ptr<pldm_numeric_effecter_value_pdr> pdr);

    void addStateEffecter(EffecterID eId, StateSetInfo effecterInfo);

    /** @brief maximum buffer size the terminus can send and receive */
    uint16_t maxBufferSize;

    /** @brief callback when received interfaceAdded signal from
     * /xyz/openbmc_project/inventory */
    void interfaceAdded(sdbusplus::message::message& m);

    /** @brief check if device inventory belong to the terminus
     *
     *  @param[in] objPath - device inventory path
     *  @return true  - the device inventory might belong to the terminus
     *          false - the device inventory doesn't belong to the terminus
     */
    requester::Coroutine checkDeviceInventory(const std::string& objPath);
    requester::Coroutine checkI2CDeviceInventory(uint8_t bus, uint8_t addr);
    bool checkNsmDeviceInventory(UUID nsmUuid);

    /** @brief get Sensor Aux Name from EM configuration PDI
     *
     *  @param[in] objPath - device inventory path
     */
    requester::Coroutine getSensorAuxNameFromEM(uint8_t bus, uint8_t addr,
                                                const std::string& objPath);

#ifdef OEM_NVIDIA
    /** @brief get sensor Port information from EM configuration PDI
     *
     *  @param[in] objPath - device inventory path
     */
    requester::Coroutine getPortInfoFromEM(const std::string& objPath);
    requester::Coroutine getInfoForNVSwitchFromEM(const std::string& objPath);
#endif

    /** @brief The flag indicates whether the terminus has been initialized
     * by terminusManaer */
    bool initalized;

    /** @brief The flag indicates that the terminus FIFO contains a large
     * message that will require a multipart transfer via the
     * PollForPlatformEvent command */
    bool pollEvent;

    /** @brief The flag indicates that the terminus is ready ( i.e All of it's
     * round robin sensors were checked at least once ) */
    bool ready;

    /** @brief This value indicates the event messaging styles supported by the
     * terminus */
    uint8_t synchronyConfigurationSupported;

    /** @brief This value indicates if the terminus is resumed successfully */
    bool resumed;

    /** @brief This value indicates if polling sensor list need to be
     * initialized */
    bool initSensorList;

    /** @brief set the terminus to online state */
    void setOnline();

    /** @brief set the terminus to offline state */
    void setOffline();

    const UUID& getUuid()
    {
        return uuid;
    }

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

    std::optional<std::string> getAuxNameForNumericSensor(SensorID id);

    tid_t tid;

    std::bitset<64> supportedTypes;

    UUID uuid;

    std::vector<std::shared_ptr<SensorAuxiliaryNames>>
        sensorAuxiliaryNamesTbl{};

    std::vector<std::shared_ptr<EffecterAuxiliaryNames>>
        effecterAuxiliaryNamesTbl{};

    /** @brief The sensor aux name from EntityManager configuration PDI */
    std::map<SensorID, AuxiliaryNames> sensorAuxNameOverwriteTbl{};

#ifdef OEM_NVIDIA
    /** @brief The Port information from EntityManager configuration PDI */
    std::map<SensorID, std::tuple<PortType, PortProtocol, uint64_t,
                                  std::vector<dbus::PathAssociation>>>
        sensorPortInfoOverwriteTbl{};
#endif

    std::string systemInventoryPath;

    std::vector<std::tuple<dbus::ObjectPath, EntityType, EntityInstance>>
        inventories;

    std::unique_ptr<sdbusplus::bus::match_t> interfaceAddedMatch;

    EnitityAssociations entityAssociations;

    TerminusManager& terminusManager;

    std::coroutine_handle<> refreshAssociationsTaskHandle;
    void refreshAssociations();
    requester::Coroutine refreshAssociationsTask();
    bool needRefresh;
};
} // namespace platform_mc
} // namespace pldm
