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
#include <exec/start_detached.hpp>

#include <cstddef>
#include <filesystem>
#include <format>

#ifdef OEM_NVIDIA
#include "oem/nvidia/platform-mc/oem_nvidia.hpp"
#endif

#include "common/dBusAsyncUtils.hpp"
#include "common/shared_mem_manager.hpp"
#include "common/utils.hpp"
#include "terminus.hpp"
#include "terminus_manager.hpp"

#include <cstring>
#include <stdexcept>

namespace pldm
{
namespace platform_mc
{

namespace
{

/** @brief The platform's system inventory, the default parent of every
 *         sensor and effecter. Non-const only because the sensor and
 *         effecter constructors take a std::string&. */
std::string& systemInventoryPath()
{
#ifdef OEM_NVIDIA
    return oem_nvidia::InventoryManager::get().systemInventoryPath;
#else
    static std::string path = PLATFORM_CHASSIS_PATH;
    return path;
#endif
}

} // namespace

Terminus::Terminus(tid_t tid, uint64_t supportedTypes, UUID& uuid,
                   TerminusManager& terminusManager) :
    initialized(false), pollEvent(false), pollDataTransferHandle(0),
    ready(false), synchronyConfigurationSupported(0), resumed(true),
    initSensorList(true), tid(tid), supportedTypes(supportedTypes), uuid(uuid),
    terminusManager(terminusManager)
{
    maxBufferSize = EVENT_MESSAGE_BUFFER_SIZE_MAX;
    needRefresh = false;
#ifdef OEM_NVIDIA
    // A terminus re-created at a reused TID starts with no entity topology,
    // instance or name; TerminusManager registers its config next.
    oem_nvidia::InventoryManager::get().forget(tid);
#endif
}

void Terminus::interfaceAdded(sdbusplus::message::message& m)
{
    sdbusplus::object_path objPath;
    pldm::dbus::InterfaceMap interfaces;
    m.read(objPath, interfaces);

    // if any interested interface added, refresh the associations
    bool interested = false;
    for (const auto& [intf, properties] : interfaces)
    {
#ifdef OEM_NVIDIA
        for (const auto& [entitytype, entityIface] :
             oem_nvidia::entityInterfaces)
        {
            if (intf == entityIface)
            {
                interested = true;
                break;
            }
        }
        if (intf == oem_nvidia::overallSystemInterface)
        {
            interested = true;
        }
        if (intf == "xyz.openbmc_project.Configuration.NsmDeviceAssociation")
        {
            interested = true;
        }
#endif
        // Device-association configs bind an inventory object to a terminus
        // (checkDeviceInventory); one arriving after its box was scanned
        // must trigger a re-scan just like the NSM variant above.
        if (intf == "xyz.openbmc_project.Configuration.I2CDeviceAssociation" ||
            intf == "xyz.openbmc_project.Configuration.USBDeviceAssociation" ||
            intf ==
                "xyz.openbmc_project.Configuration.MctpEndpointAssociation" ||
            intf == pldm::em_device::pldmDeviceIntf ||
            intf == pldm::em_device::nvDeviceIntf)
        {
            interested = true;
        }
        if (interested)
        {
            break;
        }
    }

    if (!interested)
    {
        return;
    }
    needRefresh = true;

    if (!initialized)
    {
        // Inventory can be published while the terminus is still
        // initializing (init spans many co_awaits, and the match is
        // registered before scanInventories snapshots the subtree).
        // needRefresh is already latched above, so this return only defers
        // the refresh — applyPendingRefresh() serves it right after init
        // completes; the signal is not dropped. This keeps the original
        // init-time deferral for NsmDeviceAssociation (the race fix that
        // latched needRefresh during init) and extends it to every
        // interested interface: a dropped signal leaves fallback-gated
        // sensors without metric-report updates until an unrelated future
        // inventory event.
        lg2::info("Tid={TID} interested inventory interface arrived on "
                  "{PATH} during init; refresh deferred until Terminus is "
                  "initialized",
                  "TID", tid, "PATH", std::string(objPath));
        return;
    }

    refreshAssociations();
}

bool Terminus::checkNsmDeviceInventory(UUID nsmUuid)
{
    if (nsmUuid.substr(0, 36) == uuid.substr(0, 36))
    {
        return true;
    }
    else
    {
        lg2::debug(
            "checkNsmDeviceInventory: NSM UUID {NSMUUID} does not map to terminus MCTP UUID {UUID} (TID {TID})",
            "NSMUUID", nsmUuid, "UUID", uuid, "TID", tid);
        return false;
    }
}

exec::task<int> Terminus::checkDeviceInventory(const std::string& objPath)
{
    // A device described by the platform topology: the chassis object named
    // after an NvDevice whose EID is this terminus, with the sensor names and
    // ports in the device's PLDMDevice record.
    if (objPath.starts_with(pldm::em_device::chassisRoot))
    {
        const std::string deviceName =
            objPath.substr(std::string(pldm::em_device::chassisRoot).size());
        const std::optional<MctpInfo> mctpInfo =
            terminusManager.toMctpInfo(tid);
        auto dev = deviceName.find('/') == std::string::npos
                       ? pldm::em_device::deviceByName(deviceName)
                       : std::nullopt;
        if (mctpInfo && dev && dev->eid && *dev->eid == std::get<0>(*mctpInfo))
        {
            if (auto record = pldm::em_device::pldmDevice(deviceName))
            {
                // Sensor records that still carry their own config (e.g.
                // ThermalParameters) are read first; the record adds to them.
                co_await getSensorAuxNameFromEM(
                    0, 0, static_cast<uint8_t>(*dev->eid), objPath);
#ifdef OEM_NVIDIA
                co_await getPortInfoFromEM(objPath);
#endif
                loadDeviceRecord(*record, objPath);
#ifdef OEM_NVIDIA
                co_await getInfoForNVSwitchFromEM(objPath);
                co_await getSensorEventInfoFromEM(objPath);
#endif
                co_return PLDM_SUCCESS;
            }
        }
    }

    try
    {
        auto getSubTreeResponse = co_await utils::coGetSubTree(
            objPath, 0,
            {"xyz.openbmc_project.Configuration.I2CDeviceAssociation",
             "xyz.openbmc_project.Configuration.USBDeviceAssociation",
             "xyz.openbmc_project.Configuration.MctpEndpointAssociation",
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
                        "xyz.openbmc_project.Configuration.MctpEndpointAssociation")
                    {
                        inventoryEid = co_await utils::coGetDbusProperty<
                            uint64_t>(
                            objectPath.c_str(), "EID",
                            "xyz.openbmc_project.Configuration.MctpEndpointAssociation",
                            serviceName);
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
                        lg2::debug(
                            "checkDeviceInventory: matched device inventory for TID {TID} at {PATH} (eid={EID} bus={BUS} addr={ADDR}); loading EM config",
                            "TID", tid, "PATH", objPath, "EID", inventoryEid,
                            "BUS", bus, "ADDR", addr);
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

void Terminus::loadDeviceRecord(const pldm::em_device::PldmDevice& record,
                                const std::string& objPath)
{
    for (const auto& [sensorId, name] : record.auxNames)
    {
        AuxiliaryNames auxNameTbl;
        auxNameTbl.push_back({{"en", name}});
        sensorAuxNameOverwriteTbl[sensorId] =
            std::make_tuple(auxNameTbl, objPath);
    }
#ifdef OEM_NVIDIA
    for (const auto& [sensorId, port] : record.ports)
    {
        std::vector<dbus::PathAssociation> associations;
        associations.push_back({});
        auto& assoc = associations.back();
        assoc.forward = "associated_port";
        assoc.reverse = "associated_port";
        assoc.path = objPath + "/NetworkAdapters/" + record.networkAdapter +
                     "/Ports/" + port;
        sensorPortInfoOverwriteTbl[sensorId] = std::make_tuple(
            PortInfoIntf::convertPortTypeFromString(
                "xyz.openbmc_project.Inventory.Decorator.PortInfo.PortType."
                "BidirectionalPort"),
            std::string{}, record.portMaxSpeed.value_or(0), associations);
    }
#endif
    lg2::debug("loadDeviceRecord: {AUX} aux names, {PORTS} ports for {PATH}",
               "AUX", record.auxNames.size(), "PORTS", record.ports.size(),
               "PATH", objPath);
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
            lg2::error(
                "getSensorAuxNameFromEM: unable to load SensorAuxName config from EM under {PATH} (eid={EID} bus={BUS} addr={ADDR}); sensor names fall back to PLDM defaults",
                "PATH", objPath, "EID", eid, "BUS", bus, "ADDR", addr);
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
            lg2::debug(
                "getSensorAuxNameFromEM: loaded {COUNT} aux name(s) for sensorId {SID} from EM {PATH} (eid={EID})",
                "COUNT", auxNameTbl.size(), "SID", sensorId, "PATH", path,
                "EID", eid);
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

                std::string loggingNamespace;
                try
                {
                    loggingNamespace = co_await pldm::utils::coGetDbusProperty<
                        std::string>(
                        path.c_str(), "LoggingNamespace",
                        "xyz.openbmc_project.Configuration.SensorEventInfo");
                }
                catch (const std::exception&)
                {
                    loggingNamespace.clear();
                }

                if (!loggingNamespace.empty() &&
                    !pldm::utils::isValidLoggingNamespace(loggingNamespace))
                {
                    throw std::invalid_argument(
                        "Invalid LoggingNamespace '" + loggingNamespace +
                        "'; use only [A-Za-z0-9_]");
                }

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
                        impactedComponent, std::move(eventIdsMap),
                        std::move(loggingNamespace));
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

uint64_t Terminus::updateSupportedTypes(uint64_t types)
{
    const std::bitset<64> updatedTypes(types);
    const auto changedTypes = supportedTypes ^ updatedTypes;
    supportedTypes = updatedTypes;
    return changedTypes.to_ullong();
}

bool Terminus::hasEntities() const
{
    return !numericSensors.empty() || !stateSensors.empty() ||
           !numericEffecters.empty() || !stateEffecters.empty();
}

bool Terminus::isPdrAlreadyApplied(const std::vector<uint8_t>& pdr)
{
    if (pdr.size() < sizeof(pldm_pdr_hdr))
    {
        return false;
    }
    switch (reinterpret_cast<const pldm_pdr_hdr*>(pdr.data())->type)
    {
        case PLDM_NUMERIC_SENSOR_PDR:
        {
            auto parsed = parseNumericSensorPDR(pdr);
            if (!parsed)
            {
                return false;
            }
            uint16_t id = parsed->sensor_id;
            for (const auto& s : numericSensors)
            {
                if (s && s->sensorId == id)
                {
                    return true;
                }
            }
            return false;
        }
        case PLDM_STATE_SENSOR_PDR:
        {
            std::vector<uint8_t> copy = pdr;
            auto parsed = parseStateSensorPDR(copy);
            if (!parsed)
            {
                return false;
            }
            uint16_t id = std::get<0>(*parsed);
            for (const auto& s : stateSensors)
            {
                if (s && s->sensorId == id)
                {
                    return true;
                }
            }
            return false;
        }
        case PLDM_NUMERIC_EFFECTER_PDR:
        {
            auto parsed = parseNumericEffecterPDR(pdr);
            if (!parsed)
            {
                return false;
            }
            uint16_t id = parsed->effecter_id;
            for (const auto& e : numericEffecters)
            {
                if (e && e->effecterId == id)
                {
                    return true;
                }
            }
            return false;
        }
        case PLDM_STATE_EFFECTER_PDR:
        {
            std::vector<uint8_t> copy = pdr;
            auto parsed = parseStateEffecterPDR(copy);
            if (!parsed)
            {
                return false;
            }
            uint16_t id = std::get<0>(*parsed);
            for (const auto& e : stateEffecters)
            {
                if (e && e->effecterId == id)
                {
                    return true;
                }
            }
            return false;
        }
        case PLDM_SENSOR_AUXILIARY_NAMES_PDR:
        {
            auto aux = parseSensorAuxiliaryNamesPDR(pdr);
            if (!aux)
            {
                return false;
            }
            for (const auto& a : sensorAuxiliaryNamesTbl)
            {
                if (a && std::get<0>(*a) == std::get<0>(*aux))
                {
                    return true;
                }
            }
            return false;
        }
        case PLDM_EFFECTER_AUXILIARY_NAMES_PDR:
        {
            auto aux = parseEffecterAuxiliaryNamesPDR(pdr);
            if (!aux)
            {
                return false;
            }
            for (const auto& a : effecterAuxiliaryNamesTbl)
            {
                if (a && std::get<0>(*a) == std::get<0>(*aux))
                {
                    return true;
                }
            }
            return false;
        }
#ifdef OEM_NVIDIA
        case PLDM_OEM_PDR:
        {
            // Only the energy-count sub-type owns a NumericSensor (in
            // numericSensors); other OEM PDRs are let through.
            auto parsedPdr = parseOemPDR(pdr);
            const auto& oemData = std::get<2>(parsedPdr);
            if (oemData.size() <= 2 ||
                static_cast<nvidia::NvidiaOemPdrType>(oemData[2]) !=
                    nvidia::NvidiaOemPdrType::
                        NVIDIA_OEM_PDR_TYPE_SENSOR_ENERGYCOUNT)
            {
                return false;
            }
            auto parsed = nvidia::parseOEMEnergyCountNumericSensorPDR(oemData);
            if (!parsed)
            {
                return false;
            }
            uint16_t id = parsed->sensor_id;
            for (const auto& s : numericSensors)
            {
                if (s && s->sensorId == id)
                {
                    return true;
                }
            }
            return false;
        }
#endif
        default:
            return false;
    }
}

bool Terminus::pdrTypeConsumed(uint8_t type)
{
    // Keep in sync with parseOnePdrIntoCache().
    switch (type)
    {
        case PLDM_SENSOR_AUXILIARY_NAMES_PDR:
        case PLDM_EFFECTER_AUXILIARY_NAMES_PDR:
        case PLDM_NUMERIC_SENSOR_PDR:
        case PLDM_NUMERIC_EFFECTER_PDR:
        case PLDM_STATE_SENSOR_PDR:
        case PLDM_PDR_ENTITY_ASSOCIATION:
        case PLDM_STATE_EFFECTER_PDR:
        case PLDM_OEM_PDR:
            return true;
        default:
            return false;
    }
}

bool Terminus::parseOnePdrIntoCache(std::vector<uint8_t>& pdr)
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
        if (parsedPdr)
        {
            stateSensorPdrs.emplace_back(std::move(*parsedPdr));
        }
    }
    else if (pdrHdr->type == PLDM_PDR_ENTITY_ASSOCIATION)
    {
        parseEntityAssociationPDR(pdr);
    }
    else if (pdrHdr->type == PLDM_STATE_EFFECTER_PDR)
    {
        auto parsedPdr = parseStateEffecterPDR(pdr);
        if (parsedPdr)
        {
            stateEffecterPdrs.emplace_back(std::move(*parsedPdr));
        }
    }
    else if (pdrHdr->type == PLDM_OEM_PDR)
    {
        auto parsedPdr = parseOemPDR(pdr);
#ifdef OEM_NVIDIA
        if (std::get<2>(parsedPdr).size() > 2 &&
            static_cast<nvidia::NvidiaOemPdrType>(std::get<2>(parsedPdr)[2]) ==
                nvidia::NvidiaOemPdrType::
                    NVIDIA_OEM_PDR_TYPE_SENSOR_ENERGYCOUNT)
        {
            std::shared_ptr<pldm_oem_energycount_numeric_sensor_value_pdr>
                parsedOEMPdr = nvidia::parseOEMEnergyCountNumericSensorPDR(
                    std::get<2>(parsedPdr));
            if (!parsedOEMPdr)
            {
                const std::optional<MctpInfo> mctpInfo =
                    terminusManager.toMctpInfo(tid);
                const std::string eid =
                    mctpInfo ? std::to_string(std::get<0>(*mctpInfo))
                             : "unknown";
                const uint32_t recordHandle = le32toh(pdrHdr->record_handle);
                lg2::error(
                    "Skipping malformed OEM energy-count PDR: TID={TID}, EID={EID}, record handle={RECORDHANDLE}",
                    "TID", tid, "EID", eid, "RECORDHANDLE", recordHandle);
                return true;
            }
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
        return false;
    }
    return true;
}

size_t Terminus::addNewPdrs(const std::vector<std::vector<uint8_t>>& newPdrs)
{
    // Snapshot cache sizes so pass 2 creates D-Bus objects ONLY for the newly
    // parsed PDRs — never re-adding objects for PDRs already present.
    size_t numStart = numericSensorPdrs.size();
    size_t stateStart = stateSensorPdrs.size();
    size_t numEffStart = numericEffecterPdrs.size();
    size_t stateEffStart = stateEffecterPdrs.size();
#ifdef OEM_NVIDIA
    size_t oemEnergyStart = oemEnergyCountNumericSensorPdrs.size();
#endif

    // Pass 1: retain each raw PDR and parse it into the typed caches. Auxiliary
    // name PDRs in the batch are parsed here so the sensor adds below see them.
    size_t skipped = 0;
    for (const auto& pdr : newPdrs)
    {
        // A terminus that rebuilt its repository re-reports existing sensors as
        // ADDED (new record handle, same sensor id). Recreating them would
        // collide on D-Bus (FileExists) and duplicate cache entries, so skip
        // any PDR whose derived object/entry is already present.
        if (isPdrAlreadyApplied(pdr))
        {
            ++skipped;
            continue;
        }
        pdrs.emplace_back(pdr);
        if (!parseOnePdrIntoCache(pdrs.back()))
        {
            lg2::error(
                "addNewPdrs: unrecognized PDR type in repository change event, skipping one record");
        }
    }

    // Pass 2: create D-Bus objects for the newly appended cache entries only.
    for (size_t i = numStart; i < numericSensorPdrs.size(); ++i)
    {
        addNumericSensor(numericSensorPdrs[i]);
    }
#ifdef OEM_NVIDIA
    for (size_t i = oemEnergyStart; i < oemEnergyCountNumericSensorPdrs.size();
         ++i)
    {
        addOEMEnergyCountNumericSensor(oemEnergyCountNumericSensorPdrs[i]);
    }
#endif
    for (size_t i = numEffStart; i < numericEffecterPdrs.size(); ++i)
    {
        addNumericEffecter(numericEffecterPdrs[i]);
    }
    for (size_t i = stateStart; i < stateSensorPdrs.size(); ++i)
    {
        auto [sensorID, stateSetSensorInfo] = stateSensorPdrs[i];
        addStateSensor(sensorID, std::move(stateSetSensorInfo));
    }
    for (size_t i = stateEffStart; i < stateEffecterPdrs.size(); ++i)
    {
        auto [effecterId, stateSetEffecterInfo] = stateEffecterPdrs[i];
        addStateEffecter(effecterId, std::move(stateSetEffecterInfo));
    }

    // A repository change means this terminus restarted its device-side state
    // sequence. Effecters re-reported as already-present are skipped above and
    // keep whatever needsUpdate they settled on, so a tracked effecter that
    // disarmed on a terminal state would never be read again. Re-arm them here,
    // ahead of the caller's applied-count early return.
    auto rearmed = rearmTrackedEffecters();

    lg2::info(
        "addNewPdrs: applied {N} new PDR(s), skipped {SK} already-present; +{NS} numeric sensor(s), +{SS} state sensor(s), re-armed {RE} tracked effecter(s)",
        "N", newPdrs.size() - skipped, "SK", skipped, "NS",
        numericSensorPdrs.size() - numStart, "SS",
        stateSensorPdrs.size() - stateStart, "RE", rearmed);

    return newPdrs.size() - skipped;
}

void Terminus::removeModifiedPdrObjects(
    const std::vector<std::vector<uint8_t>>& modifiedPdrs)
{
    size_t removed = 0;
    for (const auto& raw : modifiedPdrs)
    {
        if (raw.size() < sizeof(pldm_pdr_hdr))
        {
            continue;
        }
        auto hdr = reinterpret_cast<const pldm_pdr_hdr*>(raw.data());
        uint32_t recordHandle = le32toh(hdr->record_handle);
        uint8_t type = hdr->type;

        if (type == PLDM_NUMERIC_SENSOR_PDR)
        {
            auto parsed = parseNumericSensorPDR(raw);
            if (!parsed)
            {
                continue;
            }
            uint16_t id = parsed->sensor_id;
            std::erase_if(numericSensors, [id](const auto& s) {
                return s && s->sensorId == id;
            });
            std::erase_if(numericSensorPdrs, [id](const auto& p) {
                return p && p->sensor_id == id;
            });
        }
        else if (type == PLDM_STATE_SENSOR_PDR)
        {
            std::vector<uint8_t> copy = raw;
            auto parsed = parseStateSensorPDR(copy);
            if (!parsed)
            {
                continue;
            }
            uint16_t id = std::get<0>(*parsed);
            std::erase_if(stateSensors, [id](const auto& s) {
                return s && s->sensorId == id;
            });
            std::erase_if(stateSensorPdrs,
                          [id](const auto& t) { return std::get<0>(t) == id; });
        }
        else if (type == PLDM_NUMERIC_EFFECTER_PDR)
        {
            auto parsed = parseNumericEffecterPDR(raw);
            if (!parsed)
            {
                continue;
            }
            uint16_t id = parsed->effecter_id;
            std::erase_if(numericEffecters, [id](const auto& e) {
                return e && e->effecterId == id;
            });
            std::erase_if(numericEffecterPdrs, [id](const auto& p) {
                return p && p->effecter_id == id;
            });
        }
        else if (type == PLDM_STATE_EFFECTER_PDR)
        {
            std::vector<uint8_t> copy = raw;
            auto parsed = parseStateEffecterPDR(copy);
            if (!parsed)
            {
                continue;
            }
            uint16_t id = std::get<0>(*parsed);
            std::erase_if(stateEffecters, [id](const auto& e) {
                return e && e->effecterId == id;
            });
            std::erase_if(stateEffecterPdrs,
                          [id](const auto& t) { return std::get<0>(t) == id; });
        }
#ifdef OEM_NVIDIA
        else if (type == PLDM_OEM_PDR)
        {
            // Only the OEM energy-count sub-type maps to a single NumericSensor
            // (kept in numericSensors, cached in
            // oemEnergyCountNumericSensorPdrs).
            auto parsedPdr = parseOemPDR(raw);
            const auto& oemData = std::get<2>(parsedPdr);
            if (oemData.size() <= 2 ||
                static_cast<nvidia::NvidiaOemPdrType>(oemData[2]) !=
                    nvidia::NvidiaOemPdrType::
                        NVIDIA_OEM_PDR_TYPE_SENSOR_ENERGYCOUNT)
            {
                continue;
            }
            auto parsed = nvidia::parseOEMEnergyCountNumericSensorPDR(oemData);
            if (!parsed)
            {
                continue;
            }
            uint16_t id = parsed->sensor_id;
            std::erase_if(numericSensors, [id](const auto& s) {
                return s && s->sensorId == id;
            });
            std::erase_if(oemEnergyCountNumericSensorPdrs, [id](const auto& p) {
                return p && p->sensor_id == id;
            });
        }
#endif
        else
        {
            // PDR type with no 1:1 object mapping: the caller routes these to a
            // full rebuild, so reaching here means nothing to remove.
            continue;
        }

        // Drop the superseded raw PDR (matched by record handle) so the
        // repository does not accumulate stale copies across repeated modifies.
        std::erase_if(pdrs, [recordHandle](const std::vector<uint8_t>& p) {
            if (p.size() < sizeof(pldm_pdr_hdr))
            {
                return false;
            }
            auto h = reinterpret_cast<const pldm_pdr_hdr*>(p.data());
            return le32toh(h->record_handle) == recordHandle;
        });
        ++removed;
    }

    lg2::info(
        "removeModifiedPdrObjects: removed objects for {N} modified PDR(s)",
        "N", removed);
}

bool Terminus::canReplacePdrIncrementally(const std::vector<uint8_t>& pdr)
{
    if (pdr.size() < sizeof(pldm_pdr_hdr))
    {
        return false;
    }
    switch (reinterpret_cast<const pldm_pdr_hdr*>(pdr.data())->type)
    {
        case PLDM_NUMERIC_SENSOR_PDR:
        case PLDM_STATE_SENSOR_PDR:
        case PLDM_NUMERIC_EFFECTER_PDR:
        case PLDM_STATE_EFFECTER_PDR:
            return true;
#ifdef OEM_NVIDIA
        case PLDM_OEM_PDR:
        {
            // Only the OEM energy-count sub-type maps 1:1 to a sensor object.
            auto parsedPdr = parseOemPDR(pdr);
            const auto& oemData = std::get<2>(parsedPdr);
            return oemData.size() > 2 &&
                   static_cast<nvidia::NvidiaOemPdrType>(oemData[2]) ==
                       nvidia::NvidiaOemPdrType::
                           NVIDIA_OEM_PDR_TYPE_SENSOR_ENERGYCOUNT;
        }
#endif
        default:
            return false;
    }
}

int Terminus::oemPdrSubType(const std::vector<uint8_t>& pdr)
{
    if (pdr.size() < sizeof(pldm_pdr_hdr) ||
        reinterpret_cast<const pldm_pdr_hdr*>(pdr.data())->type != PLDM_OEM_PDR)
    {
        return -1;
    }
    auto parsedPdr = parseOemPDR(pdr);
    const auto& oemData = std::get<2>(parsedPdr);
    // oemData = nvidia_oem_pdr: [0..1] terminus_handle, [2] oem_pdr_type.
    if (oemData.size() <= 2)
    {
        return -1;
    }
    return oemData[2];
}

void Terminus::clearParsedPdrCaches()
{
    numericSensorPdrs.clear();
    stateSensorPdrs.clear();
    numericEffecterPdrs.clear();
    stateEffecterPdrs.clear();
    sensorAuxiliaryNamesTbl.clear();
    effecterAuxiliaryNamesTbl.clear();
    oemPdrs.clear();
#ifdef OEM_NVIDIA
    oem_nvidia::InventoryManager::get().unbindAll(tid);
    oemEnergyCountNumericSensorPdrs.clear();
#endif
}

bool Terminus::parsePDRs()
{
    bool rc = true;
    for (auto& pdr : pdrs)
    {
        if (!parseOnePdrIntoCache(pdr))
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

bool Terminus::parseAuxiliaryNameStrings(
    const uint8_t* ptr, const uint8_t* const end, uint8_t count, uint16_t id,
    std::vector<std::vector<std::pair<NameLanguageTag, SensorName>>>& out,
    const char* label)
{
    constexpr uint8_t NullTerminator = 0;
    try
    {
        for (uint8_t i = 0; i < count; i++)
        {
            // Device-controlled count: stop before dereferencing ptr.
            if (ptr >= end)
            {
                lg2::error("{LBL} aux names PDR: count overruns buffer, "
                           "id={ID}, tid={TID}.",
                           "LBL", label, "ID", id, "TID", tid);
                return false;
            }
            const uint8_t nameStringCount = *ptr++;
            std::vector<std::pair<NameLanguageTag, SensorName>> nameStrings{};
            for (uint8_t j = 0; j < nameStringCount; j++)
            {
                // Language tag: bounded NUL scan (std::string would strlen()
                // past end); reject if unterminated or over the spec max.
                auto* nul = static_cast<const uint8_t*>(
                    std::memchr(ptr, 0, static_cast<size_t>(end - ptr)));
                if (nul == nullptr ||
                    static_cast<size_t>(nul - ptr) > PLDM_STR_UTF_8_MAX_LEN)
                {
                    lg2::error("{LBL} aux names PDR: bad language tag, "
                               "id={ID}, tid={TID}.",
                               "LBL", label, "ID", id, "TID", tid);
                    return false;
                }
                std::string nameLanguageTag(reinterpret_cast<const char*>(ptr),
                                            nul - ptr);
                ptr += nameLanguageTag.size() + sizeof(NullTerminator);

                // UTF-16 name: bounded bytewise scan (ptr is not
                // char16_t-aligned); reject if unterminated or over the spec
                // max, then copy into an aligned string.
                const size_t u16Units =
                    static_cast<size_t>(end - ptr) / sizeof(char16_t);
                size_t u16Len = 0;
                while (u16Len < u16Units &&
                       (ptr[2 * u16Len] | ptr[2 * u16Len + 1]))
                {
                    ++u16Len;
                }
                if (u16Len == u16Units || u16Len > PLDM_STR_UTF_16_MAX_LEN)
                {
                    lg2::error("{LBL} aux names PDR: bad UTF-16 name, "
                               "id={ID}, tid={TID}.",
                               "LBL", label, "ID", id, "TID", tid);
                    return false;
                }
                std::u16string u16NameString(u16Len, u'\0');
                std::memcpy(u16NameString.data(), ptr,
                            u16Len * sizeof(char16_t));
                ptr += (u16Len + 1) * sizeof(char16_t);
                std::transform(u16NameString.cbegin(), u16NameString.cend(),
                               u16NameString.begin(),
                               [](uint16_t utf16) { return be16toh(utf16); });
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
                std::string nameString{};
                try
                {
                    nameString =
                        std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,
                                             char16_t>{}
                            .to_bytes(u16NameString);
                }
                catch (const std::range_error& e)
                {
                    lg2::error(
                        "{LBL} aux names PDR: UTF-16 to UTF-8 conversion "
                        "failed, skipping name. id={ID}, tid={TID}, "
                        "error={ERROR}",
                        "LBL", label, "ID", id, "TID", tid, "ERROR", e.what());
                    continue;
                }
#pragma GCC diagnostic pop
                nameStrings.emplace_back(nameLanguageTag, nameString);
            }
            out.emplace_back(nameStrings);
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to parse {LBL} aux names PDR: {ERROR}, id={ID}, "
                   "tid={TID}.",
                   "LBL", label, "ERROR", e, "ID", id, "TID", tid);
        return false;
    }
    return true;
}

std::shared_ptr<SensorAuxiliaryNames> Terminus::parseSensorAuxiliaryNamesPDR(
    const std::vector<uint8_t>& pdrData)
{
    auto pdr = reinterpret_cast<const struct pldm_sensor_auxiliary_names_pdr*>(
        pdrData.data());
    // Reject a buffer too small for the fixed header (names[] excluded).
    const size_t minPdrLen =
        sizeof(struct pldm_sensor_auxiliary_names_pdr) - sizeof(pdr->names);
    if (pdrData.size() < minPdrLen)
    {
        lg2::error("Sensor aux names PDR too small: {SIZE}<{NEED}, tid={TID}.",
                   "SIZE", pdrData.size(), "NEED", minPdrLen, "TID", tid);
        return nullptr;
    }
    std::vector<std::vector<std::pair<NameLanguageTag, SensorName>>>
        sensorAuxNames{};
    if (!parseAuxiliaryNameStrings(pdr->names, pdrData.data() + pdrData.size(),
                                   pdr->sensor_count, pdr->sensor_id,
                                   sensorAuxNames, "Sensor"))
    {
        return nullptr;
    }
    return std::make_shared<SensorAuxiliaryNames>(
        pdr->sensor_id, pdr->sensor_count, sensorAuxNames);
}

std::shared_ptr<EffecterAuxiliaryNames>
    Terminus::parseEffecterAuxiliaryNamesPDR(
        const std::vector<uint8_t>& pdrData)
{
    auto pdr =
        reinterpret_cast<const struct pldm_effecter_auxiliary_names_pdr*>(
            pdrData.data());
    // Reject a buffer too small for the fixed header (names[] excluded).
    const size_t minPdrLen =
        sizeof(struct pldm_effecter_auxiliary_names_pdr) - sizeof(pdr->names);
    if (pdrData.size() < minPdrLen)
    {
        lg2::error(
            "Effecter aux names PDR too small: {SIZE}<{NEED}, tid={TID}.",
            "SIZE", pdrData.size(), "NEED", minPdrLen, "TID", tid);
        return nullptr;
    }
    std::vector<std::vector<std::pair<NameLanguageTag, EffecterName>>>
        effecterAuxNames{};
    if (!parseAuxiliaryNameStrings(pdr->names, pdrData.data() + pdrData.size(),
                                   pdr->effecter_count, pdr->effecter_id,
                                   effecterAuxNames, "Effecter"))
    {
        return nullptr;
    }
    return std::make_shared<EffecterAuxiliaryNames>(
        pdr->effecter_id, pdr->effecter_count, effecterAuxNames);
}

void Terminus::parseEntityAssociationPDR(
    [[maybe_unused]] const std::vector<uint8_t>& pdrData)
{
#ifdef OEM_NVIDIA
    using AddResult = oem_nvidia::EntityTree::AddResult;
    static const std::map<AddResult, std::string_view> rejections = {
        {AddResult::malformed, "not a well-formed Entity Association PDR"},
        {AddResult::reservedContainer,
         "defines the reserved SYSTEM containerId"},
        {AddResult::containerConflict,
         "binds an already known containerId to a different entity"},
    };
    auto rejection = rejections.find(
        oem_nvidia::InventoryManager::get().addEntityAssociation(tid, pdrData));
    if (rejection != rejections.end())
    {
        lg2::error(
            "ERROR: TID:{TID} Entity Association PDR ({LEN} bytes) skipped: {WHY}",
            "TID", tid, "LEN", pdrData.size(), "WHY", rejection->second);
    }
#endif
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

    // The memcpy fallback above bypasses libpldm's decode validation, so
    // reject an out-of-range effecterDataSize before it selects a branch
    // of union_effecter_data_size downstream.
    if (parsedPdr->effecter_data_size > PLDM_EFFECTER_DATA_SIZE_MAX)
    {
        // Copy the packed fields; lg2 binds arguments by reference.
        uint8_t dataSize = parsedPdr->effecter_data_size;
        uint16_t effecterId = parsedPdr->effecter_id;
        lg2::error(
            "parseNumericEffecterPDR: unknown effecterDataSize {SIZE}, tid={TID}, effecterId={EFFECTERID}, skipping effecter.",
            "SIZE", dataSize, "TID", tid, "EFFECTERID", effecterId);
        return nullptr;
    }
    return parsedPdr;
}

std::optional<std::tuple<SensorID, StateSetInfo>> Terminus::parseStateSensorPDR(
    std::vector<uint8_t>& stateSensorPdr)
{
    if (stateSensorPdr.size() < sizeof(pldm_state_sensor_pdr))
    {
        lg2::error("State sensor PDR too short: TID:{TID}", "TID", tid);
        return std::nullopt;
    }
    auto pdr =
        reinterpret_cast<const pldm_state_sensor_pdr*>(stateSensorPdr.data());
    std::vector<StateSetData> stateSets{};
    auto statesPtr = pdr->possible_states;
    auto compositeSensorCount = pdr->composite_sensor_count;

    parseStateSetInfo(statesPtr, compositeSensorCount,
                      stateSensorPdr.data() + stateSensorPdr.size(), stateSets);

    auto entityInfo =
        std::make_tuple(static_cast<ContainerID>(pdr->container_id),
                        static_cast<EntityType>(pdr->entity_type),
                        static_cast<EntityInstance>(pdr->entity_instance));

    auto stateSetInfo =
        std::make_tuple(std::move(entityInfo), std::move(stateSets));
    return std::make_tuple(pdr->sensor_id, std::move(stateSetInfo));
}

std::optional<std::tuple<EffecterID, StateSetInfo>>
    Terminus::parseStateEffecterPDR(std::vector<uint8_t>& stateEffecterPdr)
{
    if (stateEffecterPdr.size() < sizeof(pldm_state_effecter_pdr))
    {
        lg2::error("State effecter PDR too short: TID:{TID}", "TID", tid);
        return std::nullopt;
    }
    auto pdr = reinterpret_cast<const pldm_state_effecter_pdr*>(
        stateEffecterPdr.data());
    std::vector<StateSetData> stateSets{};
    auto statesPtr = pdr->possible_states;
    auto compositeSensorCount = pdr->composite_effecter_count;

    parseStateSetInfo(statesPtr, compositeSensorCount,
                      stateEffecterPdr.data() + stateEffecterPdr.size(),
                      stateSets);

    auto entityInfo =
        std::make_tuple(static_cast<ContainerID>(pdr->container_id),
                        static_cast<EntityType>(pdr->entity_type),
                        static_cast<EntityInstance>(pdr->entity_instance));
    auto stateSetInfo =
        std::make_tuple(std::move(entityInfo), std::move(stateSets));
    return std::make_tuple(pdr->effecter_id, std::move(stateSetInfo));
}

void Terminus::parseStateSetInfo(
    const unsigned char* statesPtr, uint8_t compositeSensorCount,
    const unsigned char* end, std::vector<StateSetData>& stateSets)
{
    while (compositeSensorCount--)
    {
        // The fixed part of the record (state_set_id + possible_states_size +
        // states[0]) must be present before we read possible_states_size and
        // walk the states[] array driven by that wire-declared size.
        if (statesPtr > end || static_cast<size_t>(end - statesPtr) <
                                   sizeof(state_sensor_possible_states))
        {
            break;
        }
        auto state =
            reinterpret_cast<const state_sensor_possible_states*>(statesPtr);
        // states[] is bitfield8_t; compare/subtract in byte units against end.
        const unsigned char* statesBytes =
            reinterpret_cast<const unsigned char*>(&state->states[0]);
        if (statesBytes > end || static_cast<size_t>(end - statesBytes) <
                                     state->possible_states_size)
        {
            break;
        }
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
    std::vector<uint8_t> data;

    // Ensure the fixed OEM PDR header (iana, record id, data_length) is present
    // before it is read via the cast.
    constexpr size_t fixedLen = offsetof(pldm_oem_pdr, vendor_specific_data);
    if (oemPdr.size() < fixedLen)
    {
        lg2::error("OEM PDR too short: TID:{TID}", "TID", tid);
        return {};
    }
    auto pdr = reinterpret_cast<const pldm_oem_pdr*>(oemPdr.data());

    // vendor-specific data bytes starting from 0; 0 = 1 byte, 1 = 2 bytes,
    // and so on. data_length is wire-declared: cap the copy at the bytes that
    // are actually present so the memcpy source cannot read past oemPdr.
    size_t wantBytes = static_cast<size_t>(pdr->data_length) + 1;
    const size_t availBytes = oemPdr.size() - fixedLen;
    if (wantBytes > availBytes)
    {
        wantBytes = availBytes;
    }
    data.resize(wantBytes);
    memcpy(data.data(), pdr->vendor_specific_data, wantBytes);
    return std::make_tuple(pdr->vendor_iana, pdr->ome_record_id,
                           std::move(data));
}

exec::task<int> Terminus::scanInventories()
{
    if (!terminusManager.toMctpInfo(tid).has_value())
    {
        lg2::error(
            "scanInventories: TID {TID} has no MCTP mapping; device inventory cannot be matched, sensors/telemetry will be missing for this terminus",
            "TID", tid);
    }
#ifdef OEM_NVIDIA
    co_return co_await oem_nvidia::InventoryManager::get().scan(*this);
#else
    co_return PLDM_SUCCESS;
#endif
}

exec::task<int> Terminus::updateAssociations()
{
#ifdef OEM_NVIDIA
    auto& inventory = oem_nvidia::InventoryManager::get();
    inventory.bindAll(tid);
    auto findInventory = [&](const EntityInfo& entityInfo) {
        return inventory.findInventory(tid, oem_nvidia::EntityId{entityInfo});
    };
    auto isFallbackAssociation =
        [&](const std::vector<std::string>& paths, EntityType entityType) {
            return inventory.isFallbackAssociation(tid, paths, entityType);
        };
    // Replace @a paths with a CPU redirection that found a CPU.
    auto redirect = [](std::vector<std::string>& paths,
                       std::vector<std::string> cpuPaths) {
        if (!cpuPaths.empty())
        {
            paths = std::move(cpuPaths);
        }
    };
#else
    // No entity topology: sensors and effecters stay associated with the
    // system inventory they were created with.
    auto findInventory = [](const EntityInfo&) {
        return std::vector<std::string>{systemInventoryPath()};
    };
    auto isFallbackAssociation =
        [](const std::vector<std::string>&, EntityType) { return false; };
#endif

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
#ifdef OEM_NVIDIA
            const auto baseType = std::get<1>(entityInfo) & entityTypeMask;
            const auto containerId = std::get<0>(entityInfo);
            // PROC_IO_MODULE sensors belong under the CPU chassis: the first
            // CPU of the terminus's first ProcessorModule.
            if (baseType == PLDM_ENTITY_PROC_IO_MODULE)
            {
                redirect(inventoryPaths,
                         inventory.moduleProcessorInventory(tid));
            }
            // SYS_BUS numeric sensors (e.g. CLink/NVLink counters for
            // bandwidth, CRC, replay) should be associated with the CPU
            // that contains the bus, or the first CPU of the containing
            // ProcessorModule. SYS_BUS has no inventory interface of its
            // own.
            else if (baseType == PLDM_ENTITY_SYS_BUS)
            {
                redirect(inventoryPaths,
                         inventory.processorInventory(tid, containerId));
            }
            // Workaround: count/metric sensors (baseUnit == COUNTS) are
            // global per CPU (e.g. PageRetirementCount, TjMaxDramIndex),
            // so associate with CPU regardless of entity type.
            else if (ptr->getBaseUnit() == PLDM_SENSOR_UNIT_COUNTS)
            {
                redirect(inventoryPaths, inventory.containerProcessorInventory(
                                             tid, containerId));
            }
#endif
        }
        // A resolution that degraded past the entity's dedicated inventory
        // (to the system chassis, or to an ancestor while the real parent is
        // still settling) is a placeholder, not a real parent: keep
        // defaultInventoryAssociated set so the shared-memory push in
        // NumericSensor::updateReading() stays gated and the first
        // MetricReport (shmem) insert cannot freeze a wrong metricProperty
        // URI. Only the MetricReport entry is withheld — the sensor itself
        // stays visible on D-Bus/Redfish under the fallback parent.
        // refreshAssociations() re-resolves once the real inventory appears.
        // An explicit per-sensor inventory override from EM configuration is
        // authoritative and never treated as a fallback.
        bool fallbackAssociation =
            !inventoryPath &&
            isFallbackAssociation(inventoryPaths, std::get<1>(entityInfo));
        if (fallbackAssociation)
        {
            lg2::debug("Sensor {NAME}: association degraded to a "
                       "non-dedicated parent; metric-report updates gated "
                       "until inventory resolves.",
                       "NAME", ptr->getSensorName());
        }
        ptr->setInventoryPaths(inventoryPaths, fallbackAssociation);

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

#ifdef OEM_NVIDIA
    auto hasStateSetId = [](const StateSetInfo& info, uint16_t id) {
        for (const auto& sd : std::get<1>(info))
            if (std::get<0>(sd) == id)
                return true;
        return false;
    };
#endif

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

#ifdef OEM_NVIDIA
        const auto baseType = std::get<1>(entityInfo) & entityTypeMask;
        const auto containerId = std::get<0>(entityInfo);
        // SYS_BUS state sensors (CLink/NVLink port state) need CPU
        // associations. SYS_BUS has no inventory interface of its own; take
        // the CPU that contains the bus, or the first CPU of the containing
        // ProcessorModule.
        if (baseType == PLDM_ENTITY_SYS_BUS && inventoryPaths.empty())
        {
            redirect(inventoryPaths,
                     inventory.processorInventory(tid, containerId));
        }

        // Workaround: MEMORY_CONTROLLER state sensors (e.g.
        // MemorySpareChannelPresence) are CPU-global; associate with CPU
        // instead of ProcessorModule when container is PLDM_ENTITY_PROC.
        // Skip for PERFORMANCE state sets (e.g. MemoryPerformance) which
        // get their own DIMM-level associations in the OEM phase.
        if (baseType == PLDM_ENTITY_MEMORY_CONTROLLER &&
            !hasStateSetId(ptr->sensorInfo, PLDM_STATESET_ID_PERFORMANCE))
        {
            redirect(inventoryPaths,
                     inventory.containerProcessorInventory(tid, containerId));
        }
#endif

        // Same placeholder rule as numeric sensors above: keep
        // defaultInventoryAssociated set while the association is a
        // degraded fallback so state-set shared-memory updates (e.g. port
        // Status/State/Health MetricReport entries) stay gated until
        // refreshAssociations() re-resolves against real inventory.
        bool fallbackAssociation =
            !inventoryPath &&
            isFallbackAssociation(inventoryPaths, std::get<1>(entityInfo));
        if (fallbackAssociation)
        {
            lg2::debug("State sensor {ID}: association degraded to a "
                       "non-dedicated parent; metric-report updates gated "
                       "until inventory resolves.",
                       "ID", ptr->sensorId);
        }
        ptr->setInventoryPaths(inventoryPaths, fallbackAssociation);
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
            tid, true, pdr, sensorName, systemInventoryPath(), sensorEventInfo);
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
            tid, true, pdr, sensorName, systemInventoryPath(),
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
            tid, true, pdr, effecterName, systemInventoryPath(),
            terminusManager);
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
            systemInventoryPath(), stateSensorEventInfo);
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
            systemInventoryPath(), terminusManager);
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
        numericEffecter->needsUpdate = true;
    }

    for (auto& stateEffecter : stateEffecters)
    {
        stateEffecter->setAvailable(true);
        stateEffecter->needsUpdate = true;
    }

    lg2::info("Terminus {TID} is online", "TID", tid);
}

size_t Terminus::rearmTrackedEffecters()
{
    size_t rearmed = 0;
    for (auto& numericEffecter : numericEffecters)
    {
        if (!numericEffecter || !numericEffecter->trackOperationalState ||
            numericEffecter->needsUpdate)
        {
            continue;
        }
        numericEffecter->needsUpdate = true;
        ++rearmed;
    }

    if (rearmed)
    {
        lg2::info(
            "rearmTrackedEffecters: re-armed {N} tracked effecter(s) on tid={TID}",
            "N", rearmed, "TID", tid);
    }

    return rearmed;
}

void Terminus::setOffline()
{
    resumed = false;
    const auto mctpInfo = terminusManager.toMctpInfo(tid);
    const std::string terminusEid =
        mctpInfo ? std::to_string(std::get<0>(*mctpInfo)) : "unknown";
    for (auto numericSensor : numericSensors)
    {
        numericSensor->handleErrGetSensorReading();
        if (numericSensor->clearThresholdAlarms())
        {
            lg2::info(
                "Terminus TID {TID} EID {EID} went offline, cleared threshold alarms of sensor {NAME}.",
                "TID", tid, "EID", terminusEid, "NAME",
                numericSensor->getSensorName());
        }
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

    // Write the NaN readings queued above now. Polling of this terminus
    // stops or has already stopped, so its poll cycle will not flush them.
    pldm::shmem_utils::SharedMemoryManager::updateAggregateTelemetryOnTAL();

    lg2::info("Terminus {TID} is offline", "TID", tid);
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
    exec::start_detached(
        stdexec::on(stdexec::inline_scheduler{},
                    refreshAssociationsTask() |
                        stdexec::then([&](int rc) { rcOpt.emplace(rc); })));
}

exec::task<int> Terminus::refreshAssociationsTask()
{
    // A refresh request can land while this task is suspended inside
    // scanInventories() or updateAssociations() (refreshAssociations()
    // sees the task in flight and only latches needRefresh). Re-run the
    // scan+update pass until no request landed mid-pass so a latched
    // request is never dropped — a dropped request leaves fallback-gated
    // sensors without metric-report updates until an unrelated future
    // inventory event.
    do
    {
        needRefresh = false;
        // Update inventory list
        co_await scanInventories();
        // Update Sensor PDIs after scans are done
        co_await updateAssociations();
    } while (needRefresh);
    co_return PLDM_SUCCESS;
}

} // namespace platform_mc
} // namespace pldm
