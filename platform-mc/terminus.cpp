#include "terminus.hpp"

#include "terminus_manager.hpp"

namespace pldm
{
namespace platform_mc
{

Terminus::Terminus(tid_t tid, uint64_t supportedTypes) :
    tid(tid), supportedTypes(supportedTypes)
{
    inventoryPath = "/xyz/openbmc_project/inventory/system/chassis/PLDM_Device_" +
                    std::to_string(tid);
    inventoryItemChassisInft = std::make_unique<InventoryItemChassisIntf>(
        utils::DBusHandler::getBus(), inventoryPath.c_str());
    maxBufferSize = 256;
}

bool Terminus::doesSupport(uint8_t type)
{
    return supportedTypes.test(type);
}

bool Terminus::parsePDRs()
{
    bool rc = true;
    for (auto& pdr : pdrs)
    {
        auto pdrHdr = reinterpret_cast<pldm_pdr_hdr*>(pdr.data());
        if (pdrHdr->type == PLDM_SENSOR_AUXILIARY_NAMES_PDR)
        {
            auto sensorAuxiliaryNames = parseSensorAuxiliaryNamesPDR(pdr);
            sensorAuxiliaryNamesTbl.emplace_back(
                std::move(sensorAuxiliaryNames));
        }
        else if (pdrHdr->type == PLDM_NUMERIC_SENSOR_PDR)
        {
            auto parsedPdr = parseNumericSensorPDR(pdr);
            numericSensorPdrs.emplace_back(std::move(parsedPdr));
        }
        else if (pdrHdr->type == PLDM_STATE_SENSOR_PDR)
        {
            auto parsedPdr = parseStateSensorPDR(pdr);
            stateSensorPdrs.emplace_back(std::move(parsedPdr));
        }
        else
        {
            rc = false;
        }
    }

    for (auto pdr : numericSensorPdrs)
    {
        addNumericSensor(pdr);
    }

    for (auto pdr : stateSensorPdrs)
    {
        auto [sensorID, stateSetSensorInfo] = pdr;
        addStateSensor(sensorID, std::move(stateSetSensorInfo));
    }

    return rc;
}

std::shared_ptr<SensorAuxiliaryNames>
    Terminus::getSensorAuxiliaryNames(SensorId id)
{
    for (auto sensorAuxiliaryNames : sensorAuxiliaryNamesTbl)
    {
        const auto& [sensorId, sensorCnt, sensorNames] = *sensorAuxiliaryNames;
        if (sensorId == id)
        {
            return sensorAuxiliaryNames;
        }
    }
    return nullptr;
}

std::shared_ptr<SensorAuxiliaryNames>
    Terminus::parseSensorAuxiliaryNamesPDR(const std::vector<uint8_t>& pdrData)
{
    constexpr uint8_t NullTerminator = 0;
    auto pdr = reinterpret_cast<const struct pldm_sensor_auxiliary_names_pdr*>(
        pdrData.data());
    const uint8_t* ptr = pdr->name_strings;
    std::vector<std::pair<NameLanguageTag, SensorName>> nameStrings{};
    for (int i = 0; i < pdr->name_string_count; i++)
    {
        std::string nameLanguageTag(reinterpret_cast<const char*>(ptr), 0,
                                    PLDM_STR_UTF_8_MAX_LEN);
        ptr += nameLanguageTag.size() + sizeof(NullTerminator);
        std::u16string u16NameString(reinterpret_cast<const char16_t*>(ptr), 0,
                                     PLDM_STR_UTF_16_MAX_LEN);
        ptr +=
            (u16NameString.size() + sizeof(NullTerminator)) * sizeof(uint16_t);
        std::transform(u16NameString.cbegin(), u16NameString.cend(),
                       u16NameString.begin(),
                       [](uint16_t utf16) { return be16toh(utf16); });
        std::string nameString =
            std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}
                .to_bytes(u16NameString);
        nameStrings.emplace_back(std::make_pair(nameLanguageTag, nameString));
    }

    return std::make_shared<SensorAuxiliaryNames>(
        pdr->sensor_id, pdr->sensor_count, nameStrings);
}

std::shared_ptr<pldm_numeric_sensor_value_pdr>
    Terminus::parseNumericSensorPDR(const std::vector<uint8_t>& pdr)
{
    const uint8_t* ptr = pdr.data();
    auto parsedPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    size_t count = (uint8_t*)(&parsedPdr->hysteresis.value_u8) -
                   (uint8_t*)(&parsedPdr->hdr);
    memcpy(&parsedPdr->hdr, ptr, count);
    ptr += count;

    switch (parsedPdr->sensor_data_size)
    {
        case PLDM_SENSOR_DATA_SIZE_UINT8:
            parsedPdr->hysteresis.value_u8 = static_cast<uint8_t>(*ptr);
            ptr += sizeof(parsedPdr->hysteresis.value_u8);
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT8:
            parsedPdr->hysteresis.value_s8 = static_cast<int8_t>(*ptr);
            ptr += sizeof(parsedPdr->hysteresis.value_s8);
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT16:
            parsedPdr->hysteresis.value_u16 =
                le16toh(static_cast<uint16_t>(*ptr));
            ptr += sizeof(parsedPdr->hysteresis.value_u16);
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT16:
            parsedPdr->hysteresis.value_s16 =
                le16toh(static_cast<int16_t>(*ptr));
            ptr += sizeof(parsedPdr->hysteresis.value_s16);
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT32:
            parsedPdr->hysteresis.value_u32 =
                le32toh(static_cast<uint32_t>(*ptr));
            ptr += sizeof(parsedPdr->hysteresis.value_u32);
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT32:
            parsedPdr->hysteresis.value_s32 =
                le32toh(static_cast<int32_t>(*ptr));
            ptr += sizeof(parsedPdr->hysteresis.value_s32);
            break;
        default:
            break;
    }

    count = (uint8_t*)&parsedPdr->max_readable.value_u8 -
            (uint8_t*)&parsedPdr->supported_thresholds;
    memcpy(&parsedPdr->supported_thresholds, ptr, count);
    ptr += count;

    switch (parsedPdr->sensor_data_size)
    {
        case PLDM_SENSOR_DATA_SIZE_UINT8:
            parsedPdr->max_readable.value_u8 = static_cast<uint8_t>(*ptr);
            ptr += sizeof(parsedPdr->max_readable.value_u8);
            parsedPdr->min_readable.value_u8 = static_cast<uint8_t>(*ptr);
            ptr += sizeof(parsedPdr->min_readable.value_u8);
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT8:
            parsedPdr->max_readable.value_s8 = static_cast<int8_t>(*ptr);
            ptr += sizeof(parsedPdr->max_readable.value_s8);
            parsedPdr->min_readable.value_s8 = static_cast<int8_t>(*ptr);
            ptr += sizeof(parsedPdr->min_readable.value_s8);
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT16:
            parsedPdr->max_readable.value_u16 =
                le16toh(static_cast<uint16_t>(*ptr));
            ptr += sizeof(parsedPdr->max_readable.value_u16);
            parsedPdr->min_readable.value_u16 =
                le16toh(static_cast<uint16_t>(*ptr));
            ptr += sizeof(parsedPdr->min_readable.value_u16);
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT16:
            parsedPdr->max_readable.value_s16 =
                le16toh(static_cast<int16_t>(*ptr));
            ptr += sizeof(parsedPdr->max_readable.value_s16);
            parsedPdr->min_readable.value_s16 =
                le16toh(static_cast<int16_t>(*ptr));
            ptr += sizeof(parsedPdr->min_readable.value_s16);
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT32:
            parsedPdr->max_readable.value_u32 =
                le32toh(static_cast<uint32_t>(*ptr));
            ptr += sizeof(parsedPdr->max_readable.value_u32);
            parsedPdr->min_readable.value_u32 =
                le32toh(static_cast<uint32_t>(*ptr));
            ptr += sizeof(parsedPdr->min_readable.value_u32);
            break;
        case PLDM_SENSOR_DATA_SIZE_SINT32:
            parsedPdr->max_readable.value_s32 =
                le32toh(static_cast<int32_t>(*ptr));
            ptr += sizeof(parsedPdr->max_readable.value_s32);
            parsedPdr->min_readable.value_s32 =
                le32toh(static_cast<int32_t>(*ptr));
            ptr += sizeof(parsedPdr->min_readable.value_s32);
            break;
        default:
            break;
    }

    count = (uint8_t*)&parsedPdr->nominal_value.value_u8 -
            (uint8_t*)&parsedPdr->range_field_format;
    memcpy(&parsedPdr->supported_thresholds, ptr, count);
    ptr += count;

    switch (parsedPdr->range_field_format)
    {
        case PLDM_RANGE_FIELD_FORMAT_UINT8:
            parsedPdr->nominal_value.value_u8 = static_cast<uint8_t>(*ptr);
            ptr += sizeof(parsedPdr->nominal_value.value_u8);
            parsedPdr->normal_max.value_u8 = static_cast<uint8_t>(*ptr);
            ptr += sizeof(parsedPdr->normal_max.value_u8);
            parsedPdr->normal_min.value_u8 = static_cast<uint8_t>(*ptr);
            ptr += sizeof(parsedPdr->normal_min.value_u8);
            parsedPdr->warning_high.value_u8 = static_cast<uint8_t>(*ptr);
            ptr += sizeof(parsedPdr->warning_high.value_u8);
            parsedPdr->warning_low.value_u8 = static_cast<uint8_t>(*ptr);
            ptr += sizeof(parsedPdr->warning_low.value_u8);
            parsedPdr->critical_high.value_u8 = static_cast<uint8_t>(*ptr);
            ptr += sizeof(parsedPdr->critical_high.value_u8);
            parsedPdr->critical_low.value_u8 = static_cast<uint8_t>(*ptr);
            ptr += sizeof(parsedPdr->critical_low.value_u8);
            parsedPdr->fatal_high.value_u8 = static_cast<uint8_t>(*ptr);
            ptr += sizeof(parsedPdr->fatal_high.value_u8);
            parsedPdr->fatal_low.value_u8 = static_cast<uint8_t>(*ptr);
            ptr += sizeof(parsedPdr->fatal_low.value_u8);
            break;
        case PLDM_RANGE_FIELD_FORMAT_SINT8:
            parsedPdr->nominal_value.value_s8 = static_cast<int8_t>(*ptr);
            ptr += sizeof(parsedPdr->nominal_value.value_s8);
            parsedPdr->normal_max.value_s8 = static_cast<int8_t>(*ptr);
            ptr += sizeof(parsedPdr->normal_max.value_s8);
            parsedPdr->normal_min.value_s8 = static_cast<int8_t>(*ptr);
            ptr += sizeof(parsedPdr->normal_min.value_s8);
            parsedPdr->warning_high.value_s8 = static_cast<int8_t>(*ptr);
            ptr += sizeof(parsedPdr->warning_high.value_s8);
            parsedPdr->warning_low.value_s8 = static_cast<int8_t>(*ptr);
            ptr += sizeof(parsedPdr->warning_low.value_s8);
            parsedPdr->critical_high.value_s8 = static_cast<int8_t>(*ptr);
            ptr += sizeof(parsedPdr->critical_high.value_s8);
            parsedPdr->critical_low.value_s8 = static_cast<int8_t>(*ptr);
            ptr += sizeof(parsedPdr->critical_low.value_s8);
            parsedPdr->fatal_high.value_s8 = static_cast<int8_t>(*ptr);
            ptr += sizeof(parsedPdr->fatal_high.value_s8);
            parsedPdr->fatal_low.value_s8 = static_cast<int8_t>(*ptr);
            ptr += sizeof(parsedPdr->fatal_low.value_s8);
            break;
        case PLDM_RANGE_FIELD_FORMAT_UINT16:
            parsedPdr->nominal_value.value_u16 =
                le16toh(static_cast<uint16_t>(*ptr));
            ptr += sizeof(parsedPdr->nominal_value.value_u16);
            parsedPdr->normal_max.value_u16 =
                le16toh(static_cast<uint16_t>(*ptr));
            ptr += sizeof(parsedPdr->normal_max.value_u16);
            parsedPdr->normal_min.value_u16 =
                le16toh(static_cast<uint16_t>(*ptr));
            ptr += sizeof(parsedPdr->normal_min.value_u16);
            parsedPdr->warning_high.value_u16 =
                le16toh(static_cast<uint16_t>(*ptr));
            ptr += sizeof(parsedPdr->warning_high.value_u16);
            parsedPdr->warning_low.value_u16 =
                le16toh(static_cast<uint16_t>(*ptr));
            ptr += sizeof(parsedPdr->warning_low.value_u16);
            parsedPdr->critical_high.value_u16 =
                le16toh(static_cast<uint16_t>(*ptr));
            ptr += sizeof(parsedPdr->critical_high.value_u16);
            parsedPdr->critical_low.value_u16 =
                le16toh(static_cast<uint16_t>(*ptr));
            ptr += sizeof(parsedPdr->critical_low.value_u16);
            parsedPdr->fatal_high.value_u16 =
                le16toh(static_cast<uint16_t>(*ptr));
            ptr += sizeof(parsedPdr->fatal_high.value_u16);
            parsedPdr->fatal_low.value_u16 =
                le16toh(static_cast<uint16_t>(*ptr));
            ptr += sizeof(parsedPdr->fatal_low.value_u16);
            break;
        case PLDM_RANGE_FIELD_FORMAT_SINT16:
            parsedPdr->nominal_value.value_s16 =
                le16toh(static_cast<int16_t>(*ptr));
            ptr += sizeof(parsedPdr->nominal_value.value_s16);
            parsedPdr->normal_max.value_s16 =
                le16toh(static_cast<int16_t>(*ptr));
            ptr += sizeof(parsedPdr->normal_max.value_s16);
            parsedPdr->normal_min.value_s16 =
                le16toh(static_cast<int16_t>(*ptr));
            ptr += sizeof(parsedPdr->normal_min.value_s16);
            parsedPdr->warning_high.value_s16 =
                le16toh(static_cast<int16_t>(*ptr));
            ptr += sizeof(parsedPdr->warning_high.value_s16);
            parsedPdr->warning_low.value_s16 =
                le16toh(static_cast<int16_t>(*ptr));
            ptr += sizeof(parsedPdr->warning_low.value_s16);
            parsedPdr->critical_high.value_s16 =
                le16toh(static_cast<int16_t>(*ptr));
            ptr += sizeof(parsedPdr->critical_high.value_s16);
            parsedPdr->critical_low.value_s16 =
                le16toh(static_cast<int16_t>(*ptr));
            ptr += sizeof(parsedPdr->critical_low.value_s16);
            parsedPdr->fatal_high.value_s16 =
                le16toh(static_cast<int16_t>(*ptr));
            ptr += sizeof(parsedPdr->fatal_high.value_s16);
            parsedPdr->fatal_low.value_s16 =
                le16toh(static_cast<int16_t>(*ptr));
            ptr += sizeof(parsedPdr->fatal_low.value_s16);
            break;
        case PLDM_RANGE_FIELD_FORMAT_UINT32:
            parsedPdr->nominal_value.value_u32 =
                le32toh(static_cast<uint32_t>(*ptr));
            ptr += sizeof(parsedPdr->nominal_value.value_u32);
            parsedPdr->normal_max.value_u32 =
                le32toh(static_cast<uint32_t>(*ptr));
            ptr += sizeof(parsedPdr->normal_max.value_u32);
            parsedPdr->normal_min.value_u32 =
                le32toh(static_cast<uint32_t>(*ptr));
            ptr += sizeof(parsedPdr->normal_min.value_u32);
            parsedPdr->warning_high.value_u32 =
                le32toh(static_cast<uint32_t>(*ptr));
            ptr += sizeof(parsedPdr->warning_high.value_u32);
            parsedPdr->warning_low.value_u32 =
                le32toh(static_cast<uint32_t>(*ptr));
            ptr += sizeof(parsedPdr->warning_low.value_u32);
            parsedPdr->critical_high.value_u32 =
                le32toh(static_cast<uint32_t>(*ptr));
            ptr += sizeof(parsedPdr->critical_high.value_u32);
            parsedPdr->critical_low.value_u32 =
                le32toh(static_cast<uint32_t>(*ptr));
            ptr += sizeof(parsedPdr->critical_low.value_u32);
            parsedPdr->fatal_high.value_u32 =
                le32toh(static_cast<uint32_t>(*ptr));
            ptr += sizeof(parsedPdr->fatal_high.value_u32);
            parsedPdr->fatal_low.value_u32 =
                le32toh(static_cast<uint32_t>(*ptr));
            ptr += sizeof(parsedPdr->fatal_low.value_u32);
            break;
        case PLDM_RANGE_FIELD_FORMAT_SINT32:
            parsedPdr->nominal_value.value_s32 =
                le32toh(static_cast<int32_t>(*ptr));
            ptr += sizeof(parsedPdr->nominal_value.value_s32);
            parsedPdr->normal_max.value_s32 =
                le32toh(static_cast<int32_t>(*ptr));
            ptr += sizeof(parsedPdr->normal_max.value_s32);
            parsedPdr->normal_min.value_s32 =
                le32toh(static_cast<int32_t>(*ptr));
            ptr += sizeof(parsedPdr->normal_min.value_s32);
            parsedPdr->warning_high.value_s32 =
                le32toh(static_cast<int32_t>(*ptr));
            ptr += sizeof(parsedPdr->warning_high.value_s32);
            parsedPdr->warning_low.value_s32 =
                le32toh(static_cast<int32_t>(*ptr));
            ptr += sizeof(parsedPdr->warning_low.value_s32);
            parsedPdr->critical_high.value_s32 =
                le32toh(static_cast<int32_t>(*ptr));
            ptr += sizeof(parsedPdr->critical_high.value_s32);
            parsedPdr->critical_low.value_s32 =
                le32toh(static_cast<int32_t>(*ptr));
            ptr += sizeof(parsedPdr->critical_low.value_s32);
            parsedPdr->fatal_high.value_s32 =
                le32toh(static_cast<int32_t>(*ptr));
            ptr += sizeof(parsedPdr->fatal_high.value_s32);
            parsedPdr->fatal_low.value_s32 =
                le32toh(static_cast<int32_t>(*ptr));
            ptr += sizeof(parsedPdr->fatal_low.value_s32);
            break;
        case PLDM_RANGE_FIELD_FORMAT_REAL32:
            parsedPdr->nominal_value.value_f32 =
                le32toh(static_cast<float>(*ptr));
            ptr += sizeof(parsedPdr->nominal_value.value_f32);
            parsedPdr->normal_max.value_f32 = le32toh(static_cast<float>(*ptr));
            ptr += sizeof(parsedPdr->normal_max.value_f32);
            parsedPdr->normal_min.value_f32 = le32toh(static_cast<float>(*ptr));
            ptr += sizeof(parsedPdr->normal_min.value_f32);
            parsedPdr->warning_high.value_f32 =
                le32toh(static_cast<float>(*ptr));
            ptr += sizeof(parsedPdr->warning_high.value_f32);
            parsedPdr->warning_low.value_f32 =
                le32toh(static_cast<float>(*ptr));
            ptr += sizeof(parsedPdr->warning_low.value_f32);
            parsedPdr->critical_high.value_f32 =
                le32toh(static_cast<float>(*ptr));
            ptr += sizeof(parsedPdr->critical_high.value_f32);
            parsedPdr->critical_low.value_f32 =
                le32toh(static_cast<float>(*ptr));
            ptr += sizeof(parsedPdr->critical_low.value_f32);
            parsedPdr->fatal_high.value_f32 = le32toh(static_cast<float>(*ptr));
            ptr += sizeof(parsedPdr->fatal_high.value_f32);
            parsedPdr->fatal_low.value_f32 = le32toh(static_cast<float>(*ptr));
            ptr += sizeof(parsedPdr->fatal_low.value_f32);
            break;
        default:
            break;
    }
    return parsedPdr;
}

std::tuple<SensorID, StateSetSensorInfo>
    Terminus::parseStateSensorPDR(std::vector<uint8_t>& stateSensorPdr)
{
    auto pdr =
        reinterpret_cast<const pldm_state_sensor_pdr*>(stateSensorPdr.data());
    std::vector<StateSetSensor> sensors{};
    auto statesPtr = pdr->possible_states;
    auto compositeSensorCount = pdr->composite_sensor_count;

    while (compositeSensorCount--)
    {
        auto state =
            reinterpret_cast<const state_sensor_possible_states*>(statesPtr);
        auto stateSedId = state->state_set_id;
        PossibleStates possibleStates{};
        uint8_t possibleStatesPos{};
        auto updateStates = [&possibleStates,
                             &possibleStatesPos](const bitfield8_t& val) {
            for (int i = 0; i < CHAR_BIT; i++)
            {
                if (val.byte & (1 << i))
                {
                    possibleStates.insert(possibleStatesPos * CHAR_BIT + i);
                }
            }
            possibleStatesPos++;
        };
        std::for_each(&state->states[0],
                      &state->states[state->possible_states_size],
                      updateStates);
        sensors.emplace_back(std::make_tuple(stateSedId, std::move(possibleStates)));
        if (compositeSensorCount)
        {
            statesPtr += sizeof(state_sensor_possible_states) +
                         state->possible_states_size - 1;
        }
    }

    auto entityInfo =
        std::make_tuple(static_cast<ContainerID>(pdr->container_id),
                        static_cast<EntityType>(pdr->entity_type),
                        static_cast<EntityInstance>(pdr->entity_instance));
    auto sensorInfo =
        std::make_tuple(std::move(entityInfo), std::move(sensors));
    return std::make_tuple(pdr->sensor_id, std::move(sensorInfo));
}

void Terminus::addNumericSensor(
    const std::shared_ptr<pldm_numeric_sensor_value_pdr> pdr)
{
    std::string sensorName = "PLDM_Device_" + std::to_string(pdr->sensor_id) +
                             "_" + std::to_string(tid);

    auto sensorAuxiliaryNames = getSensorAuxiliaryNames(pdr->sensor_id);
    if (sensorAuxiliaryNames)
    {
        const auto& [sensorId, sensorCnt, sensorNames] = *sensorAuxiliaryNames;
        if (sensorCnt == 1)
        {
            sensorName = sensorNames[0].second + "_" +
                         std::to_string(pdr->sensor_id) + "_" +
                         std::to_string(tid);
        }
    }

    try
    {
        auto sensor = std::make_shared<NumericSensor>(
            tid, true, pdr, sensorName, inventoryPath);
        numericSensors.emplace_back(sensor);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Failed to create NumericSensor. ERROR=" << e.what()
                  << "sensorName=" << sensorName << "\n";
    }
}

void Terminus::addStateSensor(SensorID sId, StateSetSensorInfo sensorInfo)
{
    std::string sensorName = "PLDM_Device_" + std::to_string(sId) +
                             "_" + std::to_string(tid);

    auto sensorAuxiliaryNames = getSensorAuxiliaryNames(sId);
    if (sensorAuxiliaryNames)
    {
        const auto& [sensorId, sensorCnt, sensorNames] = *sensorAuxiliaryNames;
        if (sensorCnt == 1)
        {
            sensorName = sensorNames[0].second + "_" +
                         std::to_string(sId) + "_" +
                         std::to_string(tid);
        }
    }

    try
    {
        auto sensor = std::make_shared<StateSensor>(
            tid, true, sId, std::move(sensorInfo), sensorName, inventoryPath);
        stateSensors.emplace_back(sensor);
    }
    catch (const std::exception& e)
    {
        std::cerr << " Failed to create StateSensor. ERROR =" << e.what()
                  << " sensorName=" << sensorName << std::endl;
    }
}


} // namespace platform_mc
} // namespace pldm
