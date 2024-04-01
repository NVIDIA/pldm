#include <filesystem>

#ifdef OEM_NVIDIA
#include "oem/nvidia/platform-mc/oem_nvidia.hpp"
#endif

#include "terminus_manager.hpp"

namespace pldm
{
namespace platform_mc
{

Terminus::Terminus(tid_t tid, uint64_t supportedTypes, UUID& uuid,
                   TerminusManager& terminusManager) :
    initalized(false),
    pollEvent(false), synchronyConfigurationSupported(0), tid(tid),
    supportedTypes(supportedTypes), uuid(uuid), terminusManager(terminusManager)

{
    maxBufferSize = 256;
    scanInventories();
}

void Terminus::interfaceAdded(sdbusplus::message::message& m)
{
    if (!initalized)
    {
        return;
    }

    sdbusplus::message::object_path objPath;
    pldm::dbus::InterfaceMap interfaces;
    m.read(objPath, interfaces);

    // if any interested interface added, refresh the associations
    bool needRefresh = false;
    for (const auto& [intf, properties] : interfaces)
    {
        for (const auto& [entitytype, entityIface] : entityInterfaces)
        {
            if (intf == entityIface)
            {
                needRefresh = true;
                break;
            }
        }
        if (intf == overallSystemInterface)
        {
            needRefresh = true;
        }

        if (needRefresh)
        {
            break;
        }
    }

    if (needRefresh)
    {
        auto ret = scanInventories();
        if (ret)
        {
            updateAssociations();
        }
    }
}

bool Terminus::checkI2CDeviceInventory(uint8_t bus, uint8_t addr)
{
    auto mctpInfo = terminusManager.toMctpInfo(tid);
    if (!mctpInfo)
    {
        return false;
    }

    auto eid = std::get<0>(mctpInfo.value());
    auto mctpEndpoints = utils::DBusHandler().getSubtree(
        "/xyz/openbmc_project/mctp", 0, {"xyz.openbmc_project.MCTP.Endpoint"});

    for (const auto& [endPointPath, mapperServiceMap] : mctpEndpoints)
    {
        std::filesystem::path filePath(endPointPath);
        if (eid != std::stoi(filePath.filename()))
        {
            continue;
        }

        auto binding = pldm::utils::DBusHandler().getDbusProperty<std::string>(
            endPointPath.c_str(), "BindingType",
            "xyz.openbmc_project.MCTP.Binding");
        if (binding != "xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus")
        {
            continue;
        }

        auto mctpI2cBus = pldm::utils::DBusHandler().getDbusProperty<size_t>(
            endPointPath.c_str(), "Bus",
            "xyz.openbmc_project.Inventory.Decorator.I2CDevice");
        if (mctpI2cBus != bus)
        {
            continue;
        }

        auto mctpI2cAddr = pldm::utils::DBusHandler().getDbusProperty<size_t>(
            endPointPath.c_str(), "Address",
            "xyz.openbmc_project.Inventory.Decorator.I2CDevice");

        if (mctpI2cAddr != addr)
        {
            continue;
        }

        return true;
    }

    return false;
}

bool Terminus::checkNsmDeviceInventory(UUID nsmUuid)
{
    if (nsmUuid.substr(0, 36) == uuid.substr(0, 36)) {
        return true;
    }
    else {
        return false;
    }
}

bool Terminus::checkDeviceInventory(const std::string& objPath)
{
    try
    {
        auto getSubTreeResponse = utils::DBusHandler().getSubtree(
            objPath, 0,
            {"xyz.openbmc_project.Configuration.I2CDeviceAssociation",
             "xyz.openbmc_project.Configuration.NsmDeviceAssociation"});

        if (getSubTreeResponse.size() == 0)
        {
            return true;
        }

        bool found = false;

        for (const auto& [objectPath, serviceMap] : getSubTreeResponse)
        {
            for (const auto& [serviceName, interfaces] : serviceMap)
            {
                for (const auto& interface : interfaces)
                {
                    if (interface ==
                        "xyz.openbmc_project.Configuration.I2CDeviceAssociation")
                    {
                        uint64_t bus = 0;
                        uint64_t addr = 0;
                        bus = utils::DBusHandler().getDbusProperty<uint64_t>(
                            objectPath.c_str(), "Bus",
                            "xyz.openbmc_project.Configuration.I2CDeviceAssociation");
                        addr = utils::DBusHandler().getDbusProperty<uint64_t>(
                            objectPath.c_str(), "Address",
                            "xyz.openbmc_project.Configuration.I2CDeviceAssociation");
                        found = checkI2CDeviceInventory(bus, addr);
                    }
                    else if (
                        interface ==
                        "xyz.openbmc_project.Configuration.NsmDeviceAssociation")
                    {
                        auto nsmUuid = utils::DBusHandler().getDbusProperty<std::string>(
                            objectPath.c_str(), "UUID",
                            "xyz.openbmc_project.Configuration.NsmDeviceAssociation");
                        found = checkNsmDeviceInventory(nsmUuid);
                    }

                    if (found)
                    {
                        getSensorAuxNameFromEM(objPath);
                        return true;
                    }
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "no interested Configuration PDIs found, Error: {ERROR} path:{PATH}",
            "ERROR", e, "PATH", objPath);
    }

    return false;
}

void Terminus::getSensorAuxNameFromEM(const std::string& objPath)
{
    try
    {
        sensorAuxNameOverwriteTbl.clear();

        auto getSubTreeResponse = utils::DBusHandler().getSubtree(
            objPath, 0, {"xyz.openbmc_project.Configuration.SensorAuxName"});

        if (getSubTreeResponse.size() == 0)
        {
            return;
        }

        for (auto& [path, mapperServiceMap] : getSubTreeResponse)
        {
            auto sensorId = utils::DBusHandler().getDbusProperty<uint64_t>(
                path.c_str(), "SensorId",
                "xyz.openbmc_project.Configuration.SensorAuxName");

            auto auxNames =
                utils::DBusHandler().getDbusProperty<std::vector<std::string>>(
                    path.c_str(), "AuxNames",
                    "xyz.openbmc_project.Configuration.SensorAuxName");
            for (auto auxName : auxNames)
            {
                sensorAuxNameOverwriteTbl[sensorId] = {{{"en", auxName}}};
            }
        }
    }
    catch (const std::exception& e)
    {
        lg2::info("no Configuration.SensorAuxName Error: {ERROR} path:{PATH}",
                  "ERROR", e, "PATH", objPath);
    }
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
        else if (pdrHdr->type == PLDM_EFFECTER_AUXILIARY_NAMES_PDR)
        {
            auto effecterAuxiliaryNames = parseEffecterAuxiliaryNamesPDR(pdr);
            effecterAuxiliaryNamesTbl.emplace_back(
                std::move(effecterAuxiliaryNames));
        }
        else if (pdrHdr->type == PLDM_NUMERIC_SENSOR_PDR)
        {
            auto parsedPdr = parseNumericSensorPDR(pdr);
            if (parsedPdr != nullptr)
            {
                numericSensorPdrs.emplace_back(std::move(parsedPdr));
            }
        }
        else if (pdrHdr->type == PLDM_NUMERIC_EFFECTER_PDR)
        {
            auto parsedPdr = parseNumericEffecterPDR(pdr);
            numericEffecterPdrs.emplace_back(std::move(parsedPdr));
        }
        else if (pdrHdr->type == PLDM_STATE_SENSOR_PDR)
        {
            auto parsedPdr = parseStateSensorPDR(pdr);
            stateSensorPdrs.emplace_back(std::move(parsedPdr));
        }
        else if (pdrHdr->type == PLDM_PDR_ENTITY_ASSOCIATION)
        {
            parseEntityAssociationPDR(pdr);
        }
        else if (pdrHdr->type == PLDM_STATE_EFFECTER_PDR)
        {
            auto parsedPdr = parseStateEffecterPDR(pdr);
            stateEffecterPdrs.emplace_back(std::move(parsedPdr));
        }
        else if (pdrHdr->type == PLDM_OEM_PDR)
        {
            auto parsedPdr = parseOemPDR(pdr);
#ifdef OEM_NVIDIA
            if (static_cast<nvidia::NvidiaOemPdrType>(
                    std::get<2>(parsedPdr)[2]) ==
                nvidia::NvidiaOemPdrType::
                    NVIDIA_OEM_PDR_TYPE_SENSOR_ENERGYCOUNT)
            {
                auto parsedOEMPdr = nvidia::parseOEMEnergyCountNumericSensorPDR(
                    std::get<2>(parsedPdr));
                oemEnergyCountNumericSensorPdrs.emplace_back(
                    std::move(parsedOEMPdr));
            }
            else
            {
#endif
                oemPdrs.emplace_back(std::move(parsedPdr));
#ifdef OEM_NVIDIA
            }
#endif
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

#ifdef OEM_NVIDIA
    for (auto pdr : oemEnergyCountNumericSensorPdrs)
    {
        addOEMEnergyCountNumericSensor(pdr);
    }
#endif

    for (auto pdr : numericEffecterPdrs)
    {
        addNumericEffecter(pdr);
    }

    for (auto pdr : stateSensorPdrs)
    {
        auto [sensorID, stateSetSensorInfo] = pdr;
        addStateSensor(sensorID, std::move(stateSetSensorInfo));
    }

    for (auto& pdr : stateEffecterPdrs)
    {
        auto [effecterId, stateSetEffecterInfo] = pdr;
        addStateEffecter(effecterId, std::move(stateSetEffecterInfo));
    }

#ifdef OEM_NVIDIA
    nvidia::nvidiaInitTerminus(*this);
#endif

    updateAssociations();

    interfaceAddedMatch = std::make_unique<sdbusplus::bus::match_t>(
        utils::DBusHandler().getBus(),
        sdbusplus::bus::match::rules::interfacesAdded(
            "/xyz/openbmc_project/inventory"),
        std::bind(std::mem_fn(&Terminus::interfaceAdded), this,
                  std::placeholders::_1));

    return rc;
}

std::shared_ptr<SensorAuxiliaryNames>
    Terminus::getSensorAuxiliaryNames(SensorID id)
{
    if (sensorAuxNameOverwriteTbl.find(id) != sensorAuxNameOverwriteTbl.end())
    {
        return std::make_shared<SensorAuxiliaryNames>(
            id, 1, sensorAuxNameOverwriteTbl[id]);
    }
    else
    {
        for (auto sensorAuxiliaryNames : sensorAuxiliaryNamesTbl)
        {
            const auto& [sensorId, sensorCnt, sensorNames] =
                *sensorAuxiliaryNames;
            if (sensorId == id)
            {
                return sensorAuxiliaryNames;
            }
        }
    }
    return nullptr;
}

std::shared_ptr<EffecterAuxiliaryNames>
    Terminus::getEffecterAuxiliaryNames(EffecterID id)
{
    for (auto effecterAuxiliaryNames : effecterAuxiliaryNamesTbl)
    {
        const auto& [effecterId, effecterCnt, effecterNames] =
            *effecterAuxiliaryNames;
        if (effecterId == id)
        {
            return effecterAuxiliaryNames;
        }
    }
    return nullptr;
}

std::shared_ptr<SensorAuxiliaryNames>
    Terminus::parseSensorAuxiliaryNamesPDR(const std::vector<uint8_t>& pdrData)
{
    constexpr uint8_t NullTerminator = 0;
    size_t parseLen = 0;
    auto pdr = reinterpret_cast<const struct pldm_sensor_auxiliary_names_pdr*>(
        pdrData.data());
    const uint8_t* ptr = pdr->names;
    parseLen += sizeof(pldm_sensor_auxiliary_names_pdr);
    std::vector<std::vector<std::pair<NameLanguageTag, SensorName>>>
        sensorAuxNames{};
    try
    {
        for (int i = 0; i < pdr->sensor_count; i++)
        {
            const uint8_t nameStringCount = static_cast<uint8_t>(*ptr);
            ptr += sizeof(uint8_t);
            parseLen += sizeof(uint8_t);
            std::vector<std::pair<NameLanguageTag, SensorName>> nameStrings{};
            for (int j = 0; j < nameStringCount; j++)
            {
                std::string nameLanguageTag(reinterpret_cast<const char*>(ptr),
                                            0, PLDM_STR_UTF_8_MAX_LEN);
                ptr += nameLanguageTag.size() + sizeof(NullTerminator);
                parseLen += nameLanguageTag.size() + sizeof(NullTerminator);
                std::vector<uint8_t> u16NameStringVec(pdrData.begin()+parseLen-1, pdrData.end());
                std::u16string u16NameString(
                    reinterpret_cast<const char16_t*>(u16NameStringVec.data()), 0,
                    PLDM_STR_UTF_16_MAX_LEN);
                ptr += (u16NameString.size() + sizeof(NullTerminator)) *
                       sizeof(uint16_t);
                parseLen += (u16NameString.size() + sizeof(NullTerminator)) *
                       sizeof(uint16_t);
                std::transform(u16NameString.cbegin(), u16NameString.cend(),
                               u16NameString.begin(),
                               [](uint16_t utf16) { return be16toh(utf16); });
                std::string nameString =
                    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,
                                         char16_t>{}
                        .to_bytes(u16NameString);
                nameStrings.emplace_back(
                    std::make_pair(nameLanguageTag, nameString));
            }
            sensorAuxNames.emplace_back(nameStrings);
        }
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "Failed to parse sensorAuxiliaryNamesPDR record, sensorId={SENSORID}, {ERROR}.",
            "SENSORID", pdr->sensor_id, "ERROR", e);
    }

    return std::make_shared<SensorAuxiliaryNames>(
        pdr->sensor_id, pdr->sensor_count, sensorAuxNames);
}

std::shared_ptr<EffecterAuxiliaryNames>
    Terminus::parseEffecterAuxiliaryNamesPDR(
        const std::vector<uint8_t>& pdrData)
{
    constexpr uint8_t NullTerminator = 0;
    auto pdr =
        reinterpret_cast<const struct pldm_effecter_auxiliary_names_pdr*>(
            pdrData.data());
    const uint8_t* ptr = pdr->names;
    std::vector<std::vector<std::pair<NameLanguageTag, EffecterName>>>
        effecterAuxNames{};
    try
    {
        for (int i = 0; i < pdr->effecter_count; i++)
        {
            const uint8_t nameStringCount = static_cast<uint8_t>(*ptr);
            ptr += sizeof(uint8_t);
            std::vector<std::pair<NameLanguageTag, EffecterName>> nameStrings{};
            for (int j = 0; j < nameStringCount; j++)
            {
                std::string nameLanguageTag(reinterpret_cast<const char*>(ptr),
                                            0, PLDM_STR_UTF_8_MAX_LEN);
                ptr += nameLanguageTag.size() + sizeof(NullTerminator);
                std::u16string u16NameString(
                    reinterpret_cast<const char16_t*>(ptr), 0,
                    PLDM_STR_UTF_16_MAX_LEN);
                ptr += (u16NameString.size() + sizeof(NullTerminator)) *
                       sizeof(uint16_t);
                std::transform(u16NameString.cbegin(), u16NameString.cend(),
                               u16NameString.begin(),
                               [](uint16_t utf16) { return be16toh(utf16); });
                std::string nameString =
                    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,
                                         char16_t>{}
                        .to_bytes(u16NameString);
                nameStrings.emplace_back(
                    std::make_pair(nameLanguageTag, nameString));
            }
            effecterAuxNames.emplace_back(nameStrings);
        }
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "Failed to parse effecterAuxiliaryNamesPDR record, effecterId={EFFECTERID}, {ERROR}.",
            "EFFECTERID", pdr->effecter_id, "ERROR", e);
    }

    return std::make_shared<EffecterAuxiliaryNames>(
        pdr->effecter_id, pdr->effecter_count, effecterAuxNames);
}

void Terminus::parseEntityAssociationPDR(const std::vector<uint8_t>& pdrData)
{
    auto pdr = reinterpret_cast<const struct pldm_pdr_entity_association*>(
        pdrData.data() + sizeof(struct pldm_pdr_hdr));
    ContainerID containerId{pdr->container_id};
    EntityInfo container{pdr->container.entity_container_id,
                         pdr->container.entity_type,
                         pdr->container.entity_instance_num};
    if (entityAssociations.contains(containerId))
    {
        if (entityAssociations[containerId].first != container)
        {
            lg2::error(
                "ERROR: TID:{TID} ContainerId:{CONTAINER_ID} has different entity.",
                "TID", tid, "CONTAINER_ID", containerId);
            return;
        }
    }
    else
    {
        entityAssociations.emplace(
            containerId, std::make_pair(container, std::set<EntityInfo>{}));
    }

    auto& containedEntities{entityAssociations[containerId].second};
    for (int i = 0; i < pdr->num_children; ++i)
    {
        EntityInfo entityInfo{pdr->children[i].entity_container_id,
                              pdr->children[i].entity_type,
                              pdr->children[i].entity_instance_num};
        containedEntities.emplace(std::move(entityInfo));
    }
}

std::shared_ptr<pldm_numeric_sensor_value_pdr>
    Terminus::parseNumericSensorPDR(const std::vector<uint8_t>& pdr)
{
    const uint8_t* ptr = pdr.data();
    auto parsedPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    size_t count = (uint8_t*)(&parsedPdr->hysteresis.value_u8) -
                   (uint8_t*)(&parsedPdr->hdr);

    size_t expectedPDRSize = PLDM_PDR_NUMERIC_SENSOR_PDR_MIN_LENGTH;
    if (pdr.size() < expectedPDRSize)
    {
        lg2::error("parseNumericSensorPDR() Corrupted PDR, size={PDRSIZE}",
                   "PDRSIZE", pdr.size());
        return nullptr;
    }

    memcpy(&parsedPdr->hdr, ptr, count);
    ptr += count;

    expectedPDRSize -= PLDM_PDR_NUMERIC_SENSOR_PDR_VARIED_MIN_LENGTH;
    switch (parsedPdr->sensor_data_size)
    {
        case PLDM_SENSOR_DATA_SIZE_UINT8:
        case PLDM_SENSOR_DATA_SIZE_SINT8:
            expectedPDRSize += 3 * sizeof(uint8_t);
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT16:
        case PLDM_SENSOR_DATA_SIZE_SINT16:
            expectedPDRSize += 3 * sizeof(uint16_t);
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT32:
        case PLDM_SENSOR_DATA_SIZE_SINT32:
            expectedPDRSize += 3 * sizeof(uint32_t);
            break;
        default:
            break;
    }

    if (pdr.size() < expectedPDRSize)
    {
        lg2::error("parseNumericSensorPDR() Corrupted PDR, size={PDRSIZE}",
                   "PDRSIZE", pdr.size());
        return nullptr;
    }

    switch (parsedPdr->range_field_format)
    {
        case PLDM_RANGE_FIELD_FORMAT_UINT8:
        case PLDM_RANGE_FIELD_FORMAT_SINT8:
            expectedPDRSize += 9 * sizeof(uint8_t);
            break;
        case PLDM_RANGE_FIELD_FORMAT_UINT16:
        case PLDM_RANGE_FIELD_FORMAT_SINT16:
            expectedPDRSize += 9 * sizeof(uint16_t);
            break;
        case PLDM_RANGE_FIELD_FORMAT_UINT32:
        case PLDM_RANGE_FIELD_FORMAT_SINT32:
        case PLDM_RANGE_FIELD_FORMAT_REAL32:
            expectedPDRSize += 9 * sizeof(uint32_t);
            break;
        default:
            break;
    }

    if (pdr.size() < expectedPDRSize)
    {
        lg2::error("parseNumericSensorPDR() Corrupted PDR, size={PDRSIZE}",
                   "PDRSIZE", pdr.size());
        return nullptr;
    }
    
    switch (parsedPdr->sensor_data_size)
    {
        case PLDM_SENSOR_DATA_SIZE_UINT8:
        case PLDM_SENSOR_DATA_SIZE_SINT8:
            parsedPdr->hysteresis.value_u8 = *((uint8_t*)ptr);
            ptr += sizeof(parsedPdr->hysteresis.value_u8);
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT16:
        case PLDM_SENSOR_DATA_SIZE_SINT16:
        {
            //hysValue =  *((uint16_t*)ptr);
            uint16_t hysValue = 0;
            memcpy(&hysValue, ptr, sizeof(uint16_t));
            parsedPdr->hysteresis.value_u16 = le16toh(hysValue);
            ptr += sizeof(parsedPdr->hysteresis.value_u16);
            break;
        }
        case PLDM_SENSOR_DATA_SIZE_UINT32:
        case PLDM_SENSOR_DATA_SIZE_SINT32:
        {
            uint32_t hysValue = 0;
            memcpy(&hysValue, ptr, sizeof(uint32_t));
            parsedPdr->hysteresis.value_u32 = le32toh(hysValue);
            ptr += sizeof(parsedPdr->hysteresis.value_u32);
            break;
        }
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
        case PLDM_SENSOR_DATA_SIZE_SINT8:
            parsedPdr->max_readable.value_u8 = *((uint8_t*)ptr);
            ptr += sizeof(parsedPdr->max_readable.value_u8);
            parsedPdr->min_readable.value_u8 = *((uint8_t*)ptr);
            ptr += sizeof(parsedPdr->min_readable.value_u8);
            break;
        case PLDM_SENSOR_DATA_SIZE_UINT16:
        case PLDM_SENSOR_DATA_SIZE_SINT16:
        {
            uint16_t maxReadableValue = 0;
            memcpy(&maxReadableValue, ptr, sizeof(uint16_t));
            parsedPdr->max_readable.value_u16 = le16toh(maxReadableValue);
            ptr += sizeof(parsedPdr->max_readable.value_u16);
            uint16_t minReadableValue = 0;
            memcpy(&minReadableValue, ptr, sizeof(uint16_t));
            parsedPdr->min_readable.value_u16 = le16toh(minReadableValue);
            ptr += sizeof(parsedPdr->min_readable.value_u16);
            break;
        }
        case PLDM_SENSOR_DATA_SIZE_UINT32:
        case PLDM_SENSOR_DATA_SIZE_SINT32:
        {
            uint32_t maxReadableValue = 0;
            memcpy(&maxReadableValue, ptr, sizeof(uint32_t));
            parsedPdr->max_readable.value_u32 = le32toh(maxReadableValue);
            ptr += sizeof(parsedPdr->max_readable.value_u32);
            uint32_t minReadableValue = 0;
            memcpy(&minReadableValue, ptr, sizeof(uint32_t));
            parsedPdr->min_readable.value_u32 = le32toh(minReadableValue);
            ptr += sizeof(parsedPdr->min_readable.value_u32);
            break;
        }
        default:
            break;
    }

    count = (uint8_t*)&parsedPdr->nominal_value.value_u8 -
            (uint8_t*)&parsedPdr->range_field_format;
    memcpy(&parsedPdr->range_field_format, ptr, count);
    ptr += count;

    switch (parsedPdr->range_field_format)
    {
        case PLDM_RANGE_FIELD_FORMAT_UINT8:
        case PLDM_RANGE_FIELD_FORMAT_SINT8:
            parsedPdr->nominal_value.value_u8 = *((uint8_t*)ptr);
            ptr += sizeof(parsedPdr->nominal_value.value_u8);
            parsedPdr->normal_max.value_u8 = *((uint8_t*)ptr);
            ptr += sizeof(parsedPdr->normal_max.value_u8);
            parsedPdr->normal_min.value_u8 = *((uint8_t*)ptr);
            ptr += sizeof(parsedPdr->normal_min.value_u8);
            parsedPdr->warning_high.value_u8 = *((uint8_t*)ptr);
            ptr += sizeof(parsedPdr->warning_high.value_u8);
            parsedPdr->warning_low.value_u8 = *((uint8_t*)ptr);
            ptr += sizeof(parsedPdr->warning_low.value_u8);
            parsedPdr->critical_high.value_u8 = *((uint8_t*)ptr);
            ptr += sizeof(parsedPdr->critical_high.value_u8);
            parsedPdr->critical_low.value_u8 = *((uint8_t*)ptr);
            ptr += sizeof(parsedPdr->critical_low.value_u8);
            parsedPdr->fatal_high.value_u8 = *((uint8_t*)ptr);
            ptr += sizeof(parsedPdr->fatal_high.value_u8);
            parsedPdr->fatal_low.value_u8 = *((uint8_t*)ptr);
            ptr += sizeof(parsedPdr->fatal_low.value_u8);
            break;
        case PLDM_RANGE_FIELD_FORMAT_UINT16:
        case PLDM_RANGE_FIELD_FORMAT_SINT16:
        {
            uint16_t nominalValue = 0;
            memcpy(&nominalValue, ptr, sizeof(uint16_t));
            parsedPdr->nominal_value.value_u16 = le16toh(nominalValue);
            ptr += sizeof(parsedPdr->nominal_value.value_u16);
            uint16_t normalMaxValue = 0;
            memcpy(&normalMaxValue, ptr, sizeof(uint16_t));
            parsedPdr->normal_max.value_u16 = le16toh(normalMaxValue);
            ptr += sizeof(parsedPdr->normal_max.value_u16);
            uint16_t normalMinValue = 0;
            memcpy(&normalMinValue, ptr, sizeof(uint16_t));
            parsedPdr->normal_min.value_u16 = le16toh(normalMinValue);
            ptr += sizeof(parsedPdr->normal_min.value_u16);
            uint16_t warningHighValue = 0;
            memcpy(&warningHighValue, ptr, sizeof(uint16_t));
            parsedPdr->warning_high.value_u16 = le16toh(warningHighValue);
            ptr += sizeof(parsedPdr->warning_high.value_u16);
            uint16_t warninglowValue = 0;
            memcpy(&warninglowValue, ptr, sizeof(uint16_t));
            parsedPdr->warning_low.value_u16 = le16toh(warninglowValue);
            ptr += sizeof(parsedPdr->warning_low.value_u16);
            uint16_t criticalHighValue = 0;
            memcpy(&criticalHighValue, ptr, sizeof(uint16_t));
            parsedPdr->critical_high.value_u16 = le16toh(criticalHighValue);
            ptr += sizeof(parsedPdr->critical_high.value_u16);
            uint16_t criticalLowValue = 0;
            memcpy(&criticalLowValue, ptr, sizeof(uint16_t));
            parsedPdr->critical_low.value_u16 = le16toh(criticalLowValue);
            ptr += sizeof(parsedPdr->critical_low.value_u16);
            uint16_t fatalHighValue = 0;
            memcpy(&fatalHighValue, ptr, sizeof(uint16_t));
            parsedPdr->fatal_high.value_u16 = le16toh(fatalHighValue);
            ptr += sizeof(parsedPdr->fatal_high.value_u16);
            uint16_t fatalLowValue = 0;
            memcpy(&fatalLowValue, ptr, sizeof(uint16_t));
            parsedPdr->fatal_low.value_u16 = le16toh(fatalLowValue);
            ptr += sizeof(parsedPdr->fatal_low.value_u16);
            break;
        }
        case PLDM_RANGE_FIELD_FORMAT_UINT32:
        case PLDM_RANGE_FIELD_FORMAT_SINT32:
        case PLDM_RANGE_FIELD_FORMAT_REAL32:
        {
            uint32_t nominalValue = 0;
            memcpy(&nominalValue, ptr, sizeof(uint32_t));
            parsedPdr->nominal_value.value_u32 = le32toh(nominalValue);
            ptr += sizeof(parsedPdr->nominal_value.value_u32);
            uint32_t normalMaxValue = 0;
            memcpy(&normalMaxValue, ptr, sizeof(uint32_t));
            parsedPdr->normal_max.value_u32 = le32toh(normalMaxValue);
            ptr += sizeof(parsedPdr->normal_max.value_u32);
            uint32_t normalMinValue = 0;
            memcpy(&normalMinValue, ptr, sizeof(uint32_t));
            parsedPdr->normal_min.value_u32 = le32toh(normalMinValue);
            ptr += sizeof(parsedPdr->normal_min.value_u32);
            uint32_t warningHighValue = 0;
            memcpy(&warningHighValue, ptr, sizeof(uint32_t));
            parsedPdr->warning_high.value_u32 = le32toh(warningHighValue);
            ptr += sizeof(parsedPdr->warning_high.value_u32);
            uint32_t warningLowValue = 0;
            memcpy(&warningLowValue, ptr, sizeof(uint32_t));
            parsedPdr->warning_low.value_u32 = le32toh(warningLowValue);
            ptr += sizeof(parsedPdr->warning_low.value_u32);
            uint32_t criticalHighValue = 0;
            memcpy(&criticalHighValue, ptr, sizeof(uint32_t));
            parsedPdr->critical_high.value_u32 = le32toh(criticalHighValue);
            ptr += sizeof(parsedPdr->critical_high.value_u32);
            uint32_t criticalLowValue = 0;
            memcpy(&criticalLowValue, ptr, sizeof(uint32_t));
            parsedPdr->critical_low.value_u32 = le32toh(criticalLowValue);
            ptr += sizeof(parsedPdr->critical_low.value_u32);
            uint32_t fatalHighValue = 0;
            memcpy(&fatalHighValue, ptr, sizeof(uint32_t));
            parsedPdr->fatal_high.value_u32 = le32toh(fatalHighValue);
            ptr += sizeof(parsedPdr->fatal_high.value_u32);
            uint32_t fatalLowValue = 0;
            memcpy(&fatalLowValue, ptr, sizeof(uint32_t));
            parsedPdr->fatal_low.value_u32 = le32toh(fatalLowValue);
            ptr += sizeof(parsedPdr->fatal_low.value_u32);
            break;
        }
        default:
            break;
    }
    return parsedPdr;
}

std::shared_ptr<pldm_numeric_effecter_value_pdr>
    Terminus::parseNumericEffecterPDR(const std::vector<uint8_t>& pdr)
{
    const uint8_t* ptr = pdr.data();
    auto parsedPdr = std::make_shared<pldm_numeric_effecter_value_pdr>();
    size_t count = (uint8_t*)(&parsedPdr->max_set_table.value_u8) -
                   (uint8_t*)(&parsedPdr->hdr);
    memcpy(&parsedPdr->hdr, ptr, count);
    ptr += count;

    switch (parsedPdr->effecter_data_size)
    {
        case PLDM_EFFECTER_DATA_SIZE_UINT8:
        case PLDM_EFFECTER_DATA_SIZE_SINT8:
            parsedPdr->max_set_table.value_u8 = *((uint8_t*)ptr);
            ptr += sizeof(parsedPdr->max_set_table.value_u8);
            parsedPdr->min_set_table.value_u8 = *((uint8_t*)ptr);
            ptr += sizeof(parsedPdr->min_set_table.value_u8);
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT16:
        case PLDM_EFFECTER_DATA_SIZE_SINT16:
            parsedPdr->max_set_table.value_u16 = le16toh(*((uint16_t*)ptr));
            ptr += sizeof(parsedPdr->max_set_table.value_u16);
            parsedPdr->min_set_table.value_u16 = le16toh(*((uint16_t*)ptr));
            ptr += sizeof(parsedPdr->min_set_table.value_u16);
            break;
        case PLDM_EFFECTER_DATA_SIZE_UINT32:
        case PLDM_EFFECTER_DATA_SIZE_SINT32:
            parsedPdr->max_set_table.value_u32 = le32toh(*((uint32_t*)ptr));
            ptr += sizeof(parsedPdr->max_set_table.value_u32);
            parsedPdr->min_set_table.value_u32 = le32toh(*((uint32_t*)ptr));
            ptr += sizeof(parsedPdr->min_set_table.value_u32);
            break;
        default:
            break;
    }

    count = (uint8_t*)&parsedPdr->nominal_value.value_u8 -
            (uint8_t*)&parsedPdr->range_field_format;
    memcpy(&parsedPdr->range_field_format, ptr, count);
    ptr += count;

    switch (parsedPdr->range_field_format)
    {
        case PLDM_RANGE_FIELD_FORMAT_UINT8:
        case PLDM_RANGE_FIELD_FORMAT_SINT8:
            parsedPdr->nominal_value.value_u8 = *((uint8_t*)ptr);
            ptr += sizeof(parsedPdr->nominal_value.value_u8);
            parsedPdr->normal_max.value_u8 = *((uint8_t*)ptr);
            ptr += sizeof(parsedPdr->normal_max.value_u8);
            parsedPdr->normal_min.value_u8 = *((uint8_t*)ptr);
            ptr += sizeof(parsedPdr->normal_min.value_u8);
            parsedPdr->rated_max.value_u8 = *((uint8_t*)ptr);
            ptr += sizeof(parsedPdr->rated_max.value_u8);
            parsedPdr->rated_min.value_u8 = *((uint8_t*)ptr);
            ptr += sizeof(parsedPdr->rated_min.value_u8);
            break;
        case PLDM_RANGE_FIELD_FORMAT_UINT16:
        case PLDM_RANGE_FIELD_FORMAT_SINT16:
            parsedPdr->nominal_value.value_u16 = le16toh(*((uint16_t*)ptr));
            ptr += sizeof(parsedPdr->nominal_value.value_u16);
            parsedPdr->normal_max.value_u16 = le16toh(*((uint16_t*)ptr));
            ptr += sizeof(parsedPdr->normal_max.value_u16);
            parsedPdr->normal_min.value_u16 = le16toh(*((uint16_t*)ptr));
            ptr += sizeof(parsedPdr->normal_min.value_u16);
            parsedPdr->rated_max.value_u16 = le16toh(*((uint16_t*)ptr));
            ptr += sizeof(parsedPdr->rated_max.value_u16);
            parsedPdr->rated_min.value_u16 = le16toh(*((uint16_t*)ptr));
            ptr += sizeof(parsedPdr->rated_min.value_u16);
            break;
        case PLDM_RANGE_FIELD_FORMAT_UINT32:
        case PLDM_RANGE_FIELD_FORMAT_SINT32:
        case PLDM_RANGE_FIELD_FORMAT_REAL32:
            parsedPdr->nominal_value.value_u32 = le32toh(*((uint32_t*)ptr));
            ptr += sizeof(parsedPdr->nominal_value.value_u32);
            parsedPdr->normal_max.value_u32 = le32toh(*((uint32_t*)ptr));
            ptr += sizeof(parsedPdr->normal_max.value_u32);
            parsedPdr->normal_min.value_u32 = le32toh(*((uint32_t*)ptr));
            ptr += sizeof(parsedPdr->normal_min.value_u32);
            parsedPdr->rated_max.value_u32 = le32toh(*((uint32_t*)ptr));
            ptr += sizeof(parsedPdr->rated_max.value_u32);
            parsedPdr->rated_min.value_u32 = le32toh(*((uint32_t*)ptr));
            ptr += sizeof(parsedPdr->rated_min.value_u32);
            break;
        default:
            break;
    }
    return parsedPdr;
}

std::tuple<SensorID, StateSetInfo>
    Terminus::parseStateSensorPDR(std::vector<uint8_t>& stateSensorPdr)
{
    auto pdr =
        reinterpret_cast<const pldm_state_sensor_pdr*>(stateSensorPdr.data());
    std::vector<StateSetData> stateSets{};
    auto statesPtr = pdr->possible_states;
    auto compositeSensorCount = pdr->composite_sensor_count;

    parseStateSetInfo(statesPtr, compositeSensorCount, stateSets);

    auto entityInfo =
        std::make_tuple(static_cast<ContainerID>(pdr->container_id),
                        static_cast<EntityType>(pdr->entity_type),
                        static_cast<EntityInstance>(pdr->entity_instance));

    auto stateSetInfo =
        std::make_tuple(std::move(entityInfo), std::move(stateSets));
    return std::make_tuple(pdr->sensor_id, std::move(stateSetInfo));
}

std::tuple<EffecterID, StateSetInfo>
    Terminus::parseStateEffecterPDR(std::vector<uint8_t>& stateEffecterPdr)
{
    auto pdr = reinterpret_cast<const pldm_state_effecter_pdr*>(
        stateEffecterPdr.data());
    std::vector<StateSetData> stateSets{};
    auto statesPtr = pdr->possible_states;
    auto compositeSensorCount = pdr->composite_effecter_count;

    parseStateSetInfo(statesPtr, compositeSensorCount, stateSets);

    auto entityInfo =
        std::make_tuple(static_cast<ContainerID>(pdr->container_id),
                        static_cast<EntityType>(pdr->entity_type),
                        static_cast<EntityInstance>(pdr->entity_instance));
    auto stateSetInfo =
        std::make_tuple(std::move(entityInfo), std::move(stateSets));
    return std::make_tuple(pdr->effecter_id, std::move(stateSetInfo));
}

void Terminus::parseStateSetInfo(const unsigned char* statesPtr,
                                 uint8_t compositeSensorCount,
                                 std::vector<StateSetData>& stateSets)
{
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
        stateSets.emplace_back(
            std::make_tuple(stateSedId, std::move(possibleStates)));
        if (compositeSensorCount)
        {
            statesPtr += sizeof(state_sensor_possible_states) +
                         state->possible_states_size - 1;
        }
    }
}

OemPdr Terminus::parseOemPDR(const std::vector<uint8_t>& oemPdr)
{
    auto pdr = reinterpret_cast<const pldm_oem_pdr*>(oemPdr.data());
    std::vector<uint8_t> data;

    // vendor-specific data bytes starting from 0; 0 = 1 byte, 1 = 2 bytes,
    // and so on.
    data.resize(pdr->data_length + 1);
    memcpy(data.data(), pdr->vendor_specific_data, data.size());
    return std::make_tuple(pdr->vendor_iana, pdr->ome_record_id,
                           std::move(data));
}

bool Terminus::scanInventories()
{
    std::vector<std::string> interestedInterfaces;
    interestedInterfaces.emplace_back(overallSystemInterface);
    for (const auto& [entitytype, entityIface] : entityInterfaces)
    {
        interestedInterfaces.emplace_back(entityIface);
    }

    try
    {
        auto getSubTreeResponse = utils::DBusHandler().getSubtree(
            "/xyz/openbmc_project/inventory", 0, interestedInterfaces);
        // default system inventory object path
        systemInventoryPath =
            "/xyz/openbmc_project/inventory/system/chassis/Baseboard_0";

        inventories.clear();
        for (const auto& [objPath, mapperServiceMap] : getSubTreeResponse)
        {
            EntityType type = 0;
            EntityInstance instanceNumber = 0xFFFF;
            for (const auto& [serviceName, interfaces] : mapperServiceMap)
            {
                for (const auto& interface : interfaces)
                {
                    if (interface == overallSystemInterface)
                    {
                        if (std::find(interfaces.begin(), interfaces.end(),
                                      chassisInterface) == interfaces.end())
                        {
                            // The system should also has Chassis interface,
                            // continue if not.
                            continue;
                        }
                        systemInventoryPath = objPath;
                        continue;
                    }
                    if (interface == instanceInterface)
                    {
                        instanceNumber =
                            utils::DBusHandler().getDbusProperty<uint64_t>(
                                objPath.c_str(), instanceProperty,
                                instanceInterface);
                        continue;
                    }
                    for (const auto& [entitytype, entityIface] :
                         entityInterfaces)
                    {
                        if (interface == entityIface)
                        {
                            type = entitytype;
                            break;
                        }
                    }
                }
            }
            if (checkDeviceInventory(objPath))
            {

                inventories.emplace_back(objPath, type, instanceNumber);
            }
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to scan inventories Error: {ERROR}", "ERROR", e);
        return false;
    }
    return true;
}

void Terminus::updateAssociations()
{
    entities.clear();

    // Create entities
    auto createEntity = [&](const EntityInfo& entityInfo) {
        if (entities.find(entityInfo) == entities.end())
        {
            // find the inventory once to create entity
            findInventory(entityInfo, false);
        }
    };

    for (const auto& [containerId, entityAssociation] : entityAssociations)
    {
        const auto& [containerEntity, containedEntities] = entityAssociation;
        createEntity(containerEntity);
        for (const auto& containedEntity : containedEntities)
        {
            createEntity(containedEntity);
        }
    }

    for (const auto& ptr : numericSensors)
    {
        auto entityInfo = ptr->getEntityInfo();
        auto inventoryPath = findInventory(entityInfo);
        ptr->setInventoryPath(inventoryPath);

        auto type = toPhysicalContextType(std::get<1>(entityInfo));
        ptr->setPhysicalContext(type);
    }

    for (const auto& ptr : numericEffecters)
    {
        auto entityInfo = ptr->getEntityInfo();
        auto inventoryPath = findInventory(entityInfo);
        ptr->setInventoryPath(inventoryPath);

        auto type = toPhysicalContextType(std::get<1>(entityInfo));
        ptr->setPhysicalContext(type);
    }

    for (const auto& ptr : stateSensors)
    {
        auto entityInfo = ptr->getEntityInfo();
        auto inventoryPath = findInventory(entityInfo);
        ptr->setInventoryPath(inventoryPath);
        ptr->associateNumericSensor(numericSensors);
    }

    for (const auto& ptr : stateEffecters)
    {
        auto entityInfo = ptr->getEntityInfo();
        auto inventoryPath = findInventory(entityInfo);
        ptr->setInventoryPath(inventoryPath);
    }
}

std::string Terminus::findInventory(const EntityInfo entityInfo,
                                    const bool findClosest)
{
    // Search from stored result first
    auto itr = entities.find(entityInfo);
    if (itr != entities.end())
    {
        auto& entity = itr->second;
        if (findClosest)
        {
            return entity.getClosestInventory();
        }
        else
        {
            return entity.getInventory();
        }
    }

    const auto& [containerId, entityType, entityInstance] = entityInfo;
    auto ContainerInventoryPath = findInventory(containerId);

    // Search for possible inventory paths
    std::vector<std::string> candidates;
    for (const auto& [candidatePath, candidateType, candidateInstance] :
         inventories)
    {
        if ((entityType == candidateType) &&
            (entityInstance == candidateInstance))
        {
            candidates.push_back(candidatePath);
        }
    }

    std::string inventoryPath;
    if (candidates.empty())
    {
        inventoryPath.clear();
    }
    else if (candidates.size() == 1)
    {
        inventoryPath = candidates[0];
    }
    else
    {
        // default path if no one under parent path
        inventoryPath = candidates[0];

        // multiple inventories matched, find the one under parent path
        for (const auto& candidate : candidates)
        {
            if (candidate.starts_with(ContainerInventoryPath))
            {
                inventoryPath = candidate;
                break;
            }
        }
    }

    // Store the result, and also create parent_chassis/all_chassis
    // association
    entities.emplace(entityInfo, Entity{inventoryPath, ContainerInventoryPath});

    if (inventoryPath.size())
    {
        return inventoryPath;
    }
    else if (findClosest)
    {
        return ContainerInventoryPath;
    }
    else // empty inventoryPath
    {
        return inventoryPath;
    }
}

std::string Terminus::findInventory(const ContainerID containerId,
                                    const bool findClosest)
{
    if (containerId == overallSystemCotainerId)
    {
        return systemInventoryPath;
    }

    auto itr = entityAssociations.find(containerId);
    if (itr == entityAssociations.end())
    {
        lg2::error("cannot find containerId:{CONTAINERID}", "CONTAINERID",
                   containerId);
        return systemInventoryPath;
    }
    const auto& [containerEntity, containedEntities] = itr->second;
    return findInventory(containerEntity, findClosest);
}

void Terminus::addNumericSensor(
    const std::shared_ptr<pldm_numeric_sensor_value_pdr> pdr)
{
    std::string sensorName = "PLDM_Sensor_" + std::to_string(pdr->sensor_id) +
                             "_" + std::to_string(tid);

    auto sensorAuxiliaryNames = getSensorAuxiliaryNames(pdr->sensor_id);
    if (sensorAuxiliaryNames)
    {
        const auto& [sensorId, sensorCnt, sensorNames] = *sensorAuxiliaryNames;
        if (sensorCnt == 1 && sensorNames.size() > 0)
        {
            for (const auto& [languageTag, name] : sensorNames[0])
            {
                if (languageTag == "en")
                {
                    sensorName = name;
                }
            }
        }
    }

    try
    {
        auto sensor = std::make_shared<NumericSensor>(
            tid, true, pdr, sensorName, systemInventoryPath);
        numericSensors.emplace_back(sensor);
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to create NumericSensor:{SENSORNAME}, {ERROR}.",
                   "SENSORNAME", sensorName, "ERROR", e);
    }
}

#ifdef OEM_NVIDIA
void Terminus::addOEMEnergyCountNumericSensor(
    const std::shared_ptr<pldm_oem_energycount_numeric_sensor_value_pdr> pdr)
{
    std::string sensorName = "PLDM_Sensor_" + std::to_string(pdr->sensor_id) +
                             "_" + std::to_string(tid);

    auto sensorAuxiliaryNames = getSensorAuxiliaryNames(pdr->sensor_id);
    if (sensorAuxiliaryNames)
    {
        const auto& [sensorId, sensorCnt, sensorNames] = *sensorAuxiliaryNames;
        if (sensorCnt == 1 && sensorNames.size() > 0)
        {
            for (const auto& [languageTag, name] : sensorNames[0])
            {
                if (languageTag == "en")
                {
                    sensorName = name;
                }
            }
        }
    }

    try
    {
        auto sensor = std::make_shared<NumericSensor>(
            tid, true, pdr, sensorName, systemInventoryPath,
            POLLING_METHOD_INDICATOR_PLDM_TYPE_OEM);
        numericSensors.emplace_back(sensor);
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "Failed to create OEMEnergyCountNumericSensor:{SENSORNAME}, {ERROR}.",
            "SENSORNAME", sensorName, "ERROR", e);
    }
}
#endif

void Terminus::addNumericEffecter(
    const std::shared_ptr<pldm_numeric_effecter_value_pdr> pdr)
{
    std::string effecterName = "PLDM_Effecter_" +
                               std::to_string(pdr->effecter_id) + "_" +
                               std::to_string(tid);

    if (pdr->effecter_auxiliary_names)
    {
        auto effecterAuxiliaryNames =
            getEffecterAuxiliaryNames(pdr->effecter_id);
        if (effecterAuxiliaryNames)
        {
            const auto& [effecterId, effecterCnt, effecterNames] =
                *effecterAuxiliaryNames;
            if (effecterCnt == 1 && effecterNames.size() > 0)
            {
                for (const auto& [languageTag, name] : effecterNames[0])
                {
                    if (languageTag == "en")
                    {
                        effecterName = name;
                    }
                }
            }
        }
    }

    try
    {
        auto effecter = std::make_shared<NumericEffecter>(
            tid, true, pdr, effecterName, systemInventoryPath, terminusManager);
        numericEffecters.emplace_back(effecter);
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to create NumericEffecter:{EFFECTERNAME}, {ERROR}.",
                   "EFFECTERNAME", effecterName, "ERROR", e);
    }
}

void Terminus::addStateSensor(SensorID sId, StateSetInfo sensorInfo)
{
    std::string sensorName =
        "PLDM_Sensor_" + std::to_string(sId) + "_" + std::to_string(tid);

    auto sensorAuxiliaryNames = getSensorAuxiliaryNames(sId);
    std::vector<std::vector<std::pair<NameLanguageTag, SensorName>>>*
        sensorNames = nullptr;
    if (sensorAuxiliaryNames)
    {
        sensorNames = &(std::get<2>(*sensorAuxiliaryNames));
    }

    try
    {
        auto sensor =
            std::make_shared<StateSensor>(tid, true, sId, std::move(sensorInfo),
                                          sensorNames, systemInventoryPath);
        stateSensors.emplace_back(sensor);
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to create StateSensor:{SENSORNAME}, {ERROR}.",
                   "SENSORNAME", sensorName, "ERROR", e);
    }
}

void Terminus::addStateEffecter(EffecterID eId, StateSetInfo effecterInfo)
{
    std::string effecterName =
        "PLDM_Effecter_" + std::to_string(eId) + "_" + std::to_string(tid);

    auto effecterAuxiliaryNames = getEffecterAuxiliaryNames(eId);
    std::vector<std::vector<std::pair<NameLanguageTag, SensorName>>>*
        effecterNames = nullptr;
    if (effecterAuxiliaryNames)
    {
        effecterNames = &(std::get<2>(*effecterAuxiliaryNames));
    }

    try
    {
        auto effecter = std::make_shared<StateEffecter>(
            tid, true, eId, std::move(effecterInfo), effecterNames,
            systemInventoryPath, terminusManager);
        stateEffecters.emplace_back(effecter);
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to create NumericEffecter:{EFFECTERNAME}, {ERROR}.",
                   "EFFECTERNAME", effecterName, "ERROR", e);
    }
}

PhysicalContextType Terminus::toPhysicalContextType(const EntityType entityType)
{
    switch (entityType)
    {
        case PLDM_ENTITY_MEMORY_CONTROLLER:
            return PhysicalContextType::Memory;
        case PLDM_ENTITY_PROC:
            return PhysicalContextType::CPU;
        case PLDM_ENTITY_PROC_MODULE:
        case PLDM_ENTITY_PROC_IO_MODULE:
            // todo: define new PhysicalContextType enum for PROC_MODULE
            return PhysicalContextType::CPU;
        case PLDM_ENTITY_DC_DC_CONVERTER:
        case PLDM_ENTITY_POWER_CONVERTER:
            return PhysicalContextType::VoltageRegulator;
        case PLDM_ENTITY_NETWORK_CONTROLLER:
            return PhysicalContextType::NetworkingDevice;
        case PLDM_ENTITY_SYS_BOARD:
        default:
            break;
    }
    return PhysicalContextType::SystemBoard;
}

} // namespace platform_mc
} // namespace pldm
