// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
#pragma once

#include "common/utils.hpp"

#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>
#include <vector>

/** @brief Entity-manager configs where a device is described by the platform
 *         topology.
 *
 *  A device is a Configuration.NvDevice (Name, Kind, Instance, EID,
 *  ParentChassis, with a .Transport child interface). Its PLDM facts are a
 *  Configuration.PLDMDevice named "<device>_PLDM" whose
 *  UpdateableComponents, FWInventoryOnlyComponent and SensorAuxNames child
 *  interfaces map names to ids, and whose Ports child interface maps ids to
 *  names. Associations,
 *  inventory paths and other values that follow from the device kind are
 *  derived here.
 */
namespace pldm::em_device
{

constexpr auto nvDeviceIntf = "xyz.openbmc_project.Configuration.NvDevice";
constexpr auto transportIntf =
    "xyz.openbmc_project.Configuration.NvDevice.Transport";
constexpr auto pldmDeviceIntf = "xyz.openbmc_project.Configuration.PLDMDevice";
constexpr auto mctpEndpointIntf =
    "xyz.openbmc_project.Configuration.MCTPEndpoint";
constexpr auto devicePrefixIntf =
    "xyz.openbmc_project.Configuration.DevicePrefix";
constexpr auto inventoryRoot = "/xyz/openbmc_project/inventory";
constexpr auto chassisRoot = "/xyz/openbmc_project/inventory/system/chassis/";
constexpr auto componentRoot =
    "/xyz/openbmc_project/inventory/system/component/";
constexpr auto bmcRoot = "/xyz/openbmc_project/inventory/system/bmc/";
constexpr auto pldmSuffix = "_PLDM";
constexpr auto mctpSuffix = "_MCTP";

struct Device
{
    std::string name;
    std::string kind;
    std::string parentChassis;
    std::optional<uint64_t> eid;
    std::optional<uint64_t> instance;
    std::optional<uint64_t> cpuIndex;
    pldm::utils::PropertyMap transport;
};

struct PlatformNames
{
    std::string namePrefix;
};

struct PldmDevice
{
    // updateable and inventory-only firmware components together
    std::map<uint16_t, std::string> components;
    // ids of the components listed as FWInventoryOnlyComponent
    std::set<uint16_t> inventoryOnly;
    std::map<uint16_t, std::string> auxNames;
    std::map<uint16_t, std::string> ports;
    std::string networkAdapter;
    std::optional<uint64_t> portMaxSpeed;
    // name the device's unconfigured firmware components are named after
    std::string firmwareName;
};

inline std::string stringProp(const pldm::utils::PropertyMap& props,
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

inline std::optional<uint64_t> numberProp(const pldm::utils::PropertyMap& props,
                                          const std::string& key)
{
    auto it = props.find(key);
    if (it == props.end())
    {
        return std::nullopt;
    }
    return std::visit(
        [](const auto& v) -> std::optional<uint64_t> {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>)
            {
                return std::nullopt;
            }
            else if constexpr (std::is_arithmetic_v<T>)
            {
                if (v < 0)
                {
                    return std::nullopt;
                }
                return static_cast<uint64_t>(v);
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                uint64_t out = 0;
                int base = 10;
                std::string_view s(v);
                if (s.starts_with("0x") || s.starts_with("0X"))
                {
                    s.remove_prefix(2);
                    base = 16;
                }
                auto [ptr, ec] =
                    std::from_chars(s.data(), s.data() + s.size(), out, base);
                if (ec != std::errc() || ptr != s.data() + s.size())
                {
                    return std::nullopt;
                }
                return out;
            }
            else
            {
                return std::nullopt;
            }
        },
        it->second);
}

inline std::optional<pldm::utils::PropertyMap> getAll(
    const std::string& service, const std::string& path,
    const std::string& intf,
    const pldm::utils::DBusHandlerInterface& bus = pldm::utils::DBusHandler())
{
    try
    {
        return bus.getDbusPropertiesVariant(service.c_str(), path.c_str(),
                                            intf.c_str());
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

/** @brief All objects implementing @p intf: path -> service. */
inline std::map<std::string, std::string> objectsWith(
    const std::string& intf,
    const pldm::utils::DBusHandlerInterface& bus = pldm::utils::DBusHandler())
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

/** @brief The devices of the platform topology.
 *
 *  A device is published twice: by the topology (Name, Kind, Instance, EID,
 *  ParentChassis, CpuIndex and a Transport child interface) and by the entity
 *  file of its kind, a record of the same Type that carries InstanceNumber in
 *  place of Instance and no parent. The two are merged by Name; a value the
 *  first one lacks is taken from the second.
 */
inline std::vector<Device> devices(
    const pldm::utils::DBusHandlerInterface& bus = pldm::utils::DBusHandler())
{
    std::vector<Device> out;
    for (const auto& [path, service] : objectsWith(nvDeviceIntf, bus))
    {
        auto props = getAll(service, path, nvDeviceIntf, bus);
        if (!props)
        {
            continue;
        }
        Device dev;
        dev.name = stringProp(*props, "Name");
        if (dev.name.empty())
        {
            continue;
        }
        dev.kind = stringProp(*props, "Kind");
        dev.parentChassis = stringProp(*props, "ParentChassis");
        dev.eid = numberProp(*props, "EID");
        dev.instance = numberProp(*props, "Instance");
        if (!dev.instance)
        {
            dev.instance = numberProp(*props, "InstanceNumber");
        }
        dev.cpuIndex = numberProp(*props, "CpuIndex");
        if (auto tr = getAll(service, path, transportIntf, bus))
        {
            dev.transport = std::move(*tr);
        }

        auto known = std::ranges::find(out, dev.name, &Device::name);
        if (known == out.end())
        {
            out.push_back(std::move(dev));
            continue;
        }
        if (known->kind.empty())
        {
            known->kind = std::move(dev.kind);
        }
        if (known->parentChassis.empty())
        {
            known->parentChassis = std::move(dev.parentChassis);
        }
        if (!known->eid)
        {
            known->eid = dev.eid;
        }
        if (!known->instance)
        {
            known->instance = dev.instance;
        }
        if (!known->cpuIndex)
        {
            known->cpuIndex = dev.cpuIndex;
        }
        if (known->transport.empty())
        {
            known->transport = std::move(dev.transport);
        }
    }
    return out;
}

inline std::optional<Device> deviceByName(const std::string& name)
{
    for (auto& dev : devices())
    {
        if (dev.name == name)
        {
            return dev;
        }
    }
    return std::nullopt;
}

/** @brief The device-name prefix of the platform, from the DevicePrefix
 *         record of the platform topology ("HGX_" on an HMC); empty on a
 *         platform whose devices carry no prefix and no record. */
inline PlatformNames platformNames(
    const pldm::utils::DBusHandlerInterface& bus = pldm::utils::DBusHandler())
{
    PlatformNames out;
    for (const auto& [path, service] : objectsWith(devicePrefixIntf, bus))
    {
        auto props = getAll(service, path, devicePrefixIntf, bus);
        if (!props)
        {
            continue;
        }
        out.namePrefix = stringProp(*props, "Prefix");
        break;
    }
    return out;
}

/** @brief The id -> name map EM publishes for a nested object, as the
 *         child interface "<intf>.<key>" with one property per id. */
inline std::map<uint16_t, std::string> idNameMap(const std::string& service,
                                                 const std::string& path,
                                                 const std::string& intf)
{
    std::map<uint16_t, std::string> out;
    auto props = getAll(service, path, intf);
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

/** @brief The name -> id map EM publishes for a nested object, as the
 *         child interface "<intf>.<key>" with one property per name;
 *         returned keyed by id. */
inline std::map<uint16_t, std::string> nameIdMap(const std::string& service,
                                                 const std::string& path,
                                                 const std::string& intf)
{
    std::map<uint16_t, std::string> out;
    auto props = getAll(service, path, intf);
    if (!props)
    {
        return out;
    }
    for (const auto& [name, value] : *props)
    {
        auto id = numberProp(*props, name);
        if (!id || *id > std::numeric_limits<uint16_t>::max())
        {
            continue;
        }
        out.emplace(static_cast<uint16_t>(*id), name);
    }
    return out;
}

/** @brief The PLDMDevice record of a device, if it has one. */
inline std::optional<PldmDevice> pldmDevice(const std::string& deviceName)
{
    const std::string recordName = deviceName + pldmSuffix;
    for (const auto& [path, service] : objectsWith(pldmDeviceIntf))
    {
        auto props = getAll(service, path, pldmDeviceIntf);
        if (!props || stringProp(*props, "Name") != recordName)
        {
            continue;
        }
        PldmDevice out;
        const std::string base(pldmDeviceIntf);
        out.components =
            nameIdMap(service, path, base + ".UpdateableComponents");
        for (auto& [id, name] :
             nameIdMap(service, path, base + ".FWInventoryOnlyComponent"))
        {
            out.inventoryOnly.insert(id);
            out.components.emplace(id, std::move(name));
        }
        out.auxNames = nameIdMap(service, path, base + ".SensorAuxNames");
        out.ports = idNameMap(service, path, base + ".Ports");
        out.networkAdapter = stringProp(*props, "NetworkAdapter");
        out.portMaxSpeed = numberProp(*props, "PortMaxSpeedMBps");
        out.firmwareName = stringProp(*props, "FirmwareName");
        return out;
    }
    return std::nullopt;
}

/** @brief The MCTPEndpoint override record of a device, if any. */
inline std::optional<pldm::utils::PropertyMap> endpointOverrides(
    const std::string& deviceName)
{
    const std::string recordName = deviceName + mctpSuffix;
    for (const auto& [path, service] : objectsWith(mctpEndpointIntf))
    {
        auto props = getAll(service, path, mctpEndpointIntf);
        if (props && stringProp(*props, "Name") == recordName)
        {
            return props;
        }
    }
    return std::nullopt;
}

inline std::string baseName(const std::string& name, const std::string& prefix)
{
    if (!prefix.empty() && name.starts_with(prefix))
    {
        return name.substr(prefix.size());
    }
    return name;
}

/** @brief The static terminus config of a CPU device: the terminus name is
 *         the parent chassis without the platform name prefix, the entity
 *         instance is the device instance, and the CPU index of the sensor
 *         name prefix is CpuIndex of the device, default 0. */
struct TerminusConfig
{
    std::string name;
    int instance = 0;
    uint16_t cpuIndex = 0;
};

inline std::optional<TerminusConfig> terminusConfig(
    const Device& dev, const PlatformNames& platform)
{
    if (dev.kind != "cpu" ||
        dev.cpuIndex.value_or(0) > std::numeric_limits<uint16_t>::max())
    {
        return std::nullopt;
    }
    TerminusConfig config;
    config.name =
        baseName(dev.parentChassis.empty() ? dev.name : dev.parentChassis,
                 platform.namePrefix);
    config.instance = static_cast<int>(dev.instance.value_or(0));
    config.cpuIndex = static_cast<uint16_t>(dev.cpuIndex.value_or(0));
    return config;
}

struct ComponentRule
{
    std::string forward;
    std::string backward;
    std::vector<std::string> endpoints;
    /** @brief Chassis the component also belongs to, associated by the
     *         "inventory_chassis" forward name; empty when there is none. */
    std::string chassisEndpoint;
    std::string manufacturer = "NVIDIA";
    bool updateOnly = false;
};

/** @brief Association and policy of one firmware component, by device kind:
 *         the BMC firmware components (16 behind an ERoT, 10 behind an
 *         IRoT) are UpdateOnly; FWInventoryOnlyComponent entries are
 *         associated as "active", updateable ones as "activation". */
inline ComponentRule componentRule(const Device& dev,
                                   const PlatformNames& platform,
                                   const PldmDevice& record, uint16_t id)
{
    constexpr uint16_t bmcFwBehindErot = 16;
    constexpr uint16_t bmcFwBehindIrot = 10;
    constexpr uint16_t cxFirmware = 1;
    constexpr uint16_t hpmCpld = 65283;

    ComponentRule rule;
    if (id == bmcFwBehindErot || id == bmcFwBehindIrot)
    {
        rule.updateOnly = true;
        return rule;
    }
    rule.forward = "inventory";
    rule.backward = record.inventoryOnly.contains(id) ? "active" : "activation";
    if (dev.kind == "cpu")
    {
        rule.endpoints = {componentRoot + dev.name};
    }
    else if (dev.kind == "irot")
    {
        rule.endpoints = {bmcRoot + dev.parentChassis};
    }
    else if (dev.kind == "cx9_io" && id == cxFirmware)
    {
        // the firmware belongs to the NIC and to the chassis of the device
        if (!record.networkAdapter.empty())
        {
            rule.endpoints.push_back(
                chassisRoot + dev.name + "/NetworkAdapters/" +
                record.networkAdapter);
        }
        rule.chassisEndpoint = chassisRoot + dev.name;
    }
    else if (dev.kind == "hmc_hpm_sma" && id == hpmCpld)
    {
        rule.endpoints = {componentRoot + platform.namePrefix + "CPLD_" +
                          std::to_string(dev.instance.value_or(0))};
        rule.manufacturer = "Lattice";
    }
    else
    {
        rule.endpoints = {chassisRoot + dev.name};
    }
    return rule;
}

struct InventoryPaths
{
    std::string create;
    std::string update;
};

/** @brief Where pldmd writes the device identity (UUID, SKUs): the RoT
 *         chassis and the update target, by device kind. */
inline std::optional<InventoryPaths> inventoryPaths(
    const Device& dev, const PlatformNames& platform)
{
    const std::string chassis = chassisRoot + dev.name;
    // the BMC's own RoT has the BMC as its parent
    const std::string bmc = bmcRoot + dev.parentChassis;
    if (dev.kind == "gpu")
    {
        return InventoryPaths{chassisRoot + platform.namePrefix + "IRoT_" +
                                  baseName(dev.name, platform.namePrefix),
                              chassis};
    }
    if (dev.kind == "bluefield_sma" || dev.kind == "cx_sma")
    {
        return InventoryPaths{chassisRoot + std::string("IRoT_") + dev.name,
                              chassis};
    }
    if (dev.kind == "erot")
    {
        return InventoryPaths{chassis, bmc};
    }
    if (dev.kind == "irot")
    {
        return InventoryPaths{"", bmc};
    }
    if (dev.kind == "cx9_io")
    {
        return InventoryPaths{"", chassis};
    }
    return std::nullopt;
}

} // namespace pldm::em_device
