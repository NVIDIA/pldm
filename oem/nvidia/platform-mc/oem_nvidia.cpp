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
#include "memoryPageRetirementCount.hpp"
#include "oem/nvidia/platform-mc/remoteDebug.hpp"
#include "platform-mc/state_sensor.hpp"
#include "platform-mc/state_set/ethernetPortLinkState.hpp"
#include "platform-mc/terminus.hpp"
#include "staticPowerHint.hpp"

#include <phosphor-logging/lg2.hpp>

using namespace pldm::pdr;

namespace pldm
{
namespace platform_mc
{

namespace nvidia
{

static void processEffecterPowerCapPdr(Terminus& terminus,
                                       nvidia_oem_effecter_powercap_pdr* pdr)
{
    for (auto& effecter : terminus.numericEffecters)
    {
        if (effecter->effecterId != pdr->associated_effecterid)
        {
            continue;
        }

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
    }
}

static void processEffecterStoragePdr(Terminus& terminus,
                                      nvidia_oem_effecter_storage_pdr* pdr)
{
    for (auto& effecter : terminus.stateEffecters)
    {
        if (effecter->effecterId != pdr->associated_effecterid)
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
                    terminus, (nvidia_oem_effecter_powercap_pdr*)commonPdr);
                break;
            case NvidiaOemPdrType::NVIDIA_OEM_PDR_TYPE_EFFECTER_STORAGE:
                if (data.size() < sizeof(nvidia_oem_effecter_storage_pdr))
                {
                    continue;
                }
                processEffecterStoragePdr(
                    terminus, (nvidia_oem_effecter_storage_pdr*)commonPdr);
                break;
            default:
                continue;
        }
    }

    for (auto sensor : terminus.numericSensors)
    {
        auto& [containerId, entityType, entityInstance] = sensor->entityInfo;
        if ((entityType == PLDM_ENTITY_PROC ||
             entityType == PLDM_ENTITY_MEMORY_CONTROLLER) &&
            sensor->getBaseUnit() == PLDM_SENSOR_UNIT_COUNTS)
        {
            auto memoryPageRetirementCount =
                std::make_shared<OemMemoryPageRetirementCountInft>(
                    sensor, utils::DBusHandler().getBus(),
                    sensor->path.c_str());
            sensor->oemIntfs.push_back(std::move(memoryPageRetirementCount));
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
        if (effecter->getBaseUnit() == PLDM_SENSOR_UNIT_MINUTES &&
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
            parsedPdr->max_readable.value_u8 = *((uint8_t*)ptr);
            ptr += sizeof(parsedPdr->max_readable.value_u8);
            parsedPdr->min_readable.value_u8 = *((uint8_t*)ptr);
            ptr += sizeof(parsedPdr->min_readable.value_u8);
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT16:
        case PLDM_SENSOR_DATA_SIZE_SINT16:
            parsedPdr->max_readable.value_u16 = le16toh(*((uint16_t*)ptr));
            ptr += sizeof(parsedPdr->max_readable.value_u16);
            parsedPdr->min_readable.value_u16 = le16toh(*((uint16_t*)ptr));
            ptr += sizeof(parsedPdr->min_readable.value_u16);
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT32:
        case PLDM_SENSOR_DATA_SIZE_SINT32:
            parsedPdr->max_readable.value_u32 = le32toh(*((uint32_t*)ptr));
            ptr += sizeof(parsedPdr->max_readable.value_u32);
            parsedPdr->min_readable.value_u32 = le32toh(*((uint32_t*)ptr));
            ptr += sizeof(parsedPdr->min_readable.value_u32);
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT64:
        case PLDM_SENSOR_DATA_SIZE_SINT64:
            parsedPdr->max_readable.value_u64 = le64toh(*((uint64_t*)ptr));
            ptr += sizeof(parsedPdr->max_readable.value_u64);
            parsedPdr->min_readable.value_u64 = le64toh(*((uint64_t*)ptr));
            ptr += sizeof(parsedPdr->min_readable.value_u64);
            break;
        default:
            break;
    }

    return parsedPdr;
}

static std::string cpuNameToMemoryName(const std::string& cpuName)
{
    // The sensors are associated to CPU by default based on contained id. Using
    // it to associate with corresponding Memory.
    if (cpuName == "HGX_CPU_0" || cpuName == "CPU_0")
    {
        return "ProcessorModule_0_Memory_0";
    }
    if (cpuName == "HGX_CPU_1" || cpuName == "CPU_1")
    {
        return "ProcessorModule_1_Memory_0";
    }
    return "";
}

requester::Coroutine nvidiaUpdateAssociations(Terminus& terminus)
{
    for (auto sensor : terminus.stateSensors)
    {
        auto& [entityInfo, stateSets] = sensor->sensorInfo;
        auto sensorPortInfo = terminus.getSensorPortInfo(sensor->sensorId);
        if (sensorPortInfo != NULL &&
            std::get<1>(entityInfo) == PLDM_ENTITY_ETHERNET)
        {
            for (auto& stateSet : sensor->stateSets)
            {
                if (stateSet == nullptr)
                {
                    continue;
                }

                if (stateSet->getStateSetId() == PLDM_STATESET_ID_LINKSTATE)
                {
                    StateSetEthernetPortLinkState* ptr =
                        dynamic_cast<StateSetEthernetPortLinkState*>(
                            stateSet.get());

                    ptr->setPortTypeValue(get<0>(*sensorPortInfo));
                    ptr->setPortProtocolValue(get<1>(*sensorPortInfo));

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
                    std::vector<std::string> assocPaths;
                    auto assocEntityId = sensor->getAssociationEntityId();
                    auto memoryName = cpuNameToMemoryName(assocEntityId);
                    std::vector<std::string> memoryInventories;
                    auto getSubTreeResponse = co_await utils::coGetSubTree(
                        "/xyz/openbmc_project/inventory", 0,
                        {"xyz.openbmc_project.Inventory.Item.Dimm"});
                    for (const auto& [objPath, mapperServiceMap] :
                         getSubTreeResponse)
                    {
                        if (objPath.find("ProcessorModule") !=
                            std::string::npos)
                        {
                            memoryInventories.emplace_back(objPath);
                        }
                    }
                    for (const auto& inventory : memoryInventories)
                    {
                        sdbusplus::message::object_path path(inventory);
                        if (memoryName == path.filename())
                        {
                            assocPaths.emplace_back(inventory);
                        }
                    }
                    std::vector<dbus::PathAssociation> assocs;
                    for (const auto& path : assocPaths)
                    {
                        dbus::PathAssociation assoc = {"memory", "all_states",
                                                       path};
                        assocs.emplace_back(assoc);
                    }
                    stateSet->setAssociation(assocs);
                }
            }
        }
    }
    co_return PLDM_SUCCESS;
}

} // namespace nvidia
} // namespace platform_mc
} // namespace pldm
