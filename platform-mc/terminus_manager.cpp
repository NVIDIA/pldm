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

#include "manager.hpp"

#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/exception.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>

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

static bool isPreferred(const MctpInfo& currentMctpInfo,
                        const MctpInfo& newMctpInfo)
{
    auto currentMedium = std::get<2>(currentMctpInfo);
    auto newMedium = std::get<2>(newMctpInfo);
    auto currentBinding = std::get<5>(currentMctpInfo);
    auto newBinding = std::get<5>(newMctpInfo);

    if (mediumPriority.at(currentMedium) == mediumPriority.at(newMedium))
    {
        return bindingPriority.at(currentBinding) >
               bindingPriority.at(newBinding);
    }
    else
    {
        return mediumPriority.at(currentMedium) > mediumPriority.at(newMedium);
    }
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

void TerminusManager::loadStaticTerminusConfig(const std::string& configPath)
{
    namespace fs = std::filesystem;
    using Json = nlohmann::json;

    if (!fs::exists(configPath))
    {
        lg2::info(
            "Static PLDM terminus configuration file does not exist, PATH={PATH}",
            "PATH", configPath);
        return;
    }

    try
    {
        std::ifstream jsonFile(configPath);
        auto data = Json::parse(jsonFile, nullptr, false);
        if (data.is_discarded())
        {
            lg2::error(
                "Failed to parse static PLDM terminus configuration file, PATH={PATH}",
                "PATH", configPath);
            return;
        }

        const std::vector<Json> emptyJsonArray{};
        auto termini = data.value("PLDMTermini", emptyJsonArray);

        for (const auto& terminus : termini)
        {
            auto eid = terminus.value("EID", 0xFF);
            auto name = terminus.value("Name", std::string(""));
            auto terminusName = terminus.value("TerminusName", std::string(""));
            auto inst = terminus.value("Instance", 0);

            if (eid == 0xFF || name.empty() || inst < 0)
            {
                lg2::warning(
                    "Invalid terminus configuration entry, skipping. NAME={NAME}, EID={EID}",
                    "NAME", name, "EID", eid);
                continue;
            }

            lg2::info(
                "Loading static PLDM terminus config: NAME={NAME}, TERMINUS_NAME={TERMINUS_NAME}, EID={EID}",
                "NAME", name, "TERMINUS_NAME", terminusName, "EID", eid);

            // Store EID to TerminusName mapping for runtime check
            // This will be used when the actual MCTP endpoint is discovered
            if (!terminusName.empty())
            {
                eidToTerminusNameMap[eid] =
                    std::pair<int, std::string>(inst, terminusName);
                lg2::info(
                    "Stored EID to TerminusName mapping for future discovery: EID={EID},"
                    "INSTANCE={INSTANCE}, TERMINUS_NAME={TERMINUS_NAME}",
                    "EID", eid, "INSTANCE", inst, "TERMINUS_NAME",
                    terminusName);
            }
        }

        lg2::info(
            "Loaded {COUNT} static PLDM terminus name mappings from configuration",
            "COUNT", eidToTerminusNameMap.size());
    }
    catch (const std::exception& e)
    {
        lg2::error(
            "Exception while loading static PLDM terminus configuration: {ERROR}",
            "ERROR", e.what());
    }
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
    auto rc = co_await getTidOverMctp(eid, tid);
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
        rc = co_await setTidOverMctp(eid, tid);
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

        if (termini.contains(tid))
        {
            // the terminus has been discovered before
            co_return PLDM_SUCCESS;
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
                lg2::error(
                    "Failed to store Terminus Info for terminus {TID}.",
                    "TID", tid);
                co_return PLDM_ERROR;
            }
        }

        if (termini.contains(tid))
        {
            co_return PLDM_SUCCESS;
        }
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
        rc = co_await setTidOverMctp(eid, tid);
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

    // Runtime check: Match EID with configured EID and set terminus name
    auto eidMappingIt = eidToTerminusNameMap.find(eid);
    if (eidMappingIt != eidToTerminusNameMap.end())
    {
        auto inst = eidMappingIt->second.first;
        termini[tid]->setInstance(inst);
        const std::string& configuredTerminusName = eidMappingIt->second.second;
        termini[tid]->setTerminusName(configuredTerminusName);
        lg2::info(
            "Matched EID with configuration and set terminus name: TID={TID}, EID={EID}, TERMINUS_NAME={TERMINUS_NAME}",
            "TID", tid, "EID", eid, "TERMINUS_NAME", configuredTerminusName);
    }
    else
    {
        lg2::info("No configured terminus name found for TID={TID}, EID={EID}",
                  "TID", tid, "EID", eid);
    }

    co_return PLDM_SUCCESS;
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

    uint8_t completionCode = 0;
    rc = decode_get_tid_resp(responseMsg, responseLen, &completionCode, &tid);
    if (rc)
    {
        lg2::error("decode_get_tid_resp failed. eid={EID} rc={RC}", "EID", eid,
                   "RC", rc);
        co_return rc;
    }

    co_return completionCode;
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
        lg2::error("encode_get_types_req failed, tid={TID} rc={RC}.", "TID",
                   tid, "RC", rc);
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
    bitfield8_t* types = reinterpret_cast<bitfield8_t*>(&supportedTypes);
    rc =
        decode_get_types_resp(responseMsg, responseLen, &completionCode, types);
    if (rc)
    {
        lg2::error("decode_get_types_resp failed, tid={TID} rc={RC}.", "TID",
                   tid, "RC", rc);
        co_return rc;
    }
    co_return completionCode;
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
