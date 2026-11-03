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

#include "em_device.hpp"

#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <limits>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace pldm::em_device
{

std::string stringProp(const pldm::utils::PropertyMap& props,
                       const std::string& key)
{
    auto it = props.find(key);
    if (it == props.end())
    {
        return {};
    }
    if (const auto* s = std::get_if<std::string>(&it->second))
    {
        return *s;
    }
    return {};
}

std::optional<pldm::utils::PropertyMap> getAll(
    const std::string& service, const std::string& path,
    const std::string& intf, const pldm::utils::DBusHandlerInterface& bus)
{
    try
    {
        return bus.getDbusPropertiesVariant(service.c_str(), path.c_str(),
                                            intf.c_str());
    }
    catch (const std::exception& e)
    {
        lg2::debug("GetAll {INTF} on {PATH} failed: {ERROR}", "INTF", intf,
                   "PATH", path, "ERROR", e);
        return std::nullopt;
    }
}

std::map<std::string, std::string> objectsWith(
    const std::string& intf, const pldm::utils::DBusHandlerInterface& bus)
{
    std::map<std::string, std::string> out;
    try
    {
        auto subtree = bus.getSubtree(inventoryRoot, 0, {intf});
        for (const auto& [path, serviceMap] : subtree)
        {
            if (!serviceMap.empty())
            {
                out.emplace(path, serviceMap.begin()->first);
            }
        }
    }
    catch (const std::exception& e)
    {
        lg2::warning("GetSubTree for {INTF} failed: {ERROR}", "INTF", intf,
                     "ERROR", e);
    }
    return out;
}

namespace
{

/** @brief The record of a device being merged into the first record of the
 *         same name. */
struct MergeSource
{
    const std::string& name;
    const std::string& firstPath;
    const std::string& path;
};

void warnConflict(const MergeSource& src, std::string_view field,
                  const std::string& kept, const std::string& dropped)
{
    lg2::warning("NvDevice records of {NAME} disagree on {FIELD}: {KEPT} at "
                 "{FIRST_PATH} and {DROPPED} at {PATH}; keeping the first",
                 "NAME", src.name, "FIELD", field, "KEPT", kept, "FIRST_PATH",
                 src.firstPath, "DROPPED", dropped, "PATH", src.path);
}

/** @brief Fill @p known from @p other when it is unset; warn when both are
 *         set and differ. */
void merge(std::string& known, std::string other, const MergeSource& src,
           std::string_view field)
{
    if (known.empty())
    {
        known = std::move(other);
    }
    else if (!other.empty() && other != known)
    {
        warnConflict(src, field, known, other);
    }
}

void merge(std::optional<uint64_t>& known, std::optional<uint64_t> other,
           const MergeSource& src, std::string_view field)
{
    if (!known)
    {
        known = other;
    }
    else if (other && *other != *known)
    {
        warnConflict(src, field, std::to_string(*known),
                     std::to_string(*other));
    }
}

/** @brief @p value as text for a log: a number, a quoted string, or the
 *         variant index of any other type. */
std::string formatValue(const pldm::utils::PropertyValue& value)
{
    return std::visit(
        [&value](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>)
            {
                return v ? "true" : "false";
            }
            else if constexpr (std::is_arithmetic_v<T>)
            {
                return std::format("{}", v);
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                return std::format("\"{}\"", v);
            }
            else
            {
                return std::format("<variant index {}>", value.index());
            }
        },
        value);
}

/** @brief The number property @p key of @p props, if present and in
 *         [0, @p max]; a value that is not such a number is logged with the
 *         device @p name and record @p path, and dropped. */
std::optional<uint64_t> readNumber(
    const pldm::utils::PropertyMap& props, const std::string& key, uint64_t max,
    const std::string& name, const std::string& path)
{
    auto it = props.find(key);
    if (it == props.end())
    {
        return std::nullopt;
    }
    auto value = pldm::utils::numberProp(props, key, max);
    if (!value)
    {
        lg2::warning("NvDevice {NAME} at {PATH} has an invalid {KEY} {VALUE}; "
                     "ignoring it",
                     "NAME", name, "PATH", path, "KEY", key, "VALUE",
                     formatValue(it->second));
    }
    return value;
}

/** @brief Fill the transport fields of @p dev from its .Transport child
 *         interface, if it has one. */
void readTransport(Device& dev, const std::string& service,
                   const std::string& path,
                   const pldm::utils::DBusHandlerInterface& bus)
{
    auto props = getAll(service, path, transportIntf, bus);
    if (!props)
    {
        return;
    }
    dev.medium = stringProp(*props, "Medium");
    dev.poolStartEid =
        readNumber(*props, "PoolStartEID", maxDeviceEid, dev.name, path);
    dev.poolEndEid =
        readNumber(*props, "PoolEndEID", maxDeviceEid, dev.name, path);
}

/** @brief The device of the NvDevice record @p props at @p path; no name
 *         when the record has none. */
Device readDevice(const pldm::utils::PropertyMap& props,
                  const std::string& service, const std::string& path,
                  const pldm::utils::DBusHandlerInterface& bus)
{
    constexpr auto noMax = std::numeric_limits<uint64_t>::max();
    Device dev;
    dev.name = stringProp(props, "Name");
    if (dev.name.empty())
    {
        return dev;
    }
    dev.kind = stringProp(props, "Kind");
    dev.parentChassis = stringProp(props, "ParentChassis");
    dev.eid = readNumber(props, "EID", maxDeviceEid, dev.name, path);
    dev.instance = readNumber(props, "Instance", noMax, dev.name, path);
    if (!dev.instance)
    {
        dev.instance =
            readNumber(props, "InstanceNumber", noMax, dev.name, path);
    }
    dev.cpuIndex = readNumber(props, "CpuIndex", noMax, dev.name, path);
    readTransport(dev, service, path, bus);
    return dev;
}

/** @brief Fill the fields of @p known that it lacks from @p other, a second
 *         record of the same device. */
void mergeDevice(Device& known, Device&& other, const MergeSource& src)
{
    merge(known.kind, std::move(other.kind), src, "Kind");
    merge(known.parentChassis, std::move(other.parentChassis), src,
          "ParentChassis");
    merge(known.eid, other.eid, src, "EID");
    merge(known.instance, other.instance, src, "Instance");
    merge(known.cpuIndex, other.cpuIndex, src, "CpuIndex");
    merge(known.medium, std::move(other.medium), src, "Medium");
    merge(known.poolStartEid, other.poolStartEid, src, "PoolStartEID");
    merge(known.poolEndEid, other.poolEndEid, src, "PoolEndEID");
}

} // namespace

std::vector<Device> devices(const pldm::utils::DBusHandlerInterface& bus)
{
    std::vector<Device> out;
    // object path of the first record of each device, for the conflict log
    std::map<std::string, std::string> firstPaths;
    for (const auto& [path, service] : objectsWith(nvDeviceIntf, bus))
    {
        auto props = getAll(service, path, nvDeviceIntf, bus);
        if (!props)
        {
            continue;
        }
        Device dev = readDevice(*props, service, path, bus);
        if (dev.name.empty())
        {
            continue;
        }
        auto [first, inserted] = firstPaths.try_emplace(dev.name, path);
        if (inserted)
        {
            out.push_back(std::move(dev));
            continue;
        }
        auto known = std::ranges::find(out, dev.name, &Device::name);
        mergeDevice(*known, std::move(dev), {known->name, first->second, path});
    }
    return out;
}

std::optional<Device> deviceByName(const std::string& name,
                                   const pldm::utils::DBusHandlerInterface& bus)
{
    for (auto& dev : devices(bus))
    {
        if (dev.name == name)
        {
            return dev;
        }
    }
    return std::nullopt;
}

std::string dbusPathSegment(std::string name)
{
    for (auto& c : name)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
        {
            c = '_';
        }
    }
    return name;
}

std::optional<Device> deviceByPathSegment(
    const std::string& segment, const pldm::utils::DBusHandlerInterface& bus)
{
    for (auto& dev : devices(bus))
    {
        if (dbusPathSegment(dev.name) == segment)
        {
            return dev;
        }
    }
    return std::nullopt;
}

std::string platformPrefix(const pldm::utils::DBusHandlerInterface& bus)
{
    for (const auto& [path, service] : objectsWith(devicePrefixIntf, bus))
    {
        if (auto props = getAll(service, path, devicePrefixIntf, bus))
        {
            return stringProp(*props, "Prefix");
        }
    }
    return {};
}

std::map<uint16_t, std::string> idNameMap(
    const std::string& service, const std::string& path,
    const std::string& intf, const pldm::utils::DBusHandlerInterface& bus)
{
    std::map<uint16_t, std::string> out;
    auto props = getAll(service, path, intf, bus);
    if (!props)
    {
        return out;
    }
    for (const auto& [key, value] : *props)
    {
        const auto* name = std::get_if<std::string>(&value);
        uint16_t id = 0;
        auto [ptr,
              ec] = std::from_chars(key.data(), key.data() + key.size(), id);
        if (name == nullptr || ec != std::errc() ||
            ptr != key.data() + key.size())
        {
            continue;
        }
        out.emplace(id, *name);
    }
    return out;
}

std::map<uint16_t, std::string> nameIdMap(
    const std::string& service, const std::string& path,
    const std::string& intf, const pldm::utils::DBusHandlerInterface& bus)
{
    std::map<uint16_t, std::string> out;
    auto props = getAll(service, path, intf, bus);
    if (!props)
    {
        return out;
    }
    for (const auto& [name, value] : *props)
    {
        auto id = pldm::utils::numberValue(value);
        if (!id || *id > std::numeric_limits<uint16_t>::max())
        {
            continue;
        }
        out.emplace(static_cast<uint16_t>(*id), name);
    }
    return out;
}

std::optional<PldmDevice> pldmDevice(
    const std::string& deviceName, const pldm::utils::DBusHandlerInterface& bus)
{
    const std::string recordName = deviceName + pldmSuffix;
    for (const auto& [path, service] : objectsWith(pldmDeviceIntf, bus))
    {
        auto props = getAll(service, path, pldmDeviceIntf, bus);
        if (!props || stringProp(*props, "Name") != recordName)
        {
            continue;
        }
        PldmDevice out;
        const std::string base(pldmDeviceIntf);
        out.components =
            nameIdMap(service, path, base + ".UpdateableComponents", bus);
        for (auto& [id, name] :
             nameIdMap(service, path, base + ".FWInventoryOnlyComponent", bus))
        {
            out.inventoryOnly.insert(id);
            out.components.emplace(id, std::move(name));
        }
        out.auxNames = nameIdMap(service, path, base + ".SensorAuxNames", bus);
        out.ports = idNameMap(service, path, base + ".Ports", bus);
        out.networkAdapter = stringProp(*props, "NetworkAdapter");
        out.portMaxSpeed = pldm::utils::numberProp(*props, "PortMaxSpeedMBps");
        out.firmwareName = stringProp(*props, "FirmwareName");
        return out;
    }
    return std::nullopt;
}

std::string baseName(const std::string& name, const std::string& prefix)
{
    if (!prefix.empty() && name.starts_with(prefix))
    {
        return name.substr(prefix.size());
    }
    return name;
}

std::optional<TerminusConfig> terminusConfig(const Device& dev,
                                             const std::string& prefix)
{
    constexpr uint64_t max = std::numeric_limits<uint16_t>::max();
    if (dev.kind != "cpu" || dev.cpuIndex.value_or(0) > max ||
        dev.instance.value_or(0) > max)
    {
        return std::nullopt;
    }
    TerminusConfig config;
    config.name = baseName(
        dev.parentChassis.empty() ? dev.name : dev.parentChassis, prefix);
    config.instance = static_cast<int>(dev.instance.value_or(0));
    config.cpuIndex = static_cast<uint16_t>(dev.cpuIndex.value_or(0));
    return config;
}

} // namespace pldm::em_device
