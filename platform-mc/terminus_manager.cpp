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
#include "terminus_manager.hpp"

#include "common/sleep.hpp"
#include "manager.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/exception.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <variant>

#ifdef OEM_NVIDIA
#include "oem/nvidia/common/em_device.hpp"
#endif

namespace pldm
{
namespace platform_mc
{

TerminusManager::TerminusManager(
    sdeventplus::Event& event, requester::Handler<requester::Request>& handler,
    InstanceIdDb& instanceIdDb,
    std::map<tid_t, std::shared_ptr<Terminus>>& termini, mctp_eid_t localEid,
    Manager* manager, bool numericSensorsWithoutAuxName) :
    numericSensorsWithoutAuxName(numericSensorsWithoutAuxName), event(event),
    handler(handler), instanceIdDb(instanceIdDb), termini(termini),
    localEid(localEid), tidPool(tidPoolSize, false), manager(manager)
{
    // DSP0240 v1.1.0 table-8, special value: 0,0xFF = reserved
    tidPool[0] = true;
    tidPool[PLDM_TID_RESERVED] = true;
}

std::optional<MctpInfo> TerminusManager::toMctpInfo(const tid_t& tid)
{
    if (transportLayerTable[tid] != SupportedTransportLayer::MCTP)
    {
        return std::nullopt;
    }

    auto it = mctpInfoTable.find(tid);
    if (it == mctpInfoTable.end())
    {
        return std::nullopt;
    }

    return it->second;
}

std::optional<tid_t> TerminusManager::toTid(const MctpInfo& mctpInfo)
{
    auto mctpInfoTableIterator = std::find_if(
        mctpInfoTable.begin(), mctpInfoTable.end(), [&mctpInfo](auto& v) {
            return (std::get<0>(v.second) == std::get<0>(mctpInfo)) &&
                   (std::get<2>(v.second) == std::get<2>(mctpInfo));
        });
    if (mctpInfoTableIterator == mctpInfoTable.end())
    {
        return std::nullopt;
    }
    return mctpInfoTableIterator->first;
}

std::optional<tid_t> TerminusManager::mapTid(const MctpInfo& mctpInfo,
                                             tid_t tid)
{
    if (tidPool[tid])
    {
        return std::nullopt;
    }

    tidPool[tid] = true;
    transportLayerTable[tid] = SupportedTransportLayer::MCTP;
    mctpInfoTable[tid] = mctpInfo;

    return tid;
}

/**
 * @brief MCTP Medium Type priority table ordering by bandwidth
 */
using Priority = int;
static std::unordered_map<MctpMedium, Priority> mediumPriority = {
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.PCIe", 0},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.USB", 1},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SPI", 2},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.I3C", 3},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.KCS", 4},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.Serial", 5},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus", 6}};

/**
 * @brief MCTP Binding Type priority table ordering by bandwidth
 */
static std::unordered_map<MctpBinding, Priority> bindingPriority = {
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.PCIe", 0},
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.USB", 1},
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.SPI", 2},
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.KCS", 3},
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.Serial", 4},
    {"xyz.openbmc_project.MCTP.Binding.BindingTypes.SMBus", 5}};

/** @brief Look up the priority for a medium/binding string.
 *
 *  @param[in] table - priority table to query
 *  @param[in] key   - medium or binding string sourced from D-Bus
 *
 *  @return The priority value for the key, or
 *          std::numeric_limits<Priority>::max() when the key is not
 *          present in the table. Strings absent from the priority
 *          maps therefore rank as lowest priority.
 */
static Priority lookupPriority(
    const std::unordered_map<std::string, Priority>& table,
    const std::string& key)
{
    auto it = table.find(key);
    return it != table.end() ? it->second
                             : std::numeric_limits<Priority>::max();
}

static bool isPreferred(const MctpInfo& currentMctpInfo,
                        const MctpInfo& newMctpInfo)
{
    auto currentMediumPrio =
        lookupPriority(mediumPriority, std::get<2>(currentMctpInfo));
    auto newMediumPrio =
        lookupPriority(mediumPriority, std::get<2>(newMctpInfo));

    if (currentMediumPrio == newMediumPrio)
    {
        return lookupPriority(bindingPriority, std::get<5>(currentMctpInfo)) >
               lookupPriority(bindingPriority, std::get<5>(newMctpInfo));
    }
    return currentMediumPrio > newMediumPrio;
}

std::optional<tid_t> TerminusManager::mapTid(const MctpInfo& mctpInfo)
{
    // skip reserved EID
    if (std::get<0>(mctpInfo) == 0 || std::get<0>(mctpInfo) == 0xff)
    {
        lg2::error("unable to assign a TID to reserved eid={EID}.", "EID",
                   std::get<0>(mctpInfo));
        return std::nullopt;
    }

    // check if the mctpInfo has mapped before
    auto mctpInfoTableIterator = std::find_if(
        mctpInfoTable.begin(), mctpInfoTable.end(), [&mctpInfo](auto& v) {
            return (std::get<0>(v.second) == std::get<0>(mctpInfo)) &&
                   (std::get<1>(v.second) == std::get<1>(mctpInfo)) &&
                   (std::get<2>(v.second) == std::get<2>(mctpInfo)) &&
                   (std::get<3>(v.second) == std::get<3>(mctpInfo)) &&
                   (std::get<5>(v.second) == std::get<5>(mctpInfo));
        });
    if (mctpInfoTableIterator != mctpInfoTable.end())
    {
        return mctpInfoTableIterator->first;
    }

    // check if the same UUID has been mapped to TID before
    mctpInfoTableIterator = std::find_if(
        mctpInfoTable.begin(), mctpInfoTable.end(), [&mctpInfo](const auto& v) {
            return (std::get<1>(v.second) == std::get<1>(mctpInfo));
        });
    if (mctpInfoTableIterator != mctpInfoTable.end())
    {
        // check if new medium type is preferred than original
        auto& currentMctpInfo = mctpInfoTableIterator->second;
        auto tid = mctpInfoTableIterator->first;
        if (!isPreferred(currentMctpInfo, mctpInfo))
        {
            return std::nullopt;
        }
        lg2::info(
            "Reassign the terminus TID={TID} to preferred medium eid={EID}.",
            "TID", tid, "EID", std::get<0>(mctpInfo));
        tidPool[tid] = false;
        return mapTid(mctpInfo, tid);
    }

    // directly assigning TID to the value of EID
    tid_t tid = std::get<0>(mctpInfo);
    if (tidPool[tid])
    {
        // cannot find a free tid to assign
        lg2::error("failed to assign a TID to Terminus eid={EID}.", "EID",
                   std::get<0>(mctpInfo));
        return std::nullopt;
    }

    return mapTid(mctpInfo, tid);
}

void TerminusManager::unmapTid(const tid_t& tid)
{
    if (tid == 0 || tid == PLDM_TID_RESERVED)
    {
        return;
    }
    tidPool[tid] = false;

    auto transportLayerTableIterator = transportLayerTable.find(tid);
    if (transportLayerTableIterator != transportLayerTable.end())
    {
        transportLayerTable.erase(transportLayerTableIterator);
    }

    auto mctpInfoTableIterator = mctpInfoTable.find(tid);
    if (mctpInfoTableIterator != mctpInfoTable.end())
    {
        mctpInfoTable.erase(mctpInfoTableIterator);
    }
}

namespace
{

using NameToEids = std::map<std::string, std::vector<mctp_eid_t>>;

/** @brief A terminus config read from EM, and where it came from. */
struct ResolvedConfig
{
    TerminusConfigEntry config;
    std::string source;
};
using ResolvedConfigs = std::map<mctp_eid_t, ResolvedConfig>;

/** @brief Set @p config of @p source for @p eids, replacing what an earlier
 *         source of the same EM read set. */
void resolveConfig(ResolvedConfigs& resolved,
                   const std::vector<mctp_eid_t>& eids,
                   const TerminusConfigEntry& config, const std::string& source)
{
    for (auto eid : eids)
    {
        auto [it, inserted] =
            resolved.try_emplace(eid, ResolvedConfig{config, source});
        if (inserted)
        {
            continue;
        }
        if (it->second.config != config)
        {
            lg2::info(
                "{SOURCE} replaces the terminus config from {OLD_SOURCE}, EID={EID}",
                "SOURCE", source, "OLD_SOURCE", it->second.source, "EID", eid);
        }
        it->second = ResolvedConfig{config, source};
    }
}

#ifdef OEM_NVIDIA
/** @brief The terminus configs of the CPU devices of the platform topology. */
void readNvDeviceConfigs(const NameToEids& nameToEids,
                         const pldm::utils::DBusHandlerInterface& dbusHandler,
                         ResolvedConfigs& resolved)
{
    const auto prefix = pldm::em_device::platformPrefix(dbusHandler);
    for (const auto& dev : pldm::em_device::devices(dbusHandler))
    {
        auto eidIt = nameToEids.find(dev.name);
        auto config = pldm::em_device::terminusConfig(dev, prefix);
        if (eidIt == nameToEids.end() || !config || config->name.empty())
        {
            continue;
        }
        resolveConfig(resolved, eidIt->second,
                      {config->instance, config->name, config->cpuIndex},
                      "NvDevice " + dev.name);
    }
}
#endif

/** @brief The terminus configs of the PLDM device records that name a
 *         configured endpoint as their MCTPTargetName. */
void readPldmDeviceRecords(const NameToEids& nameToEids,
                           const pldm::utils::DBusHandlerInterface& dbusHandler,
                           ResolvedConfigs& resolved)
{
    constexpr std::array pldmRecordIntfs{
        "xyz.openbmc_project.Configuration.PLDMFirmwareDevice",
        "xyz.openbmc_project.Configuration.PLDMDevice"};
    constexpr uint64_t maxUint16 = std::numeric_limits<uint16_t>::max();

    pldm::utils::GetSubTreeResponse subtree;
    try
    {
        subtree = dbusHandler.getSubtree(
            "/xyz/openbmc_project/inventory", 0,
            {pldmRecordIntfs.begin(), pldmRecordIntfs.end()});
    }
    catch (const std::exception&)
    {
        // No PLDM device records published.
        return;
    }

    for (const auto& [objPath, serviceMap] : subtree)
    {
        if (serviceMap.empty())
        {
            continue;
        }
        const auto& [service, intfs] = *serviceMap.begin();
        auto intf =
            std::find_first_of(intfs.begin(), intfs.end(),
                               pldmRecordIntfs.begin(), pldmRecordIntfs.end());
        if (intf == intfs.end())
        {
            continue;
        }

        pldm::utils::PropertyMap props;
        try
        {
            props = dbusHandler.getDbusPropertiesVariant(
                service.c_str(), objPath.c_str(), intf->c_str());
        }
        catch (const std::exception& e)
        {
            lg2::warning(
                "Failed to read PLDM device record, PATH={PATH}, ERROR={ERROR}",
                "PATH", objPath, "ERROR", e);
            continue;
        }

        const auto* terminusName =
            props.contains("TerminusName")
                ? std::get_if<std::string>(&props.at("TerminusName"))
                : nullptr;
        if (terminusName == nullptr)
        {
            continue;
        }
        const auto* targetName =
            props.contains("MCTPTargetName")
                ? std::get_if<std::string>(&props.at("MCTPTargetName"))
                : nullptr;
        if (targetName == nullptr)
        {
            continue;
        }
        auto eidIt = nameToEids.find(*targetName);
        if (eidIt == nameToEids.end())
        {
            continue;
        }

        // both are uint16_t on the terminus; absent means 0
        auto inst =
            props.contains("TerminusInstance")
                ? pldm::utils::numberProp(props, "TerminusInstance", maxUint16)
                : std::optional<uint64_t>{0};
        auto cpuIdx =
            props.contains("CpuIndex")
                ? pldm::utils::numberProp(props, "CpuIndex", maxUint16)
                : std::optional<uint64_t>{0};
        if (terminusName->empty() || !inst || !cpuIdx)
        {
            lg2::warning(
                "Invalid terminus configuration in PLDM device record, skipping. PATH={PATH}, TARGET={TARGET}",
                "PATH", objPath, "TARGET", *targetName);
            continue;
        }

        resolveConfig(resolved, eidIt->second,
                      {static_cast<int>(*inst), *terminusName,
                       static_cast<uint16_t>(*cpuIdx)},
                      "PLDM device record");
    }
}

/** @brief Set the instance, terminus name and CPU index of @p config on
 *         @p terminus. */
void setTerminusConfig(Terminus& terminus, const TerminusConfigEntry& config)
{
    const auto& [inst, terminusName, cpuIndex] = config;
    terminus.setInstance(inst);
    terminus.setTerminusName(terminusName);
    if (cpuIndex.has_value())
    {
        terminus.setCpuIndex(*cpuIndex);
    }
}

/** @brief Whether @p terminus already has @p config. Without OEM_NVIDIA
 *         the terminus keeps no instance, so it is not compared. */
bool hasTerminusConfig(Terminus& terminus, const TerminusConfigEntry& config)
{
    const auto& [inst, terminusName, cpuIndex] = config;
    if (terminus.getTerminusName().value_or("") != terminusName)
    {
        return false;
    }
    if (cpuIndex.has_value() && terminus.getCpuIndex() != cpuIndex)
    {
        return false;
    }
#ifdef OEM_NVIDIA
    if (terminus.getInstance() != static_cast<uint16_t>(inst))
    {
        return false;
    }
#else
    std::ignore = inst;
#endif
    return true;
}

} // namespace

void TerminusManager::loadTerminusConfigFromEm(
    const Configurations& configurations,
    const pldm::utils::DBusHandlerInterface& dbusHandler, bool force)
{
    // A device reachable over more than one medium has one endpoint per
    // medium under the same name; all of them get the config. Each named
    // endpoint is read once, whether or not it ends up with a terminus:
    // a record added later is read through reloadTerminusConfig().
    NameToEids nameToEids;
    std::map<mctp_eid_t, std::vector<MctpInfo>> eidToEndpoints;
    std::set<std::pair<std::string, mctp_eid_t>> reading;
    for (const auto& [path, mctpInfo] : configurations)
    {
        const auto& name = std::get<4>(mctpInfo);
        if (!name.has_value() || name->empty())
        {
            continue;
        }
        const auto eid = std::get<0>(mctpInfo);
        std::pair<std::string, mctp_eid_t> endpoint{*name, eid};
        if (!force && emConfigEndpointsRead.contains(endpoint))
        {
            continue;
        }
        if (reading.insert(std::move(endpoint)).second)
        {
            nameToEids[*name].push_back(eid);
        }
        eidToEndpoints[eid].push_back(mctpInfo);
    }
    if (nameToEids.empty())
    {
        return;
    }

    // From the weakest source to the strongest: the NvDevice-derived config,
    // then the PLDM device record.
    ResolvedConfigs resolved;
#ifdef OEM_NVIDIA
    readNvDeviceConfigs(nameToEids, dbusHandler, resolved);
#endif
    readPldmDeviceRecords(nameToEids, dbusHandler, resolved);

    for (const auto& [eid, entry] : resolved)
    {
        storeTerminusConfig(eid, entry.config, entry.source,
                            eidToEndpoints[eid]);
    }
    emConfigEndpointsRead.merge(reading);
}

void TerminusManager::storeTerminusConfig(
    mctp_eid_t eid, const TerminusConfigEntry& config,
    const std::string& source, const std::vector<MctpInfo>& endpoints)
{
    const auto& [inst, terminusName, cpuIdx] = config;
    auto existing = eidToTerminusConfigMap.find(eid);
    if (existing == eidToTerminusConfigMap.end() || existing->second != config)
    {
        eidToTerminusConfigMap.insert_or_assign(eid, config);
        lg2::info(
            "Loaded terminus config from {SOURCE}: EID={EID}, INSTANCE={INSTANCE}, TERMINUS_NAME={TERMINUS_NAME}, CPU_INDEX={CPU_INDEX}",
            "SOURCE", source, "EID", eid, "INSTANCE", inst, "TERMINUS_NAME",
            terminusName, "CPU_INDEX", cpuIdx.value_or(0));
    }

    // A terminus created before the config arrived: applyTerminusConfig()
    // has run for it already.
    for (const auto& mctpInfo : endpoints)
    {
        auto tid = toTid(mctpInfo);
        auto it = tid ? termini.find(*tid) : termini.end();
        if (it == termini.end() || !it->second ||
            hasTerminusConfig(*it->second, config))
        {
            continue;
        }
        if (!it->second->initialized)
        {
            setTerminusConfig(*it->second, config);
            lg2::info(
                "Applied the terminus config from {SOURCE} to the terminus being initialized: TID={TID}, EID={EID}, TERMINUS_NAME={TERMINUS_NAME}",
                "SOURCE", source, "TID", *tid, "EID", eid, "TERMINUS_NAME",
                terminusName);
            continue;
        }
        lg2::warning(
            "The terminus config from {SOURCE} changed after the terminus was initialized; it takes effect at the next pldmd start: TID={TID}, EID={EID}, TERMINUS_NAME={TERMINUS_NAME}",
            "SOURCE", source, "TID", *tid, "EID", eid, "TERMINUS_NAME",
            terminusName);
    }
}

void TerminusManager::reloadTerminusConfig(
    tid_t tid, const pldm::utils::DBusHandlerInterface& dbusHandler)
{
    auto mctpInfo = toMctpInfo(tid);
    if (!mctpInfo)
    {
        lg2::debug(
            "No MCTP endpoint to reload the terminus config of TID={TID}",
            "TID", tid);
        return;
    }
    const auto& name = std::get<4>(*mctpInfo);
    if (!name.has_value() || name->empty())
    {
        lg2::debug(
            "The endpoint of TID={TID} has no name to reload the terminus config by",
            "TID", tid);
        return;
    }
    auto terminus = termini.find(tid);
    if (terminus != termini.end() && terminus->second &&
        terminus->second->initialized)
    {
        lg2::debug(
            "TID={TID} is initialized; its terminus config is not reloaded",
            "TID", tid);
        return;
    }
    // The key only names the entry. The endpoint was read before; the
    // record that prompts the reload was not there then.
    loadTerminusConfigFromEm({{"/reload/" + *name, *mctpInfo}}, dbusHandler,
                             /*force=*/true);
}

void TerminusManager::discoverMctpTerminus(const MctpInfos& mctpInfos)
{
    queuedMctpInfos.emplace(mctpInfos);
    if (discoverMctpTerminusTaskHandle.has_value())
    {
        auto& [scope, rcOpt] = *discoverMctpTerminusTaskHandle;
        if (!rcOpt.has_value())
        {
            lg2::error("Terminus discovery already in progress.");
            return;
        }
        stdexec::sync_wait(scope.on_empty());
        discoverMctpTerminusTaskHandle.reset();
    }
    auto& [scope, rcOpt] = discoverMctpTerminusTaskHandle.emplace();
    scope.spawn(discoverMctpTerminusTask() |
                    stdexec::then([&](int rc) { rcOpt.emplace(rc); }),
                stdexec::inline_scheduler{});
}

exec::task<int> TerminusManager::discoverMctpTerminusTask()
{
    if (manager)
    {
        manager->stopSensorPolling();
    }

    while (!queuedMctpInfos.empty())
    {
        if (manager)
        {
            co_await manager->beforeDiscoverTerminus();
        }

        const MctpInfos& mctpInfos = queuedMctpInfos.front();
        for (auto& mctpInfo : mctpInfos)
        {
            co_await initMctpTerminus(mctpInfo);
        }

        if (manager)
        {
            co_await manager->afterDiscoverTerminus();
        }

        queuedMctpInfos.pop();
    }

    if (manager)
    {
        manager->startSensorPolling();
    }

    co_return PLDM_SUCCESS;
}

exec::task<int> TerminusManager::initMctpTerminus(const MctpInfo& mctpInfo)
{
    mctp_eid_t eid = std::get<0>(mctpInfo);
    auto discoveredLocalEid = std::get<6>(mctpInfo);
    if (discoveredLocalEid.has_value())
    {
        auto newEid = discoveredLocalEid.value();
        if (newEid != 0)
        {
            localEid = newEid;
            lg2::info("Updated localEid from D-Bus LocalEID property: "
                      "localEid={LEID}, remoteEid={EID}",
                      "LEID", localEid, "EID", eid);
        }
        else
        {
            lg2::warning(
                "Ignoring invalid LocalEID=0 from D-Bus for remoteEid={EID}, "
                "keeping localEid={LEID}",
                "EID", eid, "LEID", localEid);
        }
    }
    tid_t tid = 0;
    auto rc = co_await getTidOverMctpWithRetry(eid, tid);
    // tid == 0 per DSP0240 means the
    // remote terminus has not been assigned a TID yet
    // Treat it the same as PLDM_TID_RESERVED so that
    // we can assign a fresh TID via mapTid(mctpInfo)
    if (rc || !tid || tid == PLDM_TID_RESERVED)
    {
        // Assigning a tid. If it has been mapped, mapTid()
        // returns the tid assigned before.
        auto mappedTid = mapTid(mctpInfo);
        if (!mappedTid)
        {
            lg2::error("Failed to store Terminus Info for terminus {TID}.",
                       "TID", tid);
            co_return PLDM_ERROR;
        }

        tid = mappedTid.value();
        rc = co_await setTidOverMctpWithRetry(eid, tid);
        if (rc != PLDM_SUCCESS)
        {
            if (rc == PLDM_ERROR_UNSUPPORTED_PLDM_CMD)
            {
                lg2::error("Terminus {TID} does not support SetTID command.",
                           "TID", tid);
            }
            else
            {
                lg2::error(
                    "Failed to Set terminus TID for terminus {TID}, error {ERROR}.",
                    "TID", tid, "ERROR", rc);
            }
            unmapTid(tid);
            co_return rc;
        }
    }
    else
    {
        auto mappedTid = mapTid(mctpInfo, tid);
        if (!mappedTid)
        {
            auto existingTid = toTid(mctpInfo);
            if (!existingTid || existingTid.value() != tid)
            {
                lg2::error("Failed to store Terminus Info for terminus {TID}.",
                           "TID", tid);
                co_return PLDM_ERROR;
            }
        }
    }

    auto terminus = termini.find(tid);
    if (terminus != termini.end())
    {
        // The terminus has been discovered before. Refresh its capabilities
        // regardless of whether this discovery assigned or reused its TID.
        // On failure the cached types stay in use.
        co_await refreshTerminusSupportedTypes(terminus->second, eid);
        co_return PLDM_SUCCESS;
    }

    if (rc || tid == PLDM_TID_RESERVED)
    {
        // Assigning a tid. If it has been mapped, mapTid() returns the tid
        // assigned before.
        auto mappedTid = mapTid(mctpInfo);
        if (!mappedTid)
        {
            co_return PLDM_ERROR;
        }

        tid = mappedTid.value();
        rc = co_await setTidOverMctpWithRetry(eid, tid);
        if (rc != PLDM_SUCCESS && rc != PLDM_ERROR_UNSUPPORTED_PLDM_CMD)
        {
            unmapTid(tid);
            lg2::info("setTidOverMctp failed, eid={EID} tid={TID} rc={RC}.",
                      "EID", eid, "TID", tid, "RC", rc);
            co_return rc;
        }
    }

    uint64_t supportedTypes = 0;
    rc = co_await getPLDMTypes(tid, supportedTypes);
    if (rc)
    {
        lg2::error("getPLDMTypes failed, TID={TID} rc={RC}.", "TID", tid, "RC",
                   rc);
        co_return PLDM_ERROR;
    }

    UUID uuid = std::get<1>(mctpInfo);
    if (supportedTypes & (1 << PLDM_PLATFORM))
    {
        rc = co_await getTerminusUID(tid, uuid);
        if (rc)
        {
            lg2::info("getTerminusUID failed, TID={TID} rc={RC}.", "TID", tid,
                      "RC", rc);
        }
    }

    termini[tid] = std::make_shared<Terminus>(tid, supportedTypes, uuid, *this);

    applyTerminusConfig(tid, eid);

    co_return PLDM_SUCCESS;
}

void TerminusManager::applyTerminusConfig(tid_t tid, mctp_eid_t eid)
{
    // Runtime check: Match EID with configured EID and set terminus config
    auto eidConfigIt = eidToTerminusConfigMap.find(eid);
    if (eidConfigIt == eidToTerminusConfigMap.end())
    {
        lg2::info("No configured terminus name found for TID={TID}, EID={EID}",
                  "TID", tid, "EID", eid);
        return;
    }

    setTerminusConfig(*termini[tid], eidConfigIt->second);
    const auto& configuredTerminusName = std::get<1>(eidConfigIt->second);
    lg2::info(
        "Matched EID with configuration and set terminus config: TID={TID}, EID={EID}, TERMINUS_NAME={TERMINUS_NAME}",
        "TID", tid, "EID", eid, "TERMINUS_NAME", configuredTerminusName);
}

exec::task<void> TerminusManager::refreshTerminusSupportedTypes(
    std::shared_ptr<Terminus> terminus, mctp_eid_t eid)
{
    const auto tid = terminus->getTid();
    uint64_t supportedTypes = 0;
    auto rc = co_await getPLDMTypes(tid, supportedTypes);
    if (rc)
    {
        lg2::error(
            "GetPLDMTypes to known terminus TID={TID} EID={EID} failed, rc={RC}",
            "TID", tid, "EID", eid, "RC", rc);
        co_return;
    }

    const auto changedTypes = terminus->updateSupportedTypes(supportedTypes);
    if (!(changedTypes & (uint64_t{1} << PLDM_PLATFORM)))
    {
        co_return;
    }
    if (!terminus->doesSupport(PLDM_PLATFORM))
    {
        lg2::info(
            "Terminus no longer supports PLDM T2, TID={TID} EID={EID} TYPES={TYPES}",
            "TID", tid, "EID", eid, "TYPES", lg2::hex, supportedTypes);
        terminus->stopPolling = true;
        terminus->setOffline();
        co_return;
    }
    lg2::info(
        "Terminus now supports PLDM T2, TID={TID} EID={EID} TYPES={TYPES}",
        "TID", tid, "EID", eid, "TYPES", lg2::hex, supportedTypes);
    if (!terminus->hasEntities())
    {
        terminus->initialized = false;
        // Start like a new terminus; do not resume before initialization.
        terminus->resumed = true;
    }
    else
    {
        lg2::warning(
            "Reusing kept PLDM T2 objects; PDRs are not fetched again, TID={TID} EID={EID}",
            "TID", tid, "EID", eid);
    }
    terminus->initSensorList = true;
    terminus->setOnline();
}

exec::task<int> TerminusManager::SendRecvPldmMsgOverMctp(
    mctp_eid_t eid, Request& request, const pldm_msg** responseMsg,
    size_t* responseLen)
{
    int rc = 0;
    try
    {
        std::tie(rc, *responseMsg, *responseLen) =
            co_await handler.sendRecvMsg(eid, std::move(request));
    }
    catch (const sdbusplus::exception_t& e)
    {
        lg2::error(
            "Send and Receive PLDM message over MCTP throw error - {ERROR}.",
            "ERROR", e);
        co_return PLDM_ERROR;
    }
    catch (const int& e)
    {
        lg2::error(
            "Send and Receive PLDM message over MCTP throw int error - {ERROR}.",
            "ERROR", e);
        co_return PLDM_ERROR;
    }

    if (rc)
    {
        if (pldmErrorLogLimiter.shouldLog(eid))
        {
            pldmErrorLogLimiter.recordLog(eid);
            lg2::error(
                "sendRecvPldmMsgOverMctp failed. eid={EID} rc={RC} (further failures for this EID suppressed for 2 min)",
                "EID", eid, "RC", rc);
        }
    }
    else
    {
        pldmErrorLogLimiter.clear(eid);
    }
    co_return rc;
}

exec::task<int> TerminusManager::getTidOverMctpWithRetry(mctp_eid_t eid,
                                                         tid_t& tid)
{
    int rc = PLDM_ERROR;
    for (uint8_t attempt = 1; attempt <= mctpTidDiscoveryMaxAttempts; ++attempt)
    {
        rc = co_await getTidOverMctp(eid, tid);
        // A successful exchange - even one reporting an unassigned TID of 0 or
        // PLDM_TID_RESERVED - means the endpoint responded; do not retry.
        if (rc == PLDM_SUCCESS)
        {
            co_return rc;
        }
        if (attempt < mctpTidDiscoveryMaxAttempts)
        {
            lg2::info(
                "getTidOverMctp failed, retrying. eid={EID} attempt={ATTEMPT}/{MAX} rc={RC}",
                "EID", eid, "ATTEMPT", attempt, "MAX",
                mctpTidDiscoveryMaxAttempts, "RC", rc);
            co_await timer::Sleep(event, mctpTidDiscoveryRetryDelayUsec,
                                  timer::Priority);
        }
    }
    co_return rc;
}

exec::task<int> TerminusManager::setTidOverMctpWithRetry(mctp_eid_t eid,
                                                         tid_t tid)
{
    int rc = PLDM_ERROR;
    for (uint8_t attempt = 1; attempt <= mctpTidDiscoveryMaxAttempts; ++attempt)
    {
        rc = co_await setTidOverMctp(eid, tid);
        // Success, or a definitive "command unsupported" answer, are both
        // terminal - only genuine comms failures are worth retrying.
        if (rc == PLDM_SUCCESS || rc == PLDM_ERROR_UNSUPPORTED_PLDM_CMD)
        {
            co_return rc;
        }
        if (attempt < mctpTidDiscoveryMaxAttempts)
        {
            lg2::info(
                "setTidOverMctp failed, retrying. eid={EID} tid={TID} attempt={ATTEMPT}/{MAX} rc={RC}",
                "EID", eid, "TID", tid, "ATTEMPT", attempt, "MAX",
                mctpTidDiscoveryMaxAttempts, "RC", rc);
            co_await timer::Sleep(event, mctpTidDiscoveryRetryDelayUsec,
                                  timer::Priority);
        }
    }
    co_return rc;
}

exec::task<int> TerminusManager::getTidOverMctp(mctp_eid_t eid, tid_t& tid)
{
    auto instanceIdResult = instanceIdDb.next(eid);
    if (!instanceIdResult)
    {
        co_return PLDM_ERROR;
    }
    auto instanceId = instanceIdResult.value();
    Request request(sizeof(pldm_msg_hdr));
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    auto rc = encode_get_tid_req(instanceId, requestMsg);
    if (rc)
    {
        instanceIdDb.free(eid, instanceId);
        lg2::error("encode_get_tid_req failed, eid={EID} rc={RC}", "EID", eid,
                   "RC", rc);
        co_return rc;
    }

    const pldm_msg* responseMsg = NULL;
    size_t responseLen = 0;
    rc = co_await SendRecvPldmMsgOverMctp(eid, request, &responseMsg,
                                          &responseLen);
    if (rc)
    {
        lg2::error("getTidOverMctp failed. eid={EID} rc={RC}", "EID", eid, "RC",
                   rc);
        co_return rc;
    }

    pldm_base_get_tid_resp resp{};
    rc = decode_pldm_base_get_tid_resp(responseMsg, responseLen, &resp);
    if (rc)
    {
        lg2::error(
            "Failed to decode response GetTID for Endpoint ID {EID}, error {RC} ",
            "EID", eid, "RC", rc);
        co_return rc;
    }

    if (resp.completion_code != PLDM_SUCCESS)
    {
        lg2::error("Error : GetTID for Endpoint ID {EID}, complete code {CC}.",
                   "EID", eid, "CC", resp.completion_code);
        co_return resp.completion_code;
    }

    tid = resp.tid;

    co_return resp.completion_code;
}

exec::task<int> TerminusManager::setTidOverMctp(mctp_eid_t eid, tid_t tid)
{
    auto instanceIdResult = instanceIdDb.next(eid);
    if (!instanceIdResult)
    {
        co_return PLDM_ERROR;
    }
    auto instanceId = instanceIdResult.value();
    Request request(sizeof(pldm_msg_hdr) + sizeof(pldm_set_tid_req));
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    auto rc = encode_set_tid_req(instanceId, tid, requestMsg);
    if (rc)
    {
        instanceIdDb.free(eid, instanceId);
        co_return rc;
    }

    const pldm_msg* responseMsg = NULL;
    size_t responseLen = 0;
    rc = co_await SendRecvPldmMsgOverMctp(eid, request, &responseMsg,
                                          &responseLen);
    if (rc)
    {
        lg2::error("setTidOverMctp failed. eid={EID} tid={TID} rc={RC}", "EID",
                   eid, "TID", tid, "RC", rc);
        co_return rc;
    }

    if (responseMsg == NULL || responseLen != PLDM_SET_TID_RESP_BYTES)
    {
        co_return PLDM_ERROR_INVALID_LENGTH;
    }

    co_return responseMsg->payload[0];
}

exec::task<int> TerminusManager::getPLDMTypes(tid_t tid,
                                              uint64_t& supportedTypes)
{
    Request request(sizeof(pldm_msg_hdr));
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    auto rc = encode_get_types_req(0, requestMsg);
    if (rc)
    {
        lg2::error(
            "Failed to encode request getPLDMTypes for terminus ID {TID}, error {RC} ",
            "TID", tid, "RC", rc);
        co_return rc;
    }

    const pldm_msg* responseMsg = nullptr;
    size_t responseLen = 0;

    rc = co_await SendRecvPldmMsg(tid, request, &responseMsg, &responseLen);
    if (rc)
    {
        lg2::error("Failed to send GetPLDMTypes for terminus {TID}, error {RC}",
                   "TID", tid, "RC", rc);
        co_return rc;
    }

    pldm_base_get_pldm_types_resp resp{};
    rc = decode_pldm_base_get_pldm_types_resp(responseMsg, responseLen, &resp);
    if (rc)
    {
        lg2::error(
            "Failed to decode response GetPLDMTypes for terminus ID {TID}, error {RC} ",
            "TID", tid, "RC", rc);
        co_return rc;
    }

    if (resp.completion_code != PLDM_SUCCESS)
    {
        lg2::error(
            "Error : GetPLDMTypes for terminus ID {TID}, complete code {CC}.",
            "TID", tid, "CC", resp.completion_code);
        co_return resp.completion_code;
    }

    ::memcpy(&supportedTypes, &resp.pldm_types, sizeof(resp.pldm_types));
    co_return resp.completion_code;
}

exec::task<int> TerminusManager::getTerminusUID(tid_t tid, UUID& uuid)
{
    Request request(sizeof(pldm_msg_hdr));
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    auto rc = encode_get_terminus_uid_req(0, requestMsg);
    if (rc)
    {
        lg2::error("encode_get_terminus_uid_req failed, tid={TID} rc={RC}.",
                   "TID", tid, "RC", rc);
        co_return rc;
    }

    const pldm_msg* responseMsg = NULL;
    size_t responseLen = 0;

    rc = co_await SendRecvPldmMsg(tid, request, &responseMsg, &responseLen);
    if (rc)
    {
        co_return rc;
    }

    uint8_t completionCode = 0;
    uint8_t buf[16];
    rc = decode_get_terminus_UID_resp(responseMsg, responseLen, &completionCode,
                                      buf);
    if (rc)
    {
        lg2::error("decode_get_terminus_UID_resp failed, tid={TID} rc={RC}.",
                   "TID", tid, "RC", rc);
        co_return rc;
    }

    if (completionCode == PLDM_SUCCESS)
    {
#define UUID_STR_LEN 36
        uuid.resize(UUID_STR_LEN + 1, 0);
        snprintf(
            uuid.data(), uuid.size(),
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
            buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14],
            buf[15]);
        uuid.resize(UUID_STR_LEN);
    }
    co_return completionCode;
}

exec::task<int> TerminusManager::SendRecvPldmMsg(tid_t tid, Request& request,
                                                 const pldm_msg** responseMsg,
                                                 size_t* responseLen)
{
    if (tidPool[tid] &&
        transportLayerTable[tid] == SupportedTransportLayer::MCTP)
    {
        auto mctpInfo = toMctpInfo(tid);
        if (!mctpInfo)
        {
            lg2::error("SendRecvPldmMsg: cannot find eid for tid:{TID}.", "TID",
                       tid);
            co_return PLDM_ERROR;
        }

        auto eid = std::get<0>(mctpInfo.value());
        auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
        auto instanceIdResult = instanceIdDb.next(eid);
        if (!instanceIdResult)
        {
            co_return PLDM_ERROR;
        }
        requestMsg->hdr.instance_id = instanceIdResult.value();
        auto rc = co_await SendRecvPldmMsgOverMctp(eid, request, responseMsg,
                                                   responseLen);
        co_return rc;
    }
    else
    {
        lg2::error("SendRecvPldmMsg: tid:{TID} not found.", "TID", tid);
        co_return PLDM_ERROR;
    }
}

std::shared_ptr<Terminus> TerminusManager::getTerminus(const UUID& uuid)
{
    for (auto& [tid, terminus] : termini)
    {
        if (terminus->getUuid() == uuid)
        {
            lg2::info("getTerminus: terminus found for uuid:{UUID}", "UUID",
                      uuid);
            return terminus;
        }
    }
    lg2::info("getTerminus: no terminus found for uuid:{UUID}", "UUID", uuid);
    return nullptr;
}

exec::task<int> TerminusManager::resumeTid(tid_t tid)
{
    auto mctpInfo = toMctpInfo(tid);
    if (!mctpInfo)
    {
        lg2::error("resumeTid: cannot find eid for tid:{TID}.", "TID", tid);
        co_return PLDM_ERROR;
    }

    auto eid = std::get<0>(mctpInfo.value());
    auto rc = co_await setTidOverMctp(eid, tid);
    co_return rc;
}

} // namespace platform_mc
} // namespace pldm
