/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "common/utils.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

/** @brief Entity-manager configs where a device is described by the platform
 *         topology.
 *
 *  A device is a Configuration.NvDevice (Name, Kind, Instance, EID,
 *  ParentChassis, with a .Transport child interface). Its PLDM facts are a
 *  Configuration.PLDMDevice named "<device>_PLDM" whose
 *  UpdateableComponents, FWInventoryOnlyComponent and SensorAuxNames child
 *  interfaces map names to ids, whose Ports child interface maps ids to
 *  names, and whose SensorRoles child interface maps roles to sensor or
 *  effecter names. The firmware-update policy that follows from the device kind
 *  (associations, inventory paths) is in fw-update/em_device_policy.hpp.
 */
namespace pldm::em_device
{

constexpr auto nvDeviceIntf = "xyz.openbmc_project.Configuration.NvDevice";
constexpr auto transportIntf =
    "xyz.openbmc_project.Configuration.NvDevice.Transport";
constexpr auto pldmDeviceIntf = "xyz.openbmc_project.Configuration.PLDMDevice";
constexpr auto devicePrefixIntf =
    "xyz.openbmc_project.Configuration.DevicePrefix";
constexpr auto topChassisIntf = "xyz.openbmc_project.Configuration.TopChassis";
constexpr auto inventoryRoot = "/xyz/openbmc_project/inventory";
constexpr auto chassisRoot = "/xyz/openbmc_project/inventory/system/chassis/";
constexpr auto componentRoot =
    "/xyz/openbmc_project/inventory/system/component/";
constexpr auto bmcRoot = "/xyz/openbmc_project/inventory/system/bmc/";
constexpr auto pldmSuffix = "_PLDM";
/** @brief Largest EID a device can be configured with: 0xFF is the MCTP
 *         broadcast EID (DSP0236 8.2). */
constexpr uint64_t maxDeviceEid = 0xFE;

/** @brief The roles a SensorRoles entry of a PLDMDevice record can give a
 *         sensor or effecter; each is the chassis association of that
 *         name. */
constexpr auto primaryTemperatureSensor = "primary_temperature_sensor";
constexpr auto primaryPowerControl = "primary_power_control";
constexpr auto primaryPowerSensor = "primary_power_sensor";
constexpr auto primaryCpupowerSensor = "primary_cpupower_sensor";

struct Device
{
    std::string name;
    std::string kind;
    std::string parentChassis;
    std::optional<uint64_t> eid;
    std::optional<uint64_t> instance;
    std::optional<uint64_t> cpuIndex;
    // from the .Transport child interface
    std::string medium;
};

struct PldmDevice
{
    // updateable and inventory-only firmware components together
    std::map<uint16_t, std::string> components;
    // ids of the components listed as FWInventoryOnlyComponent
    std::set<uint16_t> inventoryOnly;
    std::map<uint16_t, std::string> auxNames;
    // sensor id -> port name; the port is under the device's network
    // adapter, or under the device's chassis (<chassis>/Ports/<port>)
    // without one. Device-local ports only: NVSwitch fabric ports
    // (.../fabrics/<fabric>/Switches/<sw>/Ports/<p>) are not described by
    // a PLDMDevice record and keep their SensorPortInfo configs.
    std::map<uint16_t, std::string> ports;
    std::string networkAdapter;
    std::optional<uint64_t> portMaxSpeed;
    // name the device's unconfigured firmware components are named after
    std::string firmwareName;
    // role (primaryTemperatureSensor, ...) -> name of the sensor or
    // effecter that has it
    std::map<std::string, std::string> sensorRoles;
};

/** @brief The string property @p key of @p props; empty when it is absent
 *         or not a string. */
std::string stringProp(const pldm::utils::PropertyMap& props,
                       const std::string& key);

/** @brief The properties of @p intf at @p path, or std::nullopt when they
 *         cannot be read. */
std::optional<pldm::utils::PropertyMap> getAll(
    const std::string& service, const std::string& path,
    const std::string& intf,
    const pldm::utils::DBusHandlerInterface& bus = pldm::utils::DBusHandler());

/** @brief All objects implementing @p intf: path -> service. */
std::map<std::string, std::string> objectsWith(
    const std::string& intf,
    const pldm::utils::DBusHandlerInterface& bus = pldm::utils::DBusHandler());

/** @brief The devices of the platform topology.
 *
 *  A device is published twice: by the topology (Name, Kind, Instance, EID,
 *  ParentChassis, CpuIndex and a Transport child interface) and by the entity
 *  file of its kind, a record of the same Type that carries InstanceNumber in
 *  place of Instance and no parent. The two are merged by Name; a value the
 *  first one lacks is taken from the second, and a value both carry with a
 *  different content is logged with both values and record paths and kept
 *  from the first. An EID, Instance, InstanceNumber or CpuIndex that is
 *  not a non-negative integer in range is logged with its value and record
 *  path and treated as absent.
 */
std::vector<Device> devices(
    const pldm::utils::DBusHandlerInterface& bus = pldm::utils::DBusHandler());

/** @brief The device named @p name, if there is one. */
std::optional<Device> deviceByName(
    const std::string& name,
    const pldm::utils::DBusHandlerInterface& bus = pldm::utils::DBusHandler());

/** @brief @p name as entity-manager writes it into an object path: every
 *         character outside [A-Za-z0-9_] becomes '_'
 *         (dbus_regex::sanitizeForDBusMember). */
std::string dbusPathSegment(std::string name);

/** @brief The device whose name entity-manager publishes as the object
 *         path segment @p segment, e.g. the leaf of its chassis path. */
std::optional<Device> deviceByPathSegment(
    const std::string& segment,
    const pldm::utils::DBusHandlerInterface& bus = pldm::utils::DBusHandler());

/** @brief The device-name prefix of the platform, from the DevicePrefix
 *         record of the platform topology ("HGX_" on an HMC); empty on a
 *         platform whose devices carry no prefix and no record. */
std::string platformPrefix(
    const pldm::utils::DBusHandlerInterface& bus = pldm::utils::DBusHandler());

/** @brief The inventory path of the platform's top chassis, from the
 *         TopChassis record of the platform topology; empty without a
 *         record or a Chassis in it. */
std::string topChassisPath(
    const pldm::utils::DBusHandlerInterface& bus = pldm::utils::DBusHandler());

/** @brief The id -> name map EM publishes for a nested object, as the
 *         child interface "<intf>.<key>" with one property per id. */
std::map<uint16_t, std::string> idNameMap(
    const std::string& service, const std::string& path,
    const std::string& intf,
    const pldm::utils::DBusHandlerInterface& bus = pldm::utils::DBusHandler());

/** @brief The name -> id map EM publishes for a nested object, as the
 *         child interface "<intf>.<key>" with one property per name;
 *         returned keyed by id. */
std::map<uint16_t, std::string> nameIdMap(
    const std::string& service, const std::string& path,
    const std::string& intf,
    const pldm::utils::DBusHandlerInterface& bus = pldm::utils::DBusHandler());

/** @brief The PLDMDevice record of a device, if it has one.
 *
 *  A SensorRoles entry of a role this reader does not know is left out
 *  with a debug log; one whose value is not a non-empty string is logged
 *  with its value and left out. */
std::optional<PldmDevice> pldmDevice(
    const std::string& deviceName,
    const pldm::utils::DBusHandlerInterface& bus = pldm::utils::DBusHandler());

/** @brief @p name without the platform name prefix @p prefix. */
std::string baseName(const std::string& name, const std::string& prefix);

/** @brief The terminus config of a CPU device: the terminus name is the
 *         parent chassis without the platform name prefix, the entity
 *         instance is the device instance, and the CPU index of the sensor
 *         name prefix is CpuIndex of the device, default 0. Both are
 *         uint16_t on the terminus; a device whose values do not fit has
 *         no config. */
struct TerminusConfig
{
    std::string name;
    int instance = 0;
    uint16_t cpuIndex = 0;
};

std::optional<TerminusConfig> terminusConfig(const Device& dev,
                                             const std::string& prefix);

} // namespace pldm::em_device
