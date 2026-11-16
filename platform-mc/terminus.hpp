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

#include "oem/nvidia/common/em_device.hpp"
#include "oem/nvidia/platform-mc/derived_sensor/switchBandwidthSensor.hpp"
#include "oem/nvidia/platform-mc/inventory_manager.hpp"
#endif

#include "common/types.hpp"
#include "numeric_effecter.hpp"
#include "numeric_sensor.hpp"
#include "state_effecter.hpp"
#include "state_sensor.hpp"

#include <sdbusplus/server/object.hpp>
#include <sdeventplus/event.hpp>
#include <xyz/openbmc_project/Association/Definitions/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/PortInfo/server.hpp>
#include <xyz/openbmc_project/Inventory/Decorator/PortState/server.hpp>
#include <xyz/openbmc_project/Inventory/Item/NetworkInterface/server.hpp>

#include <coroutine>
#include <set>
#include <utility>

using namespace pldm::pdr;

namespace pldm
{
namespace platform_mc
{

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

/**
 * @brief TerminusResumptionStatus
 *
 * This struct holds the present status of the terminus resumption process
 */
struct TerminusResumptionStatus
{
    TerminusResumptionStatus() : eventReceiver{PLDM_SUCCESS} {}

    uint8_t eventReceiver;
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

    /** @brief Replace the cached PLDM types with a fresh GetPLDMTypes result
     *
     *  @param[in] types - PLDM type bitmap returned by GetPLDMTypes
     *  @return Bitmap of the types that were added or removed, 0 if the
     *          terminus still supports the same types
     */
    uint64_t updateSupportedTypes(uint64_t types);

    /** @brief Whether any sensor or effecter entities are present. */
    bool hasEntities() const;

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

    /** @brief A list of parsed state effecter PDRs */
    std::vector<std::tuple<EffecterID, StateSetInfo>> stateEffecterPdrs{};

    /** @brief Get Sensor Auxiliary Names by sensorID
     *
     *  @param[in] id - sensor ID
     *  @return sensor auxiliary names
     */
    std::shared_ptr<SensorAuxiliaryNames> getSensorAuxiliaryNames(SensorID id);

    /** @brief Get inventory path by sensorID
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

    /** @brief Feed an Entity Association PDR to the terminus's entity tree
     *         in InventoryManager. */
    void parseEntityAssociationPDR(const std::vector<uint8_t>& pdrData);

    /** @brief Re-scan the inventory objects that belong to the terminus;
     *         see InventoryManager::scan(). When the scan publishes a new
     *         system inventory, the other termini are re-bound to it. */
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

#ifdef OEM_NVIDIA
    /** @brief Fill the aux-name and port-info tables from a PLDMDevice
     *         record of the device at @p objPath. */
    void loadDeviceRecord(const pldm::em_device::PldmDevice& record,
                          const std::string& objPath);

    /** @brief The NvDevice this terminus was matched to; empty until the
     *         chassis scan (checkDeviceInventory()) found it, and again
     *         once a scan of its chassis no longer matches it. A removed
     *         NvDevice is not forgotten; a stale name only causes extra
     *         refreshes when that device's records reappear. */
    std::string deviceName;

    /** @brief Read sensorRoles anew from the PLDMDevice record of the
     *         NvDevice that is the endpoint of this terminus.
     *
     *  The device is the one with the endpoint's name, which the terminus
     *  config also comes from, else the only one with the endpoint's EID.
     *  Its record is read whatever inventory objects the chassis scan
     *  offers: the inventory of a device need not be chassis/<Name> (a
     *  CPU's is component/<Name>). Called at the start of every
     *  scanInventories().
     */
    void loadSensorRoles();

    /** @brief The SensorRoles of the record loadSensorRoles() read: role
     *         (em_device::primaryTemperatureSensor, ...) -> auxiliary name
     *         of the sensor or effecter that has it, as the device names it
     *         ("TempLimit_0"). The role goes to the sensor or effecter whose
     *         name is sensorNameFor() its entity type and that auxiliary
     *         name; a sensor that the record renames (AuxNames) has no
     *         role. Empty without a record; a role it lacks has no sensor
     *         or effecter. */
    std::map<std::string, std::string> sensorRoles;

    /** @brief The (role, auxiliary name) pairs of sensorRoles that no
     *         sensor or effecter of this terminus had when
     *         nvidiaUpdateAssociations() ran, so that each is logged once. */
    std::set<std::pair<std::string, std::string>> unmatchedSensorRoles;
#endif
    bool checkNsmDeviceInventory(UUID nsmUuid);

    /** @brief get Sensor Aux Name from EM configuration PDI
     *
     *  @param[in] objPath - device inventory path
     *  @param[in] namesFromRecord - a PLDMDevice record supplies the names,
     *             so the absence of SensorAuxName configs is not an error
     */
    exec::task<int> getSensorAuxNameFromEM(
        uint8_t bus, uint8_t addr, uint8_t eid, const std::string& objPath,
        bool namesFromRecord = false);

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
     * by terminusManager */
    bool initialized;

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

    /** @brief Resume operational-state tracking on numeric effecters that
     *         disarmed it, after the terminus restarted its state sequence.
     *
     *  @return number of effecters re-armed
     */
    size_t rearmTrackedEffecters();

    const UUID& getUuid()
    {
        return uuid;
    }

    /** @brief The setter to set terminus's name; also registered with
     *         InventoryManager. */
    void setTerminusName(const std::string& tName)
    {
        terminusName = tName;
#ifdef OEM_NVIDIA
        oem_nvidia::InventoryManager::get().setTerminusName(tid, tName);
#endif
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

    /** @brief Register the terminus's configured instance with
     *         InventoryManager, which matches CPU and ProcessorModule
     *         inventory by it. No-op without OEM_NVIDIA. */
    void setInstance([[maybe_unused]] uint16_t inst)
    {
#ifdef OEM_NVIDIA
        oem_nvidia::InventoryManager::get().setInstance(tid, inst);
#endif
    }

    /** @brief The instance registered with InventoryManager, if any. */
    std::optional<uint16_t> getInstance()
    {
#ifdef OEM_NVIDIA
        return oem_nvidia::InventoryManager::get().instance(tid);
#else
        return std::nullopt;
#endif
    }

    /** @brief The name this terminus gives a sensor or effecter of
     *         @p entityType whose auxiliary name is @p auxName: @p auxName
     *         after the sensor name prefix of the entity type
     *         (buildSensorNamePrefix()), or @p auxName alone when the
     *         terminus has no name.
     *
     *  Old firmware that already puts the entity tag into the auxiliary
     *  name ("CPU_0_TempLimit_0") gets the same name for the plain one.
     */
    std::string sensorNameFor(uint16_t entityType,
                              const std::string& auxName) const;

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

    /** @brief Parse the variable-length name-string entries shared by the
     *         sensor and effecter auxiliary-names PDRs. Walks the
     *         device-supplied buffer [ptr, end) with explicit bounds and
     *         length caps; returns false (caller returns nullptr) on any
     *         malformed, truncated, or over-long field. @a label is used only
     *         for logging.
     */
    bool parseAuxiliaryNameStrings(
        const uint8_t* ptr, const uint8_t* end, uint8_t count, uint16_t id,
        std::vector<std::vector<std::pair<NameLanguageTag, SensorName>>>& out,
        const char* label);

    std::optional<std::tuple<SensorID, StateSetInfo>> parseStateSensorPDR(
        std::vector<uint8_t>& pdr);

    void parseStateSetInfo(const unsigned char* statesPtr,
                           uint8_t compositeSensorCount,
                           const unsigned char* end,
                           std::vector<StateSetData>& stateSets);

    std::optional<std::tuple<EffecterID, StateSetInfo>> parseStateEffecterPDR(
        std::vector<uint8_t>& stateEffecterPdr);

    OemPdr parseOemPDR(const std::vector<uint8_t>& oemPdr);

    /** @brief The inventory an entity associates with: its own, else the
     *         closest ancestor's, else the system inventory. */
    std::vector<std::string> resolveInventory(const EntityInfo& entityInfo);

    /** @brief Apply the numeric sensor's aux name, prefixed with the
     *         terminus name unless it comes from Entity Manager.
     *  @return false if the sensor has no aux name and must be skipped */
    bool updateNumericSensorName(NumericSensor& sensor);

    /** @brief resolveInventory() for a numeric sensor, redirected to the CPU
     *         for ProcessorModule, SYS_BUS and count sensors. */
    std::vector<std::string> numericSensorInventory(NumericSensor& sensor);

    /** @brief Redirect a SYS_BUS or MemoryController state sensor's
     *         @a paths to the CPU it belongs to. */
    void redirectStateSensorInventory(StateSensor& sensor,
                                      std::vector<std::string>& paths);

    /** @brief Set a sensor's inventory paths, gating metric-report updates
     *         when they are a degraded placeholder. An Entity Manager
     *         override (@a hasInventoryOverride) never is one.
     *  @return true if the association is a fallback */
    template <typename Sensor>
    bool applyInventoryAssociation(
        Sensor& sensor, const std::vector<std::string>& inventoryPaths,
        bool hasInventoryOverride);

    void updateNumericSensorAssociations();
    void updateStateSensorAssociations();
    void updateEffecterAssociations();

#ifdef OEM_NVIDIA
    /** @brief Re-run updateAssociations() on every other idle, initialised
     *         terminus with sensors or effecters, after this terminus's scan
     *         published a new system inventory; latch a refresh on those
     *         still initialising or refreshing. It does not scan itself; a
     *         latched refresh does, and finds the same system inventory. */
    exec::task<void> rebindOtherTermini();
#endif

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

    std::unique_ptr<sdbusplus::bus::match_t> interfaceAddedMatch;

    TerminusManager& terminusManager;

    std::optional<std::pair<exec::async_scope, std::optional<int>>>
        refreshAssociationsTaskHandle;
    void refreshAssociations();
    exec::task<int> refreshAssociationsTask();
    bool needRefresh;
};
} // namespace platform_mc
} // namespace pldm
