#include "oem_nvidia.hpp"

#include "memoryPageRetirementCount.hpp"
#include "oem/nvidia/platform-mc/remoteDebug.hpp"
#include "platform-mc/state_sensor.hpp"
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
             static_cast<uint8_t>(OemPowerCapPersistence::OEM_POWERCAP_TDP_NONVOLATILE))
            ||
            (pdr->oem_effecter_powercap ==
             static_cast<uint8_t>(OemPowerCapPersistence::OEM_POWERCAP_EDPP_NONVOLATILE)))
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
    auto parsedPdr = std::make_shared<pldm_oem_energycount_numeric_sensor_value_pdr>();

    size_t expectedPDRSize = PLDM_PDR_OEM_ENERGYCOUNT_NUMERIC_SENSOR_PDR_MIN_LENGTH;
    if (vendorData.size() < expectedPDRSize)
    {
        lg2::error("parseOEMEnergyCountNumericSensorPDR() Corrupted PDR, size={PDRSIZE}",
                   "PDRSIZE", vendorData.size());
        return nullptr;
    }

    size_t count = (uint8_t*)(&parsedPdr->max_readable.value_u8) -
            (uint8_t*)(&parsedPdr->terminus_handle);
    memcpy(&parsedPdr->terminus_handle, ptr, count);
    ptr += count;

    expectedPDRSize -= PLDM_PDR_OEM_ENERGYCOUNT_NUMERIC_SENSOR_PDR_VARIED_MIN_LENGTH;
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
        lg2::error("parseOEMEnergyCountNumericSensorPDR() Corrupted PDR, size={PDRSIZE}",
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

} // namespace nvidia
} // namespace platform_mc
} // namespace pldm
