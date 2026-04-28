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
#include <filesystem>
#include <format>

#ifdef OEM_NVIDIA
#include "oem/nvidia/platform-mc/oem_nvidia.hpp"
#endif

#include "common/dBusAsyncUtils.hpp"
#include "common/utils.hpp"
#include "terminus.hpp"
#include "terminus_manager.hpp"

#include <cstring>

namespace pldm
{
namespace platform_mc
{

Terminus::Terminus(tid_t tid, uint64_t supportedTypes, UUID& uuid,
                   TerminusManager& terminusManager) :
    initalized(false), pollEvent(false), ready(false),
    synchronyConfigurationSupported(0), resumed(true), initSensorList(true),
    tid(tid), supportedTypes(supportedTypes), uuid(uuid),
    terminusManager(terminusManager)
{
    // default system inventory object path
    systemInventoryPath = PLATFORM_CHASSIS_PATH;
    maxBufferSize = 256;
    needRefresh = false;
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
#ifdef OEM_NVIDIA
        if (intf == "xyz.openbmc_project.Configuration.NsmDeviceAssociation")
        {
            needRefresh = true;
        }
#endif
        if (needRefresh)
        {
            break;
        }
    }

    if (needRefresh)
    {
        refreshAssociations();
    }
}

bool Terminus::checkNsmDeviceInventory(UUID nsmUuid)
{
    if (nsmUuid.substr(0, 36) == uuid.substr(0, 36))
    {
        return true;
    }
    else
    {
        return false;
    }
}

exec::task<int> Terminus::checkDeviceInventory(const std::string& objPath)
{
    try
    {
        auto getSubTreeResponse = co_await utils::coGetSubTree(
            objPath, 0,
            {"xyz.openbmc_project.Configuration.I2CDeviceAssociation",
             "xyz.openbmc_project.Configuration.USBDeviceAssociation",
             "xyz.openbmc_project.Configuration.NsmDeviceAssociation"});

        if (getSubTreeResponse.size() == 0)
        {
            co_return PLDM_SUCCESS;
        }

        const std::optional<MctpInfo> mctpInfo =
            terminusManager.toMctpInfo(tid);

        if (!mctpInfo.has_value())
        {
            co_return PLDM_FAILED;
        }

        const EID terminusEid = std::get<0>(mctpInfo.value());
        bool found = false;

        for (const auto& [objectPath, serviceMap] : getSubTreeResponse)
        {
            for (const auto& [serviceName, interfaces] : serviceMap)
            {
                for (const auto& interface : interfaces)
                {
                    uint64_t bus = 0;
                    uint64_t addr = 0;
                    uint64_t inventoryEid = 0;
                    if (interface ==
                        "xyz.openbmc_project.Configuration.I2CDeviceAssociation")
                    {
                        bus = co_await utils::coGetDbusProperty<uint64_t>(
                            objectPath.c_str(), "Bus",
                            "xyz.openbmc_project.Configuration.I2CDeviceAssociation",
                            serviceName);
                        addr = co_await utils::coGetDbusProperty<uint64_t>(
                            objectPath.c_str(), "Address",
                            "xyz.openbmc_project.Configuration.I2CDeviceAssociation",
                            serviceName);
                        inventoryEid = co_await utils::coGetDbusProperty<
                            uint64_t>(
                            objectPath.c_str(), "EID",
                            "xyz.openbmc_project.Configuration.I2CDeviceAssociation",
                            serviceName);
                        if (terminusEid == inventoryEid)
                        {
                            found = true;
                        }
                    }
                    else if (
                        interface ==
                        "xyz.openbmc_project.Configuration.USBDeviceAssociation")
                    {
                        inventoryEid = co_await utils::coGetDbusProperty<
                            uint64_t>(
                            objectPath.c_str(), "EID",
                            "xyz.openbmc_project.Configuration.USBDeviceAssociation");
                        if (terminusEid == inventoryEid)
                        {
                            found = true;
                        }
                    }
                    else if (
                        interface ==
                        "xyz.openbmc_project.Configuration.NsmDeviceAssociation")
                    {
                        auto nsmUuid = co_await utils::coGetDbusProperty<
                            std::string>(
                            objectPath.c_str(), "UUID",
                            "xyz.openbmc_project.Configuration.NsmDeviceAssociation",
                            serviceName);
                        found = checkNsmDeviceInventory(nsmUuid);
                    }

                    if (found)
                    {
                        co_await getSensorAuxNameFromEM(bus, addr, inventoryEid,
                                                        objPath);
#ifdef OEM_NVIDIA
                        co_await getPortInfoFromEM(objPath);
                        co_await getInfoForNVSwitchFromEM(objPath);
                        co_await getSensorEventInfoFromEM(objPath);
#endif
                        co_return PLDM_SUCCESS;
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

    co_return PLDM_FAILED;
}

exec::task<int> Terminus::getSensorAuxNameFromEM(
    uint8_t bus, uint8_t addr, uint8_t eid, const std::string& objPath)
{
    static constexpr const char* sensorAuxInterface =
        "xyz.openbmc_project.Configuration.SensorAuxName";

    try
    {
        sensorAuxNameOverwriteTbl.clear();

        auto getSubTreeResponse =
            co_await utils::coGetSubTree(objPath, 0, {sensorAuxInterface});

        if (getSubTreeResponse.size() == 0)
        {
            co_return PLDM_SUCCESS;
        }

        for (const auto& [path, mapperServiceMap] : getSubTreeResponse)
        {
            if (mapperServiceMap.empty()) [[unlikely]]
            {
                lg2::error("No Service found for path: {PATH}. Skipping.",
                           "PATH", path);
                continue;
            }

            if (mapperServiceMap.size() > 1) [[unlikely]]
            {
                lg2::error(
                    "More than one service found for the same path: {PATH}. "
                    "A path should be globally unique. Skipping.",
                    "PATH", path);
                continue;
            }

            const auto& [serviceName, interfaces] = *(mapperServiceMap.begin());

            const utils::PropertyMap properties =
                utils::DBusHandler().getDbusPropertiesVariant(
                    serviceName.c_str(), path.c_str(), sensorAuxInterface);

            if (!properties.contains("SensorId") ||
                !properties.contains("AuxNames")) [[unlikely]]
            {
                lg2::error(
                    "Unable to find mandatory properties SensorId, AuxNames for:"
                    "{PATH}. Skipping.",
                    "PATH", path);
                continue;
            }

            const auto sensorId = std::get<uint64_t>(properties.at("SensorId"));

            const auto auxNames =
                std::get<std::vector<std::string>>(properties.at("AuxNames"));

            // Check Bus/Address property if they exist
            if (properties.contains("Bus"))
            {
                const auto mctpI2cBus =
                    std::get<uint64_t>(properties.at("Bus"));
                if (mctpI2cBus != 0 && mctpI2cBus != bus)
                {
                    continue;
                }
            }

            if (properties.contains("Address"))
            {
                const auto mctpI2cAddr =
                    std::get<uint64_t>(properties.at("Address"));
                if (mctpI2cAddr != 0 && mctpI2cAddr != addr)
                {
                    continue;
                }
            }

            if (properties.contains("EID"))
            {
                const auto mctpEid = std::get<uint64_t>(properties.at("EID"));
                if (mctpEid != 0 && mctpEid != eid)
                {
                    continue;
                }
            }

            AuxiliaryNames auxNameTbl;
            for (auto auxName : auxNames)
            {
                auxNameTbl.push_back({{"en", auxName}});
            }

            auto parentPathEM =
                co_await pldm::utils::coGetDbusProperty<std::string>(
                    path.c_str(), "ParentObjPath",
                    "xyz.openbmc_project.Configuration.SensorAuxName");

            sensorAuxNameOverwriteTbl[sensorId] =
                std::make_tuple(auxNameTbl, parentPathEM);
        }
    }
    catch (const std::exception& e)
    {
        lg2::info("no Configuration.SensorAuxName Error: {ERROR} path:{PATH}",
                  "ERROR", e, "PATH", objPath);
    }
    co_return PLDM_SUCCESS;
}

#ifdef OEM_NVIDIA
exec::task<int> Terminus::getPortInfoFromEM(const std::string& objPath)
{
    try
    {
        sensorPortInfoOverwriteTbl.clear();

        auto getSubTreeResponse = co_await utils::coGetSubTree(
            objPath, 0, {"xyz.openbmc_project.Configuration.SensorPortInfo"});

        if (getSubTreeResponse.size() == 0)
        {
            co_return PLDM_FAILED;
        }

        for (auto& [path, mapperServiceMap] : getSubTreeResponse)
        {
            auto sensorId = co_await pldm::utils::coGetDbusProperty<uint64_t>(
                path.c_str(), "SensorId",
                "xyz.openbmc_project.Configuration.SensorPortInfo");
            auto maxSpeed = co_await pldm::utils::coGetDbusProperty<uint64_t>(
                path.c_str(), "MaxSpeedMBps",
                "xyz.openbmc_project.Configuration.SensorPortInfo");
            auto portType =
                co_await pldm::utils::coGetDbusProperty<std::string>(
                    path.c_str(), "PortType",
                    "xyz.openbmc_project.Configuration.SensorPortInfo");
            auto portProtocol =
                co_await pldm::utils::coGetDbusProperty<std::string>(
                    path.c_str(), "PortProtocol",
                    "xyz.openbmc_project.Configuration.SensorPortInfo");
            auto associationsEM = co_await pldm::utils::coGetDbusProperty<
                std::vector<std::string>>(
                path.c_str(), "Association",
                "xyz.openbmc_project.Configuration.SensorPortInfo");

            std::vector<dbus::PathAssociation> associations;
            if (associationsEM.size() % 3 != 0)
            {
                lg2::error(
                    "Association in port info must follow (fwd, bck, Path) for {OBJ}",
                    "OBJ", path);
                co_return PLDM_FAILED;
            }

            for (size_t it = 0; it < associationsEM.size(); it += 3)
            {
                associations.push_back({});
                auto& tmp = associations.back();

                tmp.forward = associationsEM[it];
                tmp.reverse = associationsEM[it + 1];
                tmp.path = associationsEM[it + 2];
            }

            sensorPortInfoOverwriteTbl[sensorId] = std::make_tuple(
                PortInfoIntf::convertPortTypeFromString(portType), portProtocol,
                maxSpeed, associations);
        }
    }
    catch (const std::exception& e)
    {
        lg2::info("no Configuration.SensorPortInfo Error: {ERROR} path:{PATH}",
                  "ERROR", e, "PATH", objPath);
    }
    co_return PLDM_SUCCESS;
}

exec::task<int> Terminus::getInfoForNVSwitchFromEM(const std::string& objPath)
{
    if (switchBandwidthSensor)
    {
        co_return PLDM_SUCCESS;
    }

    try
    {
        auto getSubTreeResponse = co_await utils::coGetSubTree(
            objPath, 0,
            {"xyz.openbmc_project.Configuration.NSM_NVSwitch.Switch"});

        if (getSubTreeResponse.size() == 0)
        {
            co_return PLDM_FAILED;
        }

        for (auto& [path, mapperServiceMap] : getSubTreeResponse)
        {
            auto name = co_await pldm::utils::coGetDbusProperty<std::string>(
                path.c_str(), "Name",
                "xyz.openbmc_project.Configuration.NSM_NVSwitch.Switch");
            auto switchType =
                co_await pldm::utils::coGetDbusProperty<std::string>(
                    path.c_str(), "SwitchType",
                    "xyz.openbmc_project.Configuration.NSM_NVSwitch.Switch");
            auto switchSupportedProtocols =
                co_await pldm::utils::coGetDbusProperty<
                    std::vector<std::string>>(
                    path.c_str(), "SwitchSupportedProtocols",
                    "xyz.openbmc_project.Configuration.NSM_NVSwitch.Switch");
            auto associationsEM = co_await pldm::utils::coGetDbusProperty<
                std::vector<std::string>>(
                path.c_str(), "Association",
                "xyz.openbmc_project.Configuration.NSM_NVSwitch.Switch");

            std::vector<dbus::PathAssociation> associations;
            if (associationsEM.size() % 3 != 0)
            {
                lg2::error(
                    "Association in switch info must follow (fwd, bck, Path) for {OBJ}",
                    "OBJ", path);
                co_return PLDM_FAILED;
            }

            for (size_t it = 0; it < associationsEM.size(); it += 3)
            {
                associations.push_back({});
                auto& tmp = associations.back();

                tmp.forward = associationsEM[it];
                tmp.reverse = associationsEM[it + 1];
                tmp.path = associationsEM[it + 2];
            }

            switchBandwidthSensor =
                std::make_shared<oem_nvidia::SwitchBandwidthSensor>(
                    tid, name, switchType, switchSupportedProtocols,
                    associations);
        }
    }
    catch (const std::exception& e)
    {
        lg2::info("no Configuration.NSM_NVSwitch Error: {ERROR} path:{PATH}",
                  "ERROR", e, "PATH", objPath);
    }
    co_return PLDM_SUCCESS;
}

exec::task<int> Terminus::getSensorEventInfoFromEM(const std::string& objPath)
{
    try
    {
        auto getSubTreeResponse = co_await utils::coGetSubTree(
            objPath, 0, {"xyz.openbmc_project.Configuration.SensorEventInfo"});
        if (getSubTreeResponse.size() == 0)
        {
            co_return PLDM_FAILED;
        }
        for (auto& [path, mapperServiceMap] : getSubTreeResponse)
        {
            try
            {
                auto sensorId =
                    co_await pldm::utils::coGetDbusProperty<uint64_t>(
                        path.c_str(), "SensorId",
                        "xyz.openbmc_project.Configuration.SensorEventInfo");
                auto impactedComponent =
                    co_await pldm::utils::coGetDbusProperty<std::string>(
                        path.c_str(), "ImpactedComponent",
                        "xyz.openbmc_project.Configuration.SensorEventInfo");
                auto eventIdsEM = co_await pldm::utils::coGetDbusProperty<
                    std::vector<std::string>>(
                    path.c_str(), "EventIds",
                    "xyz.openbmc_project.Configuration.SensorEventInfo");

                std::unordered_map<std::string, std::string> eventIdsMap;
                if (eventIdsEM.size() % 2 != 0)
                {
                    lg2::error(
                        "EventIds in sensor event info must follow (eventIdKey,"
                        " eventId) for {OBJ}",
                        "OBJ", path);
                    co_return PLDM_FAILED;
                }

                for (size_t it = 0; it < eventIdsEM.size(); it += 2)
                {
                    eventIdsMap[eventIdsEM[it]] = eventIdsEM[it + 1];
                }

                sensorEventInfoOverwriteTbl[sensorId] =
                    std::make_shared<utils::SensorEventInfo>(
                        impactedComponent, std::move(eventIdsMap));
            }
            catch (const std::exception& e)
            {
                lg2::error(
                    "Failed to parse SensorEventInfo for path {PATH}: {ERROR}",
                    "PATH", path, "ERROR", e);
            }
        }
    }
    catch (const std::exception& e)
    {
        lg2::info("no Configuration.SensorEventInfo Error: {ERROR} path:{PATH}",
                  "ERROR", e, "PATH", objPath);
    }
    co_return PLDM_SUCCESS;
}
#endif

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
            if (sensorAuxiliaryNames != nullptr)
            {
                sensorAuxiliaryNamesTbl.emplace_back(
                    std::move(sensorAuxiliaryNames));
            }
        }
        else if (pdrHdr->type == PLDM_EFFECTER_AUXILIARY_NAMES_PDR)
        {
            auto effecterAuxiliaryNames = parseEffecterAuxiliaryNamesPDR(pdr);
            if (effecterAuxiliaryNames != nullptr)
            {
                effecterAuxiliaryNamesTbl.emplace_back(
                    std::move(effecterAuxiliaryNames));
            }
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
            if (parsedPdr != nullptr)
            {
                numericEffecterPdrs.emplace_back(std::move(parsedPdr));
            }
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

    if (!interfaceAddedMatch)
    {
        interfaceAddedMatch = std::make_unique<sdbusplus::bus::match_t>(
            utils::DBusHandler().getBus(),
            sdbusplus::bus::match::rules::interfacesAdded(
                "/xyz/openbmc_project/inventory"),
            std::bind(std::mem_fn(&Terminus::interfaceAdded), this,
                      std::placeholders::_1));
    }
    return rc;
}

std::shared_ptr<SensorAuxiliaryNames> Terminus::getSensorAuxiliaryNames(
    SensorID id)
{
    if (sensorAuxNameOverwriteTbl.find(id) != sensorAuxNameOverwriteTbl.end())
    {
        return std::make_shared<SensorAuxiliaryNames>(
            id, 1, std::get<0>(sensorAuxNameOverwriteTbl[id]));
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

std::optional<ParentObjPath> Terminus::getInventoryPath(SensorID id)
{
    if (sensorAuxNameOverwriteTbl.find(id) != sensorAuxNameOverwriteTbl.end())
    {
        auto& path = std::get<1>(sensorAuxNameOverwriteTbl[id]);
        if (!path.empty() &&
            path.starts_with("/xyz/openbmc_project/inventory/"))
        {
            return path;
        }
    }
    return std::nullopt;
}

#ifdef OEM_NVIDIA
std::shared_ptr<std::tuple<PortType, std::string, uint64_t,
                           std::vector<dbus::PathAssociation>>>
    Terminus::getSensorPortInfo(SensorID id)
{
    if (sensorPortInfoOverwriteTbl.find(id) != sensorPortInfoOverwriteTbl.end())
    {
        return std::make_shared<std::tuple<PortType, std::string, uint64_t,
                                           std::vector<dbus::PathAssociation>>>(
            sensorPortInfoOverwriteTbl[id]);
    }
    return nullptr;
}

std::shared_ptr<utils::SensorEventInfo> Terminus::getSensorEventInfo(
    SensorID id)
{
    auto it = sensorEventInfoOverwriteTbl.find(id);
    if (it != sensorEventInfoOverwriteTbl.end())
    {
        return it->second;
    }
    return nullptr;
}
#endif

std::shared_ptr<EffecterAuxiliaryNames> Terminus::getEffecterAuxiliaryNames(
    EffecterID id)
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

std::shared_ptr<SensorAuxiliaryNames> Terminus::parseSensorAuxiliaryNamesPDR(
    const std::vector<uint8_t>& pdrData)
{
    constexpr uint8_t NullTerminator = 0;
    size_t parseLen = 0;
    auto pdr = reinterpret_cast<const struct pldm_sensor_auxiliary_names_pdr*>(
        pdrData.data());
    const uint8_t* ptr = pdr->names;
    parseLen += sizeof(pldm_sensor_auxiliary_names_pdr);
    // reducing by 1 byte because the length of pdr->names (names[1]) is not
    // pared yet at the moment.
    parseLen -= sizeof(pdr->names);
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
                std::vector<uint8_t> u16NameStringVec(
                    pdrData.begin() + parseLen, pdrData.end());
                std::u16string u16NameString(
                    reinterpret_cast<const char16_t*>(u16NameStringVec.data()),
                    0, PLDM_STR_UTF_16_MAX_LEN);
                ptr += (u16NameString.size() + sizeof(NullTerminator)) *
                       sizeof(uint16_t);
                parseLen += (u16NameString.size() + sizeof(NullTerminator)) *
                            sizeof(uint16_t);
                std::transform(u16NameString.cbegin(), u16NameString.cend(),
                               u16NameString.begin(),
                               [](uint16_t utf16) { return be16toh(utf16); });
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
                std::string nameString =
                    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,
                                         char16_t>{}
                        .to_bytes(u16NameString);
#pragma GCC diagnostic pop
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
        return nullptr;
    }

    return std::make_shared<SensorAuxiliaryNames>(
        pdr->sensor_id, pdr->sensor_count, sensorAuxNames);
}

std::shared_ptr<EffecterAuxiliaryNames>
    Terminus::parseEffecterAuxiliaryNamesPDR(
        const std::vector<uint8_t>& pdrData)
{
    constexpr uint8_t NullTerminator = 0;
    size_t parseLen = 0;
    auto pdr =
        reinterpret_cast<const struct pldm_effecter_auxiliary_names_pdr*>(
            pdrData.data());
    const uint8_t* ptr = pdr->names;
    // reducing by 1 byte because the length of pdr->names (names[1]) is not
    // pared yet at the moment.
    parseLen += sizeof(pldm_effecter_auxiliary_names_pdr) - sizeof(pdr->names);
    std::vector<std::vector<std::pair<NameLanguageTag, EffecterName>>>
        effecterAuxNames{};
    try
    {
        for (int i = 0; i < pdr->effecter_count; i++)
        {
            const uint8_t nameStringCount = static_cast<uint8_t>(*ptr);
            ptr += sizeof(uint8_t);
            parseLen += sizeof(uint8_t);
            std::vector<std::pair<NameLanguageTag, EffecterName>> nameStrings{};
            for (int j = 0; j < nameStringCount; j++)
            {
                std::string nameLanguageTag(reinterpret_cast<const char*>(ptr),
                                            0, PLDM_STR_UTF_8_MAX_LEN);
                ptr += nameLanguageTag.size() + sizeof(NullTerminator);
                parseLen += nameLanguageTag.size() + sizeof(NullTerminator);
                std::vector<uint8_t> u16NameStringVec(
                    pdrData.begin() + parseLen, pdrData.end());
                std::u16string u16NameString(
                    reinterpret_cast<const char16_t*>(u16NameStringVec.data()),
                    0, PLDM_STR_UTF_16_MAX_LEN);
                ptr += (u16NameString.size() + sizeof(NullTerminator)) *
                       sizeof(uint16_t);
                parseLen += (u16NameString.size() + sizeof(NullTerminator)) *
                            sizeof(uint16_t);
                std::transform(u16NameString.cbegin(), u16NameString.cend(),
                               u16NameString.begin(),
                               [](uint16_t utf16) { return be16toh(utf16); });
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
                std::string nameString =
                    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,
                                         char16_t>{}
                        .to_bytes(u16NameString);
#pragma GCC diagnostic pop
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
        return nullptr;
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

std::shared_ptr<pldm_numeric_sensor_value_pdr> Terminus::parseNumericSensorPDR(
    const std::vector<uint8_t>& pdr)
{
    const uint8_t* ptr = pdr.data();
    auto parsedPdr = std::make_shared<pldm_numeric_sensor_value_pdr>();
    auto rc = decode_numeric_sensor_pdr_data(ptr, pdr.size(), parsedPdr.get());
    if (rc)
    {
        return nullptr;
    }
    return parsedPdr;
}

std::shared_ptr<pldm_numeric_effecter_value_pdr>
    Terminus::parseNumericEffecterPDR(const std::vector<uint8_t>& pdr)
{
    auto parsedPdr = std::make_shared<pldm_numeric_effecter_value_pdr>();
    auto rc = decode_numeric_effecter_pdr_data(pdr.data(), pdr.size(),
                                               parsedPdr.get());
    if (rc)
    {
        /* WORKAROUND: CPU/SatMC firmware does not comply with DSP0248
         * Table 87, which requires max_settable and min_settable to be
         * sized exactly by effecter_data_size on the wire.  Instead, the
         * firmware always sends both fields as 8 bytes
         * (sizeof(union_effecter_data_size)), leaking the in-memory union
         * size into the wire format.  This causes
         * decode_numeric_effecter_pdr_data() to read range_field_format
         * from the wrong offset, failing validation.  When the PDR size
         * exactly matches the struct size, fall back to memcpy which is
         * compatible with the non-compliant firmware wire format. */
        if (pdr.size() != sizeof(*parsedPdr))
        {
            lg2::error("decode_numeric_effecter_pdr_data failed: rc={RC}, "
                       "pdr_size={SIZE}",
                       "RC", rc, "SIZE", pdr.size());
            return nullptr;
        }
        memcpy(parsedPdr.get(), pdr.data(), pdr.size());
    }
    return parsedPdr;
}

std::tuple<SensorID, StateSetInfo> Terminus::parseStateSensorPDR(
    std::vector<uint8_t>& stateSensorPdr)
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

std::tuple<EffecterID, StateSetInfo> Terminus::parseStateEffecterPDR(
    std::vector<uint8_t>& stateEffecterPdr)
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

exec::task<int> Terminus::getInventoryParent(const std::string objPath)
{
    const std::string assocPath = objPath + "/parent_chassis";
    const dbus::Interfaces assocIfaces = {"xyz.openbmc_project.Association"};

    /* Use the mapper to find which service owns the association object.
     * Hardcoding mapperService here is wrong — association objects are
     * owned by entity-manager or inventory services, not the mapper. */
    auto serviceMap = co_await utils::coGetServiceMap(assocPath, assocIfaces);
    if (serviceMap.empty())
    {
        co_return PLDM_SUCCESS;
    }

    const auto& service = serviceMap.begin()->first;
    auto parents = co_await utils::coGetDbusProperty<std::vector<std::string>>(
        assocPath, "endpoints", "xyz.openbmc_project.Association", service);
    if (parents.size())
    {
        inventoryParentMap[objPath] = parents[0];
    }
    co_return PLDM_SUCCESS;
}

exec::task<int> Terminus::scanInventories()
{
    std::vector<std::string> interestedInterfaces;
    interestedInterfaces.emplace_back(overallSystemInterface);
    for (const auto& [entitytype, entityIface] : entityInterfaces)
    {
        interestedInterfaces.emplace_back(entityIface);
    }

    try
    {
        auto getSubTreeResponse = co_await utils::coGetSubTree(
            "/xyz/openbmc_project/inventory", 0, interestedInterfaces);
        // default system inventory object path
        systemInventoryPath = PLATFORM_CHASSIS_PATH;

        inventories.clear();
        inventoryParentMap.clear();
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
                            co_await utils::coGetDbusProperty<uint64_t>(
                                objPath.c_str(), instanceProperty,
                                instanceInterface, serviceName);
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

            auto rc = co_await checkDeviceInventory(objPath);
            if (rc == PLDM_SUCCESS)
            {
                inventories.emplace_back(objPath, type, instanceNumber);
                co_await getInventoryParent(objPath);
                // Strip LOGICAL flag for comparison
                uint16_t rawType = type & 0x7FFF;
                if (rawType != PLDM_ENTITY_PROC &&
                    rawType != PLDM_ENTITY_PROC_IO_MODULE)
                {
                    continue;
                }
                try
                {
                    auto mapperServiceMap = co_await utils::coGetServiceMap(
                        objPath,
                        {"xyz.openbmc_project.Association.Definitions"});

                    if (mapperServiceMap.empty())
                    {
                        continue;
                    }
                    const auto assocs =
                        co_await utils::coGetDbusProperty<std::vector<
                            std::tuple<std::string, std::string, std::string>>>(
                            objPath.c_str(), "Associations",
                            "xyz.openbmc_project.Association.Definitions",
                            mapperServiceMap.begin()->first);

                    for (const auto& assoc : assocs)
                    {
                        std::string assocPath = std::get<2>(assoc);
                        if (std::get<1>(assoc) != "all_processors" ||
                            assocPath == objPath)
                        {
                            continue;
                        }
                        inventories.emplace_back(assocPath, type,
                                                 instanceNumber);
                        co_await getInventoryParent(assocPath);
                    }
                }
                catch (const std::exception& e)
                {
                    lg2::error("failed to query chassis cpu: {P} Error: {ERR}",
                               "P", objPath, "ERR", e);
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to scan inventories Error: {ERROR}", "ERROR", e);
        co_return PLDM_FAILED;
    }
    co_return PLDM_SUCCESS;
}

exec::task<int> Terminus::updateAssociations()
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
        auto name = getAuxNameForNumericSensor(ptr->sensorId);
        if (name)
        {
            // Prepend terminus name (+ entity type + cpu index) if available
            // and name is not from Entity Manager
            std::string updatedName = *name;
            if (!terminusName.empty() &&
                sensorAuxNameOverwriteTbl.find(ptr->sensorId) ==
                    sensorAuxNameOverwriteTbl.end())
            {
                auto entityInfo = ptr->getEntityInfo();
                auto prefix = buildSensorNamePrefix(std::get<1>(entityInfo));
                // FixME: Backward compat — old firmware includes
                // entity-type + CPU index in aux name (e.g. "CPU_0_X").
                // Detect and prepend only terminus name; remove when
                // all firmware sends plain aux names.
                auto entityTag = buildEntityTypeTag(std::get<1>(entityInfo));
                if (!entityTag.empty() &&
                    updatedName.starts_with(entityTag + "_"))
                {
                    updatedName =
                        std::format("{}_{}", terminusName, updatedName);
                }
                else
                {
                    updatedName = std::format("{}_{}", prefix, updatedName);
                }
            }
            ptr->updateSensorName(updatedName);
        }
        else
        {
            if (!terminusManager.numericSensorsWithoutAuxName)
            {
                ptr->removeValueIntf();
                continue;
            }
        }

        auto entityInfo = ptr->getEntityInfo();
        std::vector<ParentObjPath> inventoryPaths;
        auto inventoryPath = getInventoryPath(ptr->sensorId);
        if (inventoryPath)
        {
            inventoryPaths.push_back(*inventoryPath);
        }
        else
        {
            inventoryPaths = findInventory(entityInfo);
            // PROC_IO_MODULE sensors belong under the CPU chassis; find the
            // CPU child within the module container and use its inventory path.
            if ((std::get<1>(entityInfo) & 0x7FFF) ==
                PLDM_ENTITY_PROC_IO_MODULE)
            {
                for (const auto& [cid, assoc] : entityAssociations)
                {
                    const auto& [containerEntity, containedEntities] = assoc;
                    if ((std::get<1>(containerEntity) & 0x7FFF) !=
                        PLDM_ENTITY_PROC_IO_MODULE)
                    {
                        continue;
                    }
                    for (const auto& child : containedEntities)
                    {
                        if ((std::get<1>(child) & 0x7FFF) == PLDM_ENTITY_PROC)
                        {
                            auto cpuPaths = findInventory(child, true);
                            if (!cpuPaths.empty())
                            {
                                inventoryPaths = cpuPaths;
                            }
                            break;
                        }
                    }
                    break;
                }
            }
            // SYS_BUS numeric sensors (e.g. CLink/NVLink counters for
            // bandwidth, CRC, replay) should be associated with the
            // CPU in the containing ProcessorModule. SYS_BUS has no
            // inventory interface so findInventory returns empty; walk
            // up to ProcessorModule and find the CPU sibling.
            else if ((std::get<1>(entityInfo) & 0x7FFF) == PLDM_ENTITY_SYS_BUS)
            {
                const auto& containerId = std::get<0>(entityInfo);
                auto containerItr = entityAssociations.find(containerId);
                if (containerItr != entityAssociations.end())
                {
                    const auto& [containerEntity, containedEntities] =
                        containerItr->second;
                    uint16_t containerType =
                        std::get<1>(containerEntity) & 0x7FFF;
                    if (containerType == PLDM_ENTITY_PROC)
                    {
                        auto cpuPaths = findInventory(containerEntity, true);
                        if (!cpuPaths.empty())
                        {
                            inventoryPaths = cpuPaths;
                        }
                    }
                    else if (containerType == PLDM_ENTITY_PROC_IO_MODULE)
                    {
                        for (const auto& child : containedEntities)
                        {
                            if ((std::get<1>(child) & 0x7FFF) ==
                                PLDM_ENTITY_PROC)
                            {
                                auto cpuPaths = findInventory(child, true);
                                if (!cpuPaths.empty())
                                {
                                    inventoryPaths = cpuPaths;
                                }
                                break;
                            }
                        }
                    }
                }
            }
            // Workaround: count/metric sensors (baseUnit == COUNTS) are
            // global per CPU (e.g. PageRetirementCount, TjMaxDramIndex),
            // so associate with CPU regardless of entity type.
            else if (ptr->getBaseUnit() == PLDM_SENSOR_UNIT_COUNTS)
            {
                const auto& containerId = std::get<0>(entityInfo);
                auto containerItr = entityAssociations.find(containerId);
                if (containerItr != entityAssociations.end())
                {
                    const auto& containerEntity = containerItr->second.first;
                    if ((std::get<1>(containerEntity) & 0x7FFF) ==
                        PLDM_ENTITY_PROC)
                    {
                        auto cpuPaths = findInventory(containerEntity, true);
                        if (!cpuPaths.empty())
                        {
                            inventoryPaths = cpuPaths;
                        }
                    }
                }
            }
        }
        ptr->setInventoryPaths(inventoryPaths, false);

        auto type = toPhysicalContextType(std::get<1>(entityInfo));
        ptr->setPhysicalContext(type);
    }

    for (const auto& ptr : numericEffecters)
    {
        auto entityInfo = ptr->getEntityInfo();
        auto inventoryPath = findInventory(entityInfo);
        ptr->setInventoryPaths(inventoryPath);

        auto type = toPhysicalContextType(std::get<1>(entityInfo));
        ptr->setPhysicalContext(type);
    }

    auto hasStateSetId = [](const StateSetInfo& info, uint16_t id) {
        for (const auto& sd : std::get<1>(info))
            if (std::get<0>(sd) == id)
                return true;
        return false;
    };

    for (const auto& ptr : stateSensors)
    {
        auto entityInfo = ptr->getEntityInfo();
        std::vector<ParentObjPath> inventoryPaths;
        auto inventoryPath = getInventoryPath(ptr->sensorId);
        if (inventoryPath)
        {
            inventoryPaths.push_back(*inventoryPath);
        }
        else
        {
            inventoryPaths = findInventory(entityInfo);
        }

        // SYS_BUS state sensors (CLink/NVLink port state) need CPU
        // associations. SYS_BUS has no inventory interface so
        // findInventory returns empty; walk up to the container and
        // locate the CPU entity.
        if ((std::get<1>(entityInfo) & 0x7FFF) == PLDM_ENTITY_SYS_BUS &&
            inventoryPaths.empty())
        {
            const auto& containerId = std::get<0>(entityInfo);
            auto containerItr = entityAssociations.find(containerId);
            if (containerItr != entityAssociations.end())
            {
                const auto& [containerEntity, containedEntities] =
                    containerItr->second;
                uint16_t containerType = std::get<1>(containerEntity) & 0x7FFF;
                if (containerType == PLDM_ENTITY_PROC)
                {
                    auto cpuPaths = findInventory(containerEntity, true);
                    if (!cpuPaths.empty())
                    {
                        inventoryPaths = cpuPaths;
                    }
                }
                else if (containerType == PLDM_ENTITY_PROC_IO_MODULE)
                {
                    for (const auto& child : containedEntities)
                    {
                        if ((std::get<1>(child) & 0x7FFF) == PLDM_ENTITY_PROC)
                        {
                            auto cpuPaths = findInventory(child, true);
                            if (!cpuPaths.empty())
                            {
                                inventoryPaths = cpuPaths;
                            }
                            break;
                        }
                    }
                }
            }
        }

        // Workaround: MEMORY_CONTROLLER state sensors (e.g.
        // MemorySpareChannelPresence) are CPU-global; associate with CPU
        // instead of ProcessorModule when container is PLDM_ENTITY_PROC.
        // Skip for PERFORMANCE state sets (e.g. MemoryPerformance) which
        // get their own DIMM-level associations in the OEM phase.
        if ((std::get<1>(entityInfo) & 0x7FFF) ==
                PLDM_ENTITY_MEMORY_CONTROLLER &&
            !hasStateSetId(ptr->sensorInfo, PLDM_STATESET_ID_PERFORMANCE))
        {
            const auto& containerId = std::get<0>(entityInfo);
            auto containerItr = entityAssociations.find(containerId);
            if (containerItr != entityAssociations.end())
            {
                const auto& containerEntity = containerItr->second.first;
                if ((std::get<1>(containerEntity) & 0x7FFF) == PLDM_ENTITY_PROC)
                {
                    auto cpuPaths = findInventory(containerEntity, true);
                    if (!cpuPaths.empty())
                    {
                        inventoryPaths = cpuPaths;
                    }
                }
            }
        }

        ptr->setInventoryPaths(inventoryPaths, false);
        ptr->associateNumericSensor(numericSensors);

        auto sensorAuxiliaryNames = getSensorAuxiliaryNames(ptr->sensorId);
        if (sensorAuxiliaryNames)
        {
            auto& [id, count, auxNames] = *sensorAuxiliaryNames;
            ptr->updateSensorNames(auxNames);
        }
    }

    for (const auto& ptr : stateEffecters)
    {
        auto entityInfo = ptr->getEntityInfo();
        auto inventoryPath = findInventory(entityInfo);
        ptr->setInventoryPaths(inventoryPath);
    }

#ifdef OEM_NVIDIA
    co_await nvidia::nvidiaUpdateAssociations(*this);
#endif
    co_return PLDM_SUCCESS;
}
std::vector<std::string> Terminus::findInventory(const EntityInfo entityInfo,
                                                 const bool findClosest)
{
    // Search from stored result first
    auto itr = entities.find(entityInfo);
    if (itr != entities.end())
    {
        auto& entity = itr->second;
        if (findClosest)
        {
            return entity.getClosestInventories();
        }
        else
        {
            return entity.getInventories();
        }
    }

    const auto& [containerId, entityType, instance] = entityInfo;
    auto entityInstance = instance;
    // Strip LOGICAL flag for comparison - PDRs may use either raw or LOGICAL
    // types
    uint16_t rawEntityType = entityType & 0x7FFF;
    // Use terminus instance from static config for CPU and ProcessorModule
    // entities
    if ((rawEntityType == PLDM_ENTITY_PROC ||
         rawEntityType == PLDM_ENTITY_PROC_IO_MODULE) &&
        getInstance())
    {
        entityInstance = *getInstance();
    }
    auto ContainerInventoryPaths = findInventory(containerId);

    // Search for possible inventory paths
    std::vector<std::string> candidates;
    for (const auto& [candidatePath, candidateType, candidateInstance] :
         inventories)
    {
        // Strip LOGICAL flag from both for comparison - PDRs may use
        // LOGICAL|type while inventory uses raw type
        uint16_t rawCandidateType = candidateType & 0x7FFF;
        if ((rawEntityType == rawCandidateType) &&
            (entityInstance == candidateInstance))
        {
            candidates.push_back(candidatePath);
        }
    }

    std::vector<std::string> inventoryPaths;
    if (candidates.empty())
    {
        inventoryPaths.clear();
    }
    else if (rawEntityType == PLDM_ENTITY_PROC ||
             rawEntityType == PLDM_ENTITY_PROC_IO_MODULE)
    {
        // For CPU and ProcessorModule entities, return ALL matching candidates
        // These may have all_processors associations that link multiple paths
        inventoryPaths = candidates;
    }
    else
    {
        // For other entities, find the one under parent path
        for (const auto& candidate : candidates)
        {
            for (const auto& containerPath : ContainerInventoryPaths)
            {
                const auto& it = inventoryParentMap.find(candidate);
                if (it != inventoryParentMap.end() &&
                    it->second == containerPath)
                {
                    inventoryPaths.emplace_back(candidate);
                }
            }
        }
        // default path if no one under parent path
        if (inventoryPaths.empty())
        {
            inventoryPaths.emplace_back(candidates[0]);
        }
    }

    // Entity-type scope overrides for Redfish representation:
    //  MEMORY_CONTROLLER (0x8F): associate with HGX_ProcessorModule_x
    //  POWER_SUPPLY (0x78, VREGs): associate with HGX_CPU_x
    auto effectiveContainerPaths = ContainerInventoryPaths;
    if (inventoryPaths.empty())
    {
        auto containerItr = entityAssociations.find(containerId);
        if (containerItr != entityAssociations.end())
        {
            if (rawEntityType == PLDM_ENTITY_MEMORY_CONTROLLER)
            {
                // Walk up: DRAM is under CPU, but belongs at ProcessorModule
                const auto& containerEntity = containerItr->second.first;
                uint16_t rawContainerType =
                    std::get<1>(containerEntity) & 0x7FFF;
                if (rawContainerType != PLDM_ENTITY_PROC_IO_MODULE)
                {
                    const auto& parentContainerId =
                        std::get<0>(containerEntity);
                    auto modulePaths = findInventory(parentContainerId, true);
                    if (!modulePaths.empty())
                    {
                        effectiveContainerPaths = modulePaths;
                    }
                }
            }
            else if (rawEntityType == PLDM_ENTITY_POWER_SUPPLY)
            {
                // Sideways: VREG is under ProcessorModule, but powers the CPU.
                // Find the sibling CPU entity in the same container.
                const auto& siblings = containerItr->second.second;
                for (const auto& sibling : siblings)
                {
                    if ((std::get<1>(sibling) & 0x7FFF) == PLDM_ENTITY_PROC)
                    {
                        auto cpuPaths = findInventory(sibling, true);
                        if (!cpuPaths.empty())
                        {
                            effectiveContainerPaths = cpuPaths;
                            break;
                        }
                    }
                }
            }
        }
    }

    // Store the result, and also create parent_chassis/all_chassis
    // association
    entities.emplace(entityInfo,
                     Entity{inventoryPaths, effectiveContainerPaths});

    if (!inventoryPaths.empty() || !findClosest)
    {
        return inventoryPaths;
    }
    return effectiveContainerPaths;
}

std::vector<std::string> Terminus::findInventory(const ContainerID containerId,
                                                 const bool findClosest)
{
    std::vector<std::string> inventoryPath;
    inventoryPath.emplace_back(systemInventoryPath);
    if (containerId == overallSystemCotainerId)
    {
        return inventoryPath;
    }

    auto itr = entityAssociations.find(containerId);
    if (itr == entityAssociations.end())
    {
        lg2::error(
            "cannot find containerId:{CID}, returning systemInventoryPath",
            "CID", containerId);
        return inventoryPath;
    }
    const auto& [containerEntity, containedEntities] = itr->second;
    return findInventory(containerEntity, findClosest);
}

std::string Terminus::buildEntityTypeTag(uint16_t entityType) const
{
    static const std::map<uint16_t, std::string_view> entityTypeNameMap = {
        {32, "OS"},       {120, "Vreg"},   {135, "CPU"},
        {143, "MemCntl"}, {161, "SysBus"}, {166, "PCIeBus"},
    };

    uint16_t baseEntityType = entityType & 0x7FFF;
    auto it = entityTypeNameMap.find(baseEntityType);
    if (it == entityTypeNameMap.end())
    {
        return {};
    }

    if (cpuIndex.has_value())
    {
        return std::format("{}_{}", it->second, *cpuIndex);
    }
    return std::string(it->second);
}

std::string Terminus::buildSensorNamePrefix(uint16_t entityType) const
{
    auto entityTag = buildEntityTypeTag(entityType);
    if (entityTag.empty())
    {
        return terminusName;
    }
    return std::format("{}_{}", terminusName, entityTag);
}

void Terminus::addNumericSensor(
    const std::shared_ptr<pldm_numeric_sensor_value_pdr> pdr)
{
    auto sensorName = "PLDM_Sensor_" + std::to_string(pdr->sensor_id) + "_" +
                      std::to_string(tid);
    auto auxName = getAuxNameForNumericSensor(pdr->sensor_id);
    if (auxName)
    {
        sensorName = *auxName;
    }

    std::shared_ptr<utils::SensorEventInfo> sensorEventInfo = nullptr;
#ifdef OEM_NVIDIA
    sensorEventInfo = getSensorEventInfo(pdr->sensor_id);
#endif

    // Prepend terminus name (+ entity type + cpu index) if available and name
    // is not from Entity Manager. Names from EM are in
    // sensorAuxNameOverwriteTbl
    if (!terminusName.empty() &&
        sensorAuxNameOverwriteTbl.find(pdr->sensor_id) ==
            sensorAuxNameOverwriteTbl.end())
    {
        auto prefix = buildSensorNamePrefix(pdr->entity_type);
        // FixME: Backward compat — old firmware includes entity-type +
        // CPU index in aux name. Remove when all firmware sends plain
        // aux names.
        auto entityTag = buildEntityTypeTag(pdr->entity_type);
        if (!entityTag.empty() && sensorName.starts_with(entityTag + "_"))
        {
            sensorName = std::format("{}_{}", terminusName, sensorName);
        }
        else
        {
            sensorName = std::format("{}_{}", prefix, sensorName);
        }
    }

    try
    {
        auto sensor = std::make_shared<NumericSensor>(
            tid, true, pdr, sensorName, systemInventoryPath, sensorEventInfo);
        numericSensors.emplace_back(sensor);
        if (auxName)
        {
            // set default value to pdi and shm if aux name is available
            sensor->updateReading(true, false,
                                  std::numeric_limits<double>::quiet_NaN());
        }
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
    auto sensorName = "PLDM_Sensor_" + std::to_string(pdr->sensor_id) + "_" +
                      std::to_string(tid);
    auto auxName = getAuxNameForNumericSensor(pdr->sensor_id);
    if (auxName)
    {
        sensorName = *auxName;
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
    std::string effecterName =
        "PLDM_Effecter_" + std::to_string(pdr->effecter_id) + "_" +
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

    if (!terminusName.empty())
    {
        auto prefix = buildSensorNamePrefix(pdr->entity_type);
        // FixME: Backward compat — old firmware includes entity-type +
        // CPU index in aux name. Remove when all firmware sends plain
        // aux names.
        auto entityTag = buildEntityTypeTag(pdr->entity_type);
        if (!entityTag.empty() && effecterName.starts_with(entityTag + "_"))
        {
            effecterName = std::format("{}_{}", terminusName, effecterName);
        }
        else
        {
            effecterName = std::format("{}_{}", prefix, effecterName);
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

    uint16_t entityType = std::get<1>(std::get<0>(sensorInfo));

    auto sensorAuxiliaryNames = getSensorAuxiliaryNames(sId);
    AuxiliaryNames* sensorNames = nullptr;
    std::shared_ptr<AuxiliaryNames> prependedSensorNames;

    if (sensorAuxiliaryNames)
    {
        sensorNames = &(std::get<2>(*sensorAuxiliaryNames));

        // Prepend terminus name (+ entity type + cpu index) to auxiliary names
        // if available and not from Entity Manager
        if (!terminusName.empty() && sensorAuxNameOverwriteTbl.find(sId) ==
                                         sensorAuxNameOverwriteTbl.end())
        {
            auto prefix = buildSensorNamePrefix(entityType);
            // FixME: Backward compat — old firmware includes
            // entity-type + CPU index in aux name. Remove when all
            // firmware sends plain aux names.
            auto entityTag = buildEntityTypeTag(entityType);
            prependedSensorNames = std::make_shared<AuxiliaryNames>();
            for (const auto& compositeSensorNames : *sensorNames)
            {
                std::vector<std::pair<NameLanguageTag, SensorName>>
                    prependedCompositeNames;
                prependedCompositeNames.reserve(compositeSensorNames.size());
                for (const auto& [tag, name] : compositeSensorNames)
                {
                    std::string newName;
                    if (!entityTag.empty() && name.starts_with(entityTag + "_"))
                    {
                        newName = std::format("{}_{}", terminusName, name);
                    }
                    else
                    {
                        newName = std::format("{}_{}", prefix, name);
                    }
                    prependedCompositeNames.emplace_back(tag,
                                                         std::move(newName));
                }
                prependedSensorNames->emplace_back(
                    std::move(prependedCompositeNames));
            }
            sensorNames = prependedSensorNames.get();
        }
    }

    std::shared_ptr<utils::SensorEventInfo> stateSensorEventInfo = nullptr;
#ifdef OEM_NVIDIA
    stateSensorEventInfo = getSensorEventInfo(sId);
#endif

    try
    {
        auto sensor = std::make_shared<StateSensor>(
            tid, true, sId, std::move(sensorInfo), sensorNames,
            systemInventoryPath, stateSensorEventInfo);
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

    uint16_t entityType = std::get<1>(std::get<0>(effecterInfo));

    auto effecterAuxiliaryNames = getEffecterAuxiliaryNames(eId);
    AuxiliaryNames* effecterNames = nullptr;
    std::shared_ptr<AuxiliaryNames> prependedEffecterNames;

    if (effecterAuxiliaryNames)
    {
        effecterNames = &(std::get<2>(*effecterAuxiliaryNames));

        if (!terminusName.empty())
        {
            auto prefix = buildSensorNamePrefix(entityType);
            // FixME: Backward compat — old firmware includes
            // entity-type + CPU index in aux name. Remove when all
            // firmware sends plain aux names.
            auto entityTag = buildEntityTypeTag(entityType);
            prependedEffecterNames = std::make_shared<AuxiliaryNames>();
            for (const auto& compositeEffecterNames : *effecterNames)
            {
                std::vector<std::pair<NameLanguageTag, SensorName>>
                    prependedCompositeNames;
                prependedCompositeNames.reserve(compositeEffecterNames.size());
                for (const auto& [tag, name] : compositeEffecterNames)
                {
                    std::string newName;
                    if (!entityTag.empty() && name.starts_with(entityTag + "_"))
                    {
                        newName = std::format("{}_{}", terminusName, name);
                    }
                    else
                    {
                        newName = std::format("{}_{}", prefix, name);
                    }
                    prependedCompositeNames.emplace_back(tag,
                                                         std::move(newName));
                }
                prependedEffecterNames->emplace_back(
                    std::move(prependedCompositeNames));
            }
            effecterNames = prependedEffecterNames.get();
        }
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
        lg2::error("Failed to create StateEffecter:{EFFECTERNAME}, {ERROR}.",
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
        case PLDM_ENTITY_PROC_MODULE:
        case PLDM_ENTITY_PROC_IO_MODULE:
            return PhysicalContextType::CPU;
        case PLDM_ENTITY_DC_DC_CONVERTER:
        case PLDM_ENTITY_POWER_CONVERTER:
        case PLDM_ENTITY_POWER_SUPPLY:
            return PhysicalContextType::VoltageRegulator;
        case PLDM_ENTITY_NETWORK_CONTROLLER:
            return PhysicalContextType::NetworkingDevice;
        case PLDM_ENTITY_SYS_BOARD:
        default:
            break;
    }
    return PhysicalContextType::SystemBoard;
}

void Terminus::setOnline()
{
    // Restore availability for effecters that were marked unavailable by
    // setOffline(). Sensors already have available=true after setOffline().
    // The actual functional state and values will be restored by sensor polling
    // when it reads fresh data from the terminus.
    for (auto& numericEffecter : numericEffecters)
    {
        numericEffecter->setAvailable(true);
        numericEffecter->needUpdate = true;
    }

    for (auto& stateEffecter : stateEffecters)
    {
        stateEffecter->setAvailable(true);
        stateEffecter->needUpdate = true;
    }
}

void Terminus::setOffline()
{
    resumed = false;
    for (auto numericSensor : numericSensors)
    {
        numericSensor->handleErrGetSensorReading();
    }

    for (auto numericEffecter : numericEffecters)
    {
        numericEffecter->handleErrGetNumericEffecterValue();
    }

    for (auto stateSensor : stateSensors)
    {
        stateSensor->handleErrGetSensorReading();
    }

    for (auto stateEffecter : stateEffecters)
    {
        stateEffecter->handleErrGetStateEffecterStates();
    }
}

std::optional<std::string> Terminus::getAuxNameForNumericSensor(SensorID id)
{
    auto sensorAuxiliaryNames = getSensorAuxiliaryNames(id);
    if (sensorAuxiliaryNames)
    {
        const auto& [sensorId, sensorCnt, sensorNames] = *sensorAuxiliaryNames;
        if (sensorCnt == 1 && sensorNames.size() > 0)
        {
            for (const auto& [languageTag, name] : sensorNames[0])
            {
                if (languageTag == "en")
                {
                    return name;
                }
            }
        }
    }
    return std::nullopt;
}

void Terminus::refreshAssociations()
{
    if (refreshAssociationsTaskHandle.has_value())
    {
        auto& [scope, rcOpt] = *refreshAssociationsTaskHandle;
        if (!rcOpt.has_value())
        {
            return;
        }
        stdexec::sync_wait(scope.on_empty());
        refreshAssociationsTaskHandle.reset();
    }
    auto& [scope, rcOpt] = refreshAssociationsTaskHandle.emplace();
    stdexec::start_detached(
        refreshAssociationsTask() |
            stdexec::then([&](int rc) { rcOpt.emplace(rc); }),
        exec::default_task_context<void>(exec::inline_scheduler{}));
}

exec::task<int> Terminus::refreshAssociationsTask()
{
    while (needRefresh)
    {
        needRefresh = false;
        // Update inventory list
        co_await scanInventories();
    }
    // Update Sensor PDIs after scans are done
    co_await updateAssociations();
    co_return PLDM_SUCCESS;
}

} // namespace platform_mc
} // namespace pldm
