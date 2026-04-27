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
#include "oem_nvidia.hpp"

#include "common/dBusAsyncUtils.hpp"
#include "common/utils.hpp"
#include "mirrorEffecter.hpp"
#include "oem/nvidia/platform-mc/remoteDebug.hpp"
#include "platform-mc/state_sensor.hpp"
#include "platform-mc/state_set/ethIBPortLinkState.hpp"
#include "platform-mc/terminus.hpp"
#include "staticPowerHint.hpp"

#include <phosphor-logging/lg2.hpp>

#include <string_view>

using namespace pldm::pdr;

namespace pldm
{
namespace platform_mc
{

namespace nvidia
{

static void processEffecterPowerCapPdr(Terminus& terminus,
                                       nvidia_oem_effecter_powercap_pdr* pdr,
                                       uint16_t oemRecordId)
{
    auto& effecters = terminus.numericEffecters;

    for (auto it = effecters.begin(); it != effecters.end();)
    {
        auto& effecter = *it;

        // Use the PLDM OEM PDR header's oemRecordId for the match: the firmware
        // stores the real effecter ID there (including any per-CPU prefix),
        // while the vendor-specific associated_effecterid always uses
        // CPU_0-namespace IDs and mismatches for CPU_1.
        if (effecter->effecterId != oemRecordId)
        {
            it++;
            continue;
        }

#ifdef NVIDIA_DISABLE_EDPP_EFFECTERS
        // filter out CPU's EDPp Volatile and EDPp Non-Volatile effecters
        if (pdr->oem_effecter_powercap ==
                static_cast<uint8_t>(
                    OemPowerCapPersistence::OEM_POWERCAP_EDPP_VOLATILE) ||
            pdr->oem_effecter_powercap ==
                static_cast<uint8_t>(
                    OemPowerCapPersistence::OEM_POWERCAP_EDPP_NONVOLATILE))
        {
            it = effecters.erase(it);
            continue;
        }
#endif

        auto persistenceIntf = std::make_unique<OemPersistenceIntf>(
            utils::DBusHandler().getBus(), effecter->path.c_str());
        bool persistence =
            ((pdr->oem_effecter_powercap ==
              static_cast<uint8_t>(
                  OemPowerCapPersistence::OEM_POWERCAP_TDP_NONVOLATILE)) ||
             (pdr->oem_effecter_powercap ==
              static_cast<uint8_t>(
                  OemPowerCapPersistence::OEM_POWERCAP_EDPP_NONVOLATILE)))
                ? true
                : false;
        persistenceIntf->persistent(persistence);
        effecter->oemIntfs.push_back(std::move(persistenceIntf));

        ++it;
    }
}

static void processEffecterStoragePdr(Terminus& terminus,
                                      nvidia_oem_effecter_storage_pdr* pdr,
                                      uint16_t oemRecordId)
{
    for (auto& effecter : terminus.stateEffecters)
    {
        // Same oemRecordId-based match as processEffecterPowerCapPdr.
        if (effecter->effecterId != oemRecordId)
        {
            continue;
        }

        auto secureStateIntf = std::make_unique<OemStorageIntf>(
            utils::DBusHandler().getBus(), effecter->path.c_str());
        bool secureState =
            (pdr->oem_effecter_storage ==
             static_cast<uint8_t>(
                 OemStorageSecureState::OEM_STORAGE_SECURE_VARIABLE))
                ? true
                : false;
        secureStateIntf->secure(secureState);
        effecter->oemIntfs.push_back(std::move(secureStateIntf));
    }
}

void nvidiaInitTerminus(Terminus& terminus)
{
    for (const auto& pdr : terminus.oemPdrs)
    {
        const auto& [iana, recordId, data] = pdr;

        if (iana != NvidiaIana)
        {
            continue;
        }

        if (data.size() < sizeof(nvidia_oem_pdr))
        {
            continue;
        }

        nvidia_oem_pdr* commonPdr = (nvidia_oem_pdr*)data.data();
        NvidiaOemPdrType type =
            static_cast<NvidiaOemPdrType>(commonPdr->oem_pdr_type);

        switch (type)
        {
            case NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_POWERCAP:
                if (data.size() < sizeof(nvidia_oem_effecter_powercap_pdr))
                {
                    continue;
                }
                processEffecterPowerCapPdr(
                    terminus, (nvidia_oem_effecter_powercap_pdr*)commonPdr,
                    recordId);
                break;
            case NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_STORAGE:
                if (data.size() < sizeof(nvidia_oem_effecter_storage_pdr))
                {
                    continue;
                }
                processEffecterStoragePdr(
                    terminus, (nvidia_oem_effecter_storage_pdr*)commonPdr,
                    recordId);
                break;
            default:
                continue;
        }
    }

    // remote debug
    std::shared_ptr<StateEffecter> remoteDebugStateEffecter = nullptr;
    std::shared_ptr<NumericEffecter> remoteDebugNumericEffecter = nullptr;
    std::shared_ptr<StateSensor> remoteDebugStateSensor = nullptr;

    // static power hint
    std::shared_ptr<NumericEffecter> staticPowerHintTemperatureEffecter =
        nullptr;
    std::shared_ptr<NumericEffecter> staticPowerHintWorkloadFactorEffecter =
        nullptr;
    std::shared_ptr<NumericEffecter> staticPowerHintCpuClockFrequencyEffecter =
        nullptr;
    std::shared_ptr<NumericEffecter> staticPowerHintPowerEstimationEffecter =
        nullptr;

    for (auto effecter : terminus.stateEffecters)
    {
        auto& [entityInfo, stateSets] = effecter->effecterInfo;
        if (stateSets.size() == 6 &&
            std::get<0>(stateSets[0]) == PLDM_NVIDIA_OEM_STATE_SET_DEBUG_STATE)
        {
            remoteDebugStateEffecter = effecter;
            break;
        }
    }

    for (auto effecter : terminus.numericEffecters)
    {
        auto& [containerId, entityType, entityInstance] = effecter->entityInfo;
        if (entityType == PLDM_OEM_ENTITY_TYPE_MIRROR)
        {
            NumericEffecterValueInft::createMirrorValueIntf(*effecter);
        }
        else if (effecter->getBaseUnit() == PLDM_SENSOR_UNIT_MINUTES &&
                 entityType == PLDM_ENTITY_SYS_BOARD)
        {
            remoteDebugNumericEffecter = effecter;
        }
        else if (effecter->getBaseUnit() == PLDM_SENSOR_UNIT_WATTS &&
                 entityType == PLDM_ENTITY_SYS_BOARD)
        {
            staticPowerHintPowerEstimationEffecter = effecter;
        }
        else if (effecter->getBaseUnit() == PLDM_SENSOR_UNIT_NONE &&
                 entityType == PLDM_ENTITY_SYS_BOARD)
        {
            staticPowerHintWorkloadFactorEffecter = effecter;
        }
        else if (effecter->getBaseUnit() == PLDM_SENSOR_UNIT_DEGRESS_C &&
                 entityType == PLDM_ENTITY_SYS_BOARD)
        {
            staticPowerHintTemperatureEffecter = effecter;
        }
        else if (effecter->getBaseUnit() == PLDM_SENSOR_UNIT_HERTZ &&
                 entityType == PLDM_ENTITY_SYS_BOARD)
        {
            staticPowerHintCpuClockFrequencyEffecter = effecter;
        }
    }

    for (auto sensor : terminus.stateSensors)
    {
        auto& [entityInfo, stateSets] = sensor->sensorInfo;
        if (stateSets.size() == 6 &&
            std::get<0>(stateSets[0]) == PLDM_NVIDIA_OEM_STATE_SET_DEBUG_STATE)
        {
            remoteDebugStateSensor = sensor;
            break;
        }
    }

    if (remoteDebugStateEffecter != nullptr ||
        remoteDebugNumericEffecter != nullptr ||
        remoteDebugStateSensor != nullptr)
    {
        if (remoteDebugStateEffecter == nullptr)
        {
            lg2::error("Cannot find remote debug state effecter");
        }

        if (remoteDebugNumericEffecter == nullptr)
        {
            lg2::error("Cannot find remote debug timeout effecter");
        }

        if (remoteDebugStateSensor == nullptr)
        {
            lg2::error("Cannot find remote debug state sensor");
        }
    }

    if (remoteDebugNumericEffecter && remoteDebugStateEffecter &&
        remoteDebugStateSensor)
    {
        auto& bus = pldm::utils::DBusHandler::getBus();
        auto remoteDebugIntf = std::make_unique<oem_nvidia::OemRemoteDebugIntf>(
            bus, remoteDebugStateEffecter->path.c_str(),
            remoteDebugStateEffecter, remoteDebugNumericEffecter,
            remoteDebugStateSensor);
        remoteDebugStateEffecter->oemIntfs.push_back(
            std::move(remoteDebugIntf));
    }

    if (staticPowerHintTemperatureEffecter != nullptr ||
        staticPowerHintWorkloadFactorEffecter != nullptr ||
        staticPowerHintCpuClockFrequencyEffecter != nullptr ||
        staticPowerHintPowerEstimationEffecter != nullptr)
    {
        if (staticPowerHintTemperatureEffecter == nullptr)
        {
            lg2::error("Cannot find static power hint Temperature effecter");
        }

        if (staticPowerHintWorkloadFactorEffecter == nullptr)
        {
            lg2::error("Cannot find static power hint WorkloadFactor effecter");
        }

        if (staticPowerHintCpuClockFrequencyEffecter == nullptr)
        {
            lg2::error(
                "Cannot find static power hint CpuClockFrequency effecter");
        }

        if (staticPowerHintPowerEstimationEffecter == nullptr)
        {
            lg2::error("Cannot find static power hint power effecter");
        }
    }

    if (staticPowerHintTemperatureEffecter &&
        staticPowerHintWorkloadFactorEffecter &&
        staticPowerHintCpuClockFrequencyEffecter &&
        staticPowerHintPowerEstimationEffecter)
    {
        auto staticPowerHintPowerEstimation =
            std::make_shared<OemStaticPowerHintInft>(
                utils::DBusHandler().getBus(),
                staticPowerHintPowerEstimationEffecter->path.c_str(),
                staticPowerHintCpuClockFrequencyEffecter,
                staticPowerHintTemperatureEffecter,
                staticPowerHintWorkloadFactorEffecter,
                staticPowerHintPowerEstimationEffecter);
        staticPowerHintPowerEstimationEffecter->oemIntfs.push_back(
            std::move(staticPowerHintPowerEstimation));
    }
}

std::shared_ptr<pldm_oem_energycount_numeric_sensor_value_pdr>
    parseOEMEnergyCountNumericSensorPDR(const std::vector<uint8_t>& vendorData)
{
    const uint8_t* ptr = vendorData.data();
    auto parsedPdr =
        std::make_shared<pldm_oem_energycount_numeric_sensor_value_pdr>();

    size_t expectedPDRSize =
        PLDM_PDR_OEM_ENERGYCOUNT_NUMERIC_SENSOR_PDR_MIN_LENGTH;
    if (vendorData.size() < expectedPDRSize)
    {
        lg2::error(
            "parseOEMEnergyCountNumericSensorPDR() Corrupted PDR, size={PDRSIZE}",
            "PDRSIZE", vendorData.size());
        return nullptr;
    }

    size_t count = (uint8_t*)(&parsedPdr->max_readable.value_u8) -
                   (uint8_t*)(&parsedPdr->terminus_handle);
    memcpy(&parsedPdr->terminus_handle, ptr, count);
    ptr += count;

    expectedPDRSize -=
        PLDM_PDR_OEM_ENERGYCOUNT_NUMERIC_SENSOR_PDR_VARIED_MIN_LENGTH;
    switch (parsedPdr->sensor_data_size)
    {
        case PLDM_SENSOR_DATA_SIZE_UINT8:
        case PLDM_SENSOR_DATA_SIZE_SINT8:
            expectedPDRSize += 2 * sizeof(uint8_t);
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT16:
        case PLDM_SENSOR_DATA_SIZE_SINT16:
            expectedPDRSize += 2 * sizeof(uint16_t);
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT32:
        case PLDM_SENSOR_DATA_SIZE_SINT32:
            expectedPDRSize += 2 * sizeof(uint32_t);
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT64:
        case PLDM_SENSOR_DATA_SIZE_SINT64:
            expectedPDRSize += 2 * sizeof(uint64_t);
            break;
        default:
            break;
    }

    if (vendorData.size() < expectedPDRSize)
    {
        lg2::error(
            "parseOEMEnergyCountNumericSensorPDR() Corrupted PDR, size={PDRSIZE}",
            "PDRSIZE", vendorData.size());
        return nullptr;
    }

    switch (parsedPdr->sensor_data_size)
    {
        case PLDM_SENSOR_DATA_SIZE_UINT8:
        case PLDM_SENSOR_DATA_SIZE_SINT8:
            parsedPdr->max_readable.value_u8 = utils::readLEValue<uint8_t>(ptr);
            parsedPdr->min_readable.value_u8 = utils::readLEValue<uint8_t>(ptr);
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT16:
        case PLDM_SENSOR_DATA_SIZE_SINT16:
            parsedPdr->max_readable.value_u16 =
                utils::readLEValue<uint16_t>(ptr);
            parsedPdr->min_readable.value_u16 =
                utils::readLEValue<uint16_t>(ptr);
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT32:
        case PLDM_SENSOR_DATA_SIZE_SINT32:
            parsedPdr->max_readable.value_u32 =
                utils::readLEValue<uint32_t>(ptr);
            parsedPdr->min_readable.value_u32 =
                utils::readLEValue<uint32_t>(ptr);
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT64:
        case PLDM_SENSOR_DATA_SIZE_SINT64:
            parsedPdr->max_readable.value_u64 =
                utils::readLEValue<uint64_t>(ptr);
            parsedPdr->min_readable.value_u64 =
                utils::readLEValue<uint64_t>(ptr);
            break;
        default:
            break;
    }

    return parsedPdr;
}

static void setPortProtocol(StateSetEthIBPortLinkState* state,
                            std::string configPortProtocol,
                            EntityType portEntityType)
{
    if (configPortProtocol.length())
    {
        auto portProtocol =
            PortInfoIntf::convertPortProtocolFromString(configPortProtocol);
        state->setPortProtocolValue(portProtocol);
    }
    else
    {
        switch (portEntityType)
        {
            case PLDM_ENTITY_ETHERNET:
                state->setPortProtocolValue(PortProtocol::Ethernet);
                break;
            case PLDM_ENTITY_INFINIBAND:
                state->setPortProtocolValue(PortProtocol::InfiniBand);
                break;
            default:
                lg2::debug("Unsupported PortProtocol : {PORT_PROTOCOL}",
                           "PORT_PROTOCOL", std::to_string(portEntityType));
                break;
        }
    }
}

exec::task<int> nvidiaUpdateAssociations(Terminus& terminus)
{
    for (auto sensor : terminus.stateSensors)
    {
        auto& [entityInfo, stateSets] = sensor->sensorInfo;
        auto sensorPortInfo = terminus.getSensorPortInfo(sensor->sensorId);
        if (sensorPortInfo != NULL &&
            (std::get<1>(entityInfo) == PLDM_ENTITY_ETHERNET ||
             std::get<1>(entityInfo) == PLDM_ENTITY_INFINIBAND))
        {
            for (auto& stateSet : sensor->stateSets)
            {
                if (stateSet == nullptr)
                {
                    continue;
                }

                if (stateSet->getStateSetId() == PLDM_STATESET_ID_LINKSTATE)
                {
                    StateSetEthIBPortLinkState* ptr =
                        dynamic_cast<StateSetEthIBPortLinkState*>(
                            stateSet.get());

                    ptr->setPortTypeValue(get<0>(*sensorPortInfo));
                    setPortProtocol(ptr, std::get<1>(*sensorPortInfo),
                                    std::get<1>(entityInfo));

                    // convert MBps to Gbps then assign to maxSpeed
                    double maxSpeedInGbps =
                        (double)((get<2>(*sensorPortInfo) / 1000.0) * 8);
                    ptr->setMaxSpeedValue(maxSpeedInGbps);

                    std::vector<dbus::PathAssociation> associations =
                        get<3>(*sensorPortInfo);
                    ptr->addAssociation(associations);

                    for (const auto& association : associations)
                    {
                        if (association.forward == "associated_port" &&
                            association.reverse == "associated_port")
                        {
                            ptr->addSharedMemObjectPath(association.path);
                            break;
                        }
                    }

                    if (terminus.switchBandwidthSensor &&
                        !ptr->isDerivedSensorAssociated())
                    {
                        ptr->associateDerivedSensor(
                            terminus.switchBandwidthSensor);
                        terminus.switchBandwidthSensor->updateMaxBandwidth(
                            maxSpeedInGbps);
                        terminus.switchBandwidthSensor->addAssociatedSensorID(
                            sensor->sensorId);
                    }
                }
            }
        }
        else if (std::get<1>(entityInfo) == PLDM_ENTITY_MEMORY_CONTROLLER)
        {
            for (auto& stateSet : sensor->stateSets)
            {
                if (stateSet == nullptr)
                {
                    continue;
                }

                if (stateSet->getStateSetId() == PLDM_STATESET_ID_PERFORMANCE)
                {
                    // Walk up entity tree to find the Processor I/O Module
                    // ancestor and use its instance as the ProcessorModule
                    // index. On multi-terminus platforms (e.g. vr-nvl-hmc), PDR
                    // instances are all 0 so getInstance() provides the correct
                    // override. On single-terminus platforms (e.g.
                    // gb200nvl-hmc), the PDR itself has distinct instances per
                    // Proc I/O Module.
                    std::optional<uint16_t> procModuleInstance;
                    // Start from the memory controller's parent container
                    auto parentContainerId = std::get<0>(entityInfo);
                    while (auto ancestor =
                               terminus.getContainerEntity(parentContainerId))
                    {
                        auto entityType = std::get<1>(*ancestor) & 0x7FFF;
                        if (entityType == PLDM_ENTITY_PROC_IO_MODULE)
                        {
                            auto pdrInstance = std::get<2>(*ancestor);
                            procModuleInstance =
                                terminus.getInstance().value_or(pdrInstance);
                            break;
                        }
                        // Move up to the next ancestor
                        parentContainerId = std::get<0>(*ancestor);
                    }

                    if (!procModuleInstance)
                    {
                        lg2::error(
                            "Cannot find ProcessorModule for memory performance sensor on TID {TID}",
                            "TID", terminus.getTid());
                        // Without a Processor I/O Module ancestor there is no
                        // stable ProcessorModule index to build associations
                        // from, so skip this sensor instead of dereferencing a
                        // missing instance.
                        continue;
                    }

                    std::string prefix =
                        "ProcessorModule_" +
                        std::to_string(*procModuleInstance) + "_Memory_";

                    auto getSubTreeResponse = co_await utils::coGetSubTree(
                        "/xyz/openbmc_project/inventory", 0,
                        {"xyz.openbmc_project.Inventory.Item.Dimm"});

                    std::vector<dbus::PathAssociation> assocs;
                    for (const auto& [objPath, mapperServiceMap] :
                         getSubTreeResponse)
                    {
                        sdbusplus::message::object_path path(objPath);
                        std::string filename = path.filename();
                        if (filename.starts_with(prefix))
                        {
                            assocs.push_back({"memory", "all_states", objPath});
                        }
                    }

                    stateSet->setAssociation(assocs);
                }
            }
        }

        auto sensorEventInfo = terminus.getSensorEventInfo(sensor->sensorId);
        if (sensorEventInfo)
        {
            sensor->updateSensorEventInfo(sensorEventInfo);
        }
    }

    constexpr std::string_view cpuPrimaryTempSensor(CPU_PRIMARY_TEMP_SENSOR);
    for (auto sensor : terminus.numericSensors)
    {
        auto sensorEventInfo = terminus.getSensorEventInfo(sensor->sensorId);
        if (sensorEventInfo)
        {
            sensor->updateSensorEventInfo(sensorEventInfo);
        }

        if (cpuPrimaryTempSensor.empty() ||
            sensor->getSensorName().find(cpuPrimaryTempSensor) ==
                std::string::npos ||
            !sensor->associationDefinitionsIntf)
        {
            continue;
        }

        auto assocs = sensor->associationDefinitionsIntf->associations();
        bool updateRequired = false;
        const auto count = assocs.size();
        for (size_t i = 0; i < count; ++i)
        {
            const auto& [fwd, rev, path] = assocs[i];
            if (fwd == "chassis" && rev == "all_sensors")
            {
                assocs.emplace_back(std::make_tuple(
                    "chassis", "primary_temperature_sensor", path));
                updateRequired = true;
            }
        }
        if (updateRequired)
        {
            sensor->associationDefinitionsIntf->associations(assocs);
        }
    }

    // Add primary_power_sensor association for the CPU EnforcedEDPc
    // numeric sensor, pointing at the non-HGX CPU inventory path that the
    // generic layer has already associated via chassis/all_sensors.
    constexpr std::string_view cpuPrimaryPowerSensor(CPU_PRIMARY_POWER_SENSOR);
    for (auto sensor : terminus.numericSensors)
    {
        if (cpuPrimaryPowerSensor.empty() ||
            sensor->getSensorName().find(cpuPrimaryPowerSensor) ==
                std::string::npos ||
            !sensor->associationDefinitionsIntf)
        {
            continue;
        }

        auto assocs = sensor->associationDefinitionsIntf->associations();
        bool updateRequired = false;
        const auto count = assocs.size();
        for (size_t i = 0; i < count; ++i)
        {
            const auto& [fwd, rev, path] = assocs[i];
            if (fwd != "chassis" || rev != "all_sensors")
            {
                continue;
            }
            const auto slash = path.rfind('/');
            const std::string_view base =
                (slash == std::string::npos)
                    ? std::string_view{path}
                    : std::string_view{path}.substr(slash + 1);
            if (base.starts_with("HGX_"))
            {
                continue;
            }
            std::tuple<std::string, std::string, std::string> newAssoc{
                "chassis", "primary_power_sensor", path};
            if (std::find(assocs.begin(), assocs.end(), newAssoc) ==
                assocs.end())
            {
                assocs.emplace_back(std::move(newAssoc));
                updateRequired = true;
            }
        }
        if (updateRequired)
        {
            sensor->associationDefinitionsIntf->associations(assocs);
        }
    }

    constexpr std::string_view cpuPrimaryPowerControl(
        CPU_PRIMARY_POWER_CONTROL);
    for (auto effecter : terminus.numericEffecters)
    {
        if (cpuPrimaryPowerControl.empty() || !effecter->hasAssociationIntf())
        {
            continue;
        }

        sdbusplus::message::object_path effecterObjPath(effecter->path);
        if (effecterObjPath.filename().find(cpuPrimaryPowerControl) ==
            std::string::npos)
        {
            continue;
        }

        auto assocs = effecter->getAssociation();
        bool updateRequired = false;
        const auto count = assocs.size();
        for (size_t i = 0; i < count; ++i)
        {
            const auto& [fwd, rev, path] = assocs[i];
            if (fwd == "chassis" && rev == "power_controls")
            {
                assocs.emplace_back(
                    std::make_tuple("chassis", "primary_power_control", path));
                updateRequired = true;
            }
        }
        if (updateRequired)
        {
            effecter->setAssociation(assocs);
        }
    }

    co_return PLDM_SUCCESS;
}

} // namespace nvidia
} // namespace platform_mc
} // namespace pldm
