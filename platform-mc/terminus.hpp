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
#include "libpldm/pldm_types.h"

#ifdef OEM_NVIDIA
#include "libpldm/oem/nvidia/energy_count_numeric_sensor_oem.h"

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
 * @brief TerminusResumptionStatus
 *
 * This struct holds the present status of the terminus resumption process
 */
struct TerminusResumptionStatus
{
    TerminusResumptionStatus() : eventReciever{PLDM_SUCCESS} {}

    uint8_t eventReciever;
};

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

    /** @brief Incrementally apply PDRs reported as added by a
     *         PDRRepositoryChgEvent. Retains each raw PDR, parses it into the
     *         typed caches, and creates D-Bus objects ONLY for the newly added
     *         entries (does not touch or duplicate existing objects). The
     *         caller should run updateAssociations()/scanInventories() after
     *         this when entity-association PDRs were among the new records.
     *
     *  @param[in] newPdrs - raw PDRs newly fetched by record handle
     *  @return number of PDRs actually applied (i.e. not skipped as
     *          already-present); 0 means nothing changed.
     */
    size_t addNewPdrs(const std::vector<std::vector<uint8_t>>& newPdrs);

    /** @brief Remove the D-Bus objects, parsed-PDR cache entries and raw PDRs
     *         for a set of MODIFIED PDRs, so addNewPdrs() can recreate them
     *         cleanly from the freshly fetched bytes.
     *
     *  Handles the PDR types that map 1:1 to a single object: numeric/state
     *  sensors, numeric/state effecters, and the OEM energy-count numeric
     *  sensor. The caller (guarded by canReplacePdrIncrementally) routes every
     *  other PDR type -- auxiliary names and entity association (consumed by
     *  many objects), or non-energy-count OEM -- to a full rebuild instead.
     *
     *  IMPORTANT: the caller MUST first quiesce sensor polling and release the
     *  prioritySensors / roundRobinSensors references. A sensor object still
     *  held by those lists survives this erase, so its D-Bus interface lingers
     *  and re-creation collides (sd_bus FileExists).
     *
     *  @param[in] modifiedPdrs - raw PDR bytes for each modified record handle
     */
    void removeModifiedPdrObjects(
        const std::vector<std::vector<uint8_t>>& modifiedPdrs);

    /** @brief Whether a MODIFIED PDR maps 1:1 to a single sensor/effecter
     *         object and can therefore be replaced incrementally (vs needing a
     *         full rebuild). True for numeric/state sensor & effecter PDRs and
     *         the OEM energy-count numeric sensor PDR; false for
     * auxiliary-name, entity-association and other OEM PDRs, which are
     * referenced by many objects.
     *
     *  @param[in] pdr - raw PDR bytes
     */
    bool canReplacePdrIncrementally(const std::vector<uint8_t>& pdr);

    /** @brief Return the OEM PDR sub-type byte (NvidiaOemPdrType) for an OEM
     *         PDR, or -1 if the PDR is not an OEM PDR or is too short to
     *         classify. Diagnostic helper for logging which sub-type forced a
     *         full rebuild.
     *
     *  @param[in] pdr - raw PDR bytes
     */
    int oemPdrSubType(const std::vector<uint8_t>& pdr);

    /** @brief Whether platform-mc consumes a PDR of this type, i.e. whether
     *         parseOnePdrIntoCache() parses it into a cache and derives a
     *         D-Bus object/association from it. Types not consumed (e.g.
     *         PLDM_OEM_DEVICE_PDR) have no derived object, so a MODIFIED
     *         record for them requires neither an incremental replace nor a
     *         full rebuild.
     *
     *  Keep in sync with parseOnePdrIntoCache().
     *
     *  @param[in] type - PDR header type byte
     */
    static bool pdrTypeConsumed(uint8_t type);

    /** @brief Whether the object/cache entry a PDR derives is already present,
     *         keyed by the derived identity (sensor/effecter id, or aux-name
     *         target id) rather than record handle. A terminus that rebuilds
     *         its repository across a power-cycle re-reports existing sensors
     *         as ADDED (new record handles, same sensor id); creating them
     *         again collides on D-Bus (FileExists), so addNewPdrs() skips any
     *         PDR for which this returns true.
     *
     *  @param[in] pdr - raw PDR bytes
     */
    bool isPdrAlreadyApplied(const std::vector<uint8_t>& pdr);

    /** @brief Clear every parsed-PDR cache (numeric/state sensor & effecter
     *         PDRs, auxiliary-name tables, OEM PDRs, entity map).
     *
     *  parsePDRs() APPENDS to these caches and then creates a D-Bus object per
     *  cache entry. A full re-discovery must reset them first, otherwise
     *  re-init re-parses on top of the stale entries and creates every object
     *  twice (the duplicate collides on D-Bus with FileExists). Does NOT touch
     *  the live sensor/effecter object vectors — the caller clears those (after
     *  quiescing polling) so destruction order is controlled.
     */
    void clearParsedPdrCaches();

    /** @brief Parse a single raw PDR into the typed caches (one iteration of
     *         parsePDRs' first pass). Does NOT create D-Bus objects.
     *
     *  @return False if the PDR type is not recognized.
     */
    bool parseOnePdrIntoCache(std::vector<uint8_t>& pdr);

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

    /** @brief Derive a CPU/ProcessorModule entity instance from the entity
     *         association PDRs when no EID-to-terminus static config instance
     *         is available. Walks from the given container up to the owning
     *         ProcessorModule (PLDM_ENTITY_PROC_IO_MODULE) whose instance
     *         uniquely identifies CPU_0 vs CPU_1, even when a single terminus
     *         reports both CPUs.
     *
     *  @param[in] containerId - container ID of the entity being resolved
     *  @return the ProcessorModule instance number, or std::nullopt when no
     *          ProcessorModule is found in the hierarchy
     */
    std::optional<uint16_t> getInstanceFromAssoicationPdr(
        ContainerID containerId);

    /** @brief Get the container EntityInfo for a given container ID */
    std::optional<EntityInfo> getContainerEntity(ContainerID containerId) const
    {
        auto itr = entityAssociations.find(containerId);
        if (itr != entityAssociations.end())
        {
            return itr->second.first;
        }
        return std::nullopt;
    }

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

    /** @brief Return code of the sensor polling task */
    std::optional<int> sensorPollingTaskRc{0};

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

    /** @brief Get inentory path by sensorID
     *
     *  @param[in] id - sensor ID
     *  @return inventory path name
     */
    std::optional<ParentObjPath> getInventoryPath(SensorID id);

    /** @brief Get Effecter Auxiliary Names by effecterID
     *
     *  @param[in] id - effecter ID
     *  @return effecter auxiliary names
     */
    std::shared_ptr<EffecterAuxiliaryNames> getEffecterAuxiliaryNames(
        EffecterID id);

#ifdef OEM_NVIDIA
    /** @brief Get Sensor Port type by sensorID
     *
     *  @param[in] id - sensor ID
     *  @return sensor port types
     */
    std::shared_ptr<std::tuple<PortType, std::string, uint64_t,
                               std::vector<dbus::PathAssociation>>>
        getSensorPortInfo(SensorID id);

    /** @brief Get Sensor Event Info by sensorID
     *
     *  @param[in] id - sensor ID
     *  @return sensor event info
     */
    std::shared_ptr<utils::SensorEventInfo> getSensorEventInfo(SensorID id);

    std::shared_ptr<oem_nvidia::SwitchBandwidthSensor> switchBandwidthSensor =
        nullptr;
#endif

    void parseEntityAssociationPDR(const std::vector<uint8_t>& pdrData);

    exec::task<int> getInventoryParent(const std::string objPath);

    exec::task<int> scanInventories();

    exec::task<int> updateAssociations();

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
    exec::task<int> checkDeviceInventory(const std::string& objPath);
    bool checkNsmDeviceInventory(UUID nsmUuid);

    /** @brief get Sensor Aux Name from EM configuration PDI
     *
     *  @param[in] objPath - device inventory path
     */
    exec::task<int> getSensorAuxNameFromEM(
        uint8_t bus, uint8_t addr, uint8_t eid, const std::string& objPath);

#ifdef OEM_NVIDIA
    /** @brief get sensor Port information from EM configuration PDI
     *
     *  @param[in] objPath - device inventory path
     */
    exec::task<int> getPortInfoFromEM(const std::string& objPath);
    exec::task<int> getInfoForNVSwitchFromEM(const std::string& objPath);
    exec::task<int> getSensorEventInfoFromEM(const std::string& objPath);
#endif

    /** @brief The flag indicates whether the terminus has been initialized
     * by terminusManaer */
    bool initalized;

    /** @brief The flag indicates that the terminus FIFO contains a large
     * message that will require a multipart transfer via the
     * PollForPlatformEvent command */
    bool pollEvent;

    /** @brief dataTransferHandle advertised by the most recent
     * pldmMessagePollEvent. Used as the starting dataTransferHandle for the
     * first PollForPlatformEventMessage request (the FIFO does not necessarily
     * start at handle 0). */
    uint32_t pollDataTransferHandle;

    /** @brief The flag indicates that the terminus is ready ( i.e All of it's
     * round robin sensors were checked at least once ) */
    bool ready;

    /** @brief This value indicates the event messaging styles supported by the
     * terminus */
    bitfield8_t synchronyConfigurationSupported;

    /** @brief This value indicates if the terminus is resumed successfully */
    bool resumed;

    /** @brief This value indicates if polling sensor list need to be
     * initialized */
    bool initSensorList;

    /** @brief This struct holds the present status of the terminus resumption
     * process */
    TerminusResumptionStatus resumptionStatus{};

    /** @brief set the terminus to online state */
    void setOnline();

    /** @brief set the terminus to offline state */
    void setOffline();

    const UUID& getUuid()
    {
        return uuid;
    }

    /** @brief The setter to set terminus's name */
    void setTerminusName(const std::string& tName)
    {
        terminusName = tName;
    }

    /** @brief The getter to get terminus's name */
    std::optional<std::string_view> getTerminusName()
    {
        if (terminusName.empty())
        {
            return std::nullopt;
        }
        return terminusName;
    }

    /** @brief The setter to set terminus's instance */
    void setInstance(uint16_t inst)
    {
        instance = inst;
    }

    /** @brief The getter to get terminus's instance */
    std::optional<uint16_t> getInstance()
    {
        return instance;
    }

    /** @brief The setter to set terminus's CPU index */
    void setCpuIndex(uint16_t idx)
    {
        cpuIndex = idx;
    }

    /** @brief The getter to get terminus's CPU index */
    std::optional<uint16_t> getCpuIndex()
    {
        return cpuIndex;
    }

    /** @brief Trigger a deferred association refresh if one was queued while
     *         the terminus was still initializing.
     */
    void applyPendingRefresh()
    {
        if (needRefresh)
        {
            refreshAssociations();
        }
    }

  private:
    std::shared_ptr<pldm_numeric_sensor_value_pdr> parseNumericSensorPDR(
        const std::vector<uint8_t>& pdrData);

    std::shared_ptr<pldm_numeric_effecter_value_pdr> parseNumericEffecterPDR(
        const std::vector<uint8_t>& pdrData);

    std::shared_ptr<SensorAuxiliaryNames> parseSensorAuxiliaryNamesPDR(
        const std::vector<uint8_t>& pdrData);

    std::shared_ptr<EffecterAuxiliaryNames> parseEffecterAuxiliaryNamesPDR(
        const std::vector<uint8_t>& pdrData);

    std::tuple<SensorID, StateSetInfo> parseStateSensorPDR(
        std::vector<uint8_t>& pdr);

    void parseStateSetInfo(const unsigned char* statesPtr,
                           uint8_t compositeSensorCount,
                           std::vector<StateSetData>& stateSets);

    std::tuple<EffecterID, StateSetInfo> parseStateEffecterPDR(
        std::vector<uint8_t>& stateEffecterPdr);

    OemPdr parseOemPDR(const std::vector<uint8_t>& oemPdr);

    /** @brief Convert EntityType to PhysicalContextType
     *
     *  @param[in] EntityType - entityType
     *  @return PhysicalContextType
     */
    PhysicalContextType toPhysicalContextType(const EntityType entityType);

    std::optional<std::string> getAuxNameForNumericSensor(SensorID id);

    /** @brief Build sensor name prefix using terminusName, entity type string,
     *         and cpuIndex. Returns "{terminusName}_{EntityTypeStr}_{cpuIndex}"
     *         when entity type is mapped, or "{terminusName}" otherwise.
     *
     *  @param[in] entityType - raw entity type from PDR (physical bit included)
     *  @return string prefix to prepend to the aux name
     */
    std::string buildSensorNamePrefix(uint16_t entityType) const;

    /** @brief Build entity-type tag without terminus name
     *
     *  Returns just the entity-type name + optional CPU index
     *  (e.g. "CPU_0"), without the terminus name prefix.
     *  Used for backward-compat detection of old firmware aux names.
     *
     *  @param[in] entityType - raw entity type from PDR (physical bit included)
     *  @return entity tag string, or empty if entity type is unknown
     */
    std::string buildEntityTypeTag(uint16_t entityType) const;

    tid_t tid;
    std::bitset<64> supportedTypes;

    UUID uuid;

    /** @brief Terminus name from configuration */
    std::string terminusName;

    /** @brief Terminus instance from configuration */
    std::optional<uint16_t> instance;

    /** @brief CPU index from configuration */
    std::optional<uint16_t> cpuIndex;

    std::vector<std::shared_ptr<SensorAuxiliaryNames>>
        sensorAuxiliaryNamesTbl{};

    std::vector<std::shared_ptr<EffecterAuxiliaryNames>>
        effecterAuxiliaryNamesTbl{};

    /** @brief The sensor aux name from EntityManager configuration PDI */
    std::map<SensorID, std::tuple<AuxiliaryNames, ParentObjPath>>
        sensorAuxNameOverwriteTbl{};

#ifdef OEM_NVIDIA
    /** @brief The Port information from EntityManager configuration PDI */
    std::map<SensorID, std::tuple<PortType, std::string, uint64_t,
                                  std::vector<dbus::PathAssociation>>>
        sensorPortInfoOverwriteTbl{};

    /**
     * @brief Table mapping SensorID to SensorEventInfo
     */
    std::map<SensorID, std::shared_ptr<utils::SensorEventInfo>>
        sensorEventInfoOverwriteTbl{};
#endif

    std::string systemInventoryPath;

    std::vector<std::tuple<dbus::ObjectPath, EntityType, EntityInstance>>
        inventories;

    std::map<dbus::ObjectPath, dbus::ObjectPath> inventoryParentMap;

    std::unique_ptr<sdbusplus::bus::match_t> interfaceAddedMatch;

    EnitityAssociations entityAssociations;

    TerminusManager& terminusManager;

    std::optional<std::pair<exec::async_scope, std::optional<int>>>
        refreshAssociationsTaskHandle;
    void refreshAssociations();
    exec::task<int> refreshAssociationsTask();
    bool needRefresh;
};
} // namespace platform_mc
} // namespace pldm
