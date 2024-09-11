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

#include <stdio.h>

namespace pldm
{
namespace platform_mc
{

TerminusManager::TerminusManager(
    sdeventplus::Event& event, requester::Handler<requester::Request>& handler,
    dbus_api::Requester& requester,
    std::map<tid_t, std::shared_ptr<Terminus>>& termini, mctp_eid_t localEid,
    Manager* manager, bool numericSensorsWithoutAuxName) :
    numericSensorsWithoutAuxName(numericSensorsWithoutAuxName),
    event(event), handler(handler), requester(requester), termini(termini),
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
                   (std::get<3>(v.second) == std::get<3>(mctpInfo));
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
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.KCS", 3},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.Serial", 4},
    {"xyz.openbmc_project.MCTP.Endpoint.MediaTypes.SMBus", 5}};

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
    auto currentBinding = std::get<4>(currentMctpInfo);
    auto newBinding = std::get<4>(newMctpInfo);

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
                   (std::get<4>(v.second) == std::get<4>(mctpInfo));
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

    auto tidPoolIterator = std::find(tidPool.begin(), tidPool.end(), false);
    if (tidPoolIterator == tidPool.end())
    {
        // cannot find a free tid to assign
        lg2::error("failed to assign a TID to Terminus eid={EID}.", "EID",
                   std::get<0>(mctpInfo));
        return std::nullopt;
    }

    tid_t tid = std::distance(tidPool.begin(), tidPoolIterator);
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

void TerminusManager::discoverMctpTerminus(const MctpInfos& mctpInfos)
{
    queuedMctpInfos.emplace(mctpInfos);
    if (discoverMctpTerminusTaskHandle)
    {
        if (discoverMctpTerminusTaskHandle.done())
        {
            discoverMctpTerminusTaskHandle.destroy();

            auto co = discoverMctpTerminusTask();
            discoverMctpTerminusTaskHandle = co.handle;
            if (discoverMctpTerminusTaskHandle.done())
            {
                discoverMctpTerminusTaskHandle = nullptr;
            }
        }
    }
    else
    {
        auto co = discoverMctpTerminusTask();
        discoverMctpTerminusTaskHandle = co.handle;
        if (discoverMctpTerminusTaskHandle.done())
        {
            discoverMctpTerminusTaskHandle = nullptr;
        }
    }
}

requester::Coroutine TerminusManager::discoverMctpTerminusTask()
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

requester::Coroutine TerminusManager::initMctpTerminus(const MctpInfo& mctpInfo)
{
    mctp_eid_t eid = std::get<0>(mctpInfo);
    tid_t tid = 0;
    auto rc = co_await getTidOverMctp(eid, tid);
    if (rc || tid == PLDM_TID_RESERVED)
    {
        lg2::error("getTidOverMctp failed, eid={EID} rc={RC}.", "EID", eid,
                   "RC", rc);
        co_return PLDM_ERROR;
    }

    // Assigning a tid. If it has been mapped, mapTid() returns the tid assigned
    // before.
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
        lg2::info("setTidOverMctp failed, eid={EID} tid={TID} rc={RC}.", "EID",
                  eid, "TID", tid, "RC", rc);
        co_return rc;
    }

    auto it = termini.find(tid);
    if (it != termini.end())
    {
        lg2::info("terminus tid={TID} eid={EID} has been initialized.", "TID",
                  tid, "EID", eid);
        co_return PLDM_SUCCESS;
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
    co_return PLDM_SUCCESS;
}

requester::Coroutine
    TerminusManager::SendRecvPldmMsgOverMctp(mctp_eid_t eid, Request& request,
                                             const pldm_msg** responseMsg,
                                             size_t* responseLen)
{
    auto rc = co_await requester::SendRecvPldmMsg<RequesterHandler>(
        handler, eid, request, responseMsg, responseLen);
    if (rc)
    {
        lg2::error("sendRecvPldmMsgOverMctp failed. eid={EID} rc={RC}", "EID",
                   eid, "RC", rc);
    }
    co_return rc;
}

requester::Coroutine TerminusManager::getTidOverMctp(mctp_eid_t eid, tid_t& tid)
{
    auto instanceId = requester.getInstanceId(eid);
    Request request(sizeof(pldm_msg_hdr));
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    auto rc = encode_get_tid_req(instanceId, requestMsg);
    if (rc)
    {
        requester.markFree(eid, instanceId);
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

requester::Coroutine TerminusManager::setTidOverMctp(mctp_eid_t eid, tid_t tid)
{
    auto instanceId = requester.getInstanceId(eid);
    Request request(sizeof(pldm_msg_hdr) + sizeof(pldm_set_tid_req));
    auto requestMsg = reinterpret_cast<pldm_msg*>(request.data());
    auto rc = encode_set_tid_req(instanceId, tid, requestMsg);
    if (rc)
    {
        requester.markFree(eid, instanceId);
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

requester::Coroutine TerminusManager::getPLDMTypes(tid_t tid,
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

requester::Coroutine TerminusManager::getTerminusUID(tid_t tid, UUID& uuid)
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

requester::Coroutine
    TerminusManager::SendRecvPldmMsg(tid_t tid, Request& request,
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
        requestMsg->hdr.instance_id = requester.getInstanceId(eid);
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

requester::Coroutine TerminusManager::resumeTid(tid_t tid)
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
